#!/usr/bin/env python3
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

"""armasm2gas -- translate RVCT/armasm (ADS) assembly to GNU as (GAS) syntax.

The Symbian ARM bootstrap (syborg.s + the generic
bootmain/bootcpu/bootutils.s + the bootcpu.inc macro/config web) is the one
piece of the from-source kernel that is hand-written RVCT armasm, and RVCT
(armasm) is the toolchain we do NOT have.  This tool rewrites it to GAS so
`arm-none-eabi-as` can assemble it.

DESIGN -- interpret, don't re-map.
  armasm's GBLL/SETL/IF/MACRO/INCLUDE are ASSEMBLE-TIME metaprogramming.  GAS's
  own .if/.ifdef/.macro are weaker and have a different defined-vs-value model
  (armasm distinguishes "declared" from "true"; :DEF: tests declaration, IF
  tests value).  Rather than fight that impedance mismatch, this translator is a
  mini armasm PREPROCESSOR: it evaluates the variable store, conditionals,
  macros and includes itself against a fixed target config, and emits FLAT,
  already-resolved GAS containing only the taken branches plus the real
  instructions and data directives.  Output is plain, debuggable GAS.

Scope is driven by the actual constructs in the bootstrap corpus (see
tools/armasm2gas/README.md), not the whole of armasm.

Usage:
    python3 armasm2gas.py <in.s|.inc> [-I dir ...] [-D SYM[=val] ...] \
        [--config config.inc] [-o out.S]
"""

import argparse
import os
import re
import sys


class AsmError(Exception):
    pass


# ---------------------------------------------------------------------------
# Lexing helpers
# ---------------------------------------------------------------------------

def strip_comment(line):
    """Remove an armasm ';' comment, respecting quotes and char constants."""
    out = []
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                out.append(line[i])
                if line[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "'":
            # char constant 'x' -- but ' can also be spurious; treat simple case
            out.append(c)
            i += 1
            if i < n:
                out.append(line[i])
                i += 1
            if i < n and line[i] == "'":
                out.append(line[i])
                i += 1
            continue
        if c == ';':
            break
        out.append(c)
        i += 1
    return ''.join(out).rstrip()


# ---------------------------------------------------------------------------
# Expression evaluator (armasm expression subset)
# ---------------------------------------------------------------------------

class ExprEval:
    """Evaluate armasm assemble-time expressions to Python ints/strings/bools.

    Supports: numeric literals (0x.., decimal, &hex, 2_bin, n_base, 'c'),
    strings, identifiers (looked up in vars/consts), and operators
    :LNOT: :NOT: :DEF: :LEN: (unary) and :LOR: :LAND: :OR: :AND: :EOR:
    :SHL: :SHR: + - * / :MOD: and comparisons = <> < > <= >=.
    """

    def __init__(self, env):
        self.env = env  # Environment

    def eval(self, text):
        self.toks = self._tokenize(text)
        self.pos = 0
        v = self._parse_or()
        if self.pos != len(self.toks):
            raise AsmError("trailing tokens in expr: %r (at %r)"
                           % (text, self.toks[self.pos:]))
        return v

    # -- tokenizer --
    _tok_re = re.compile(r"""
        \s+
      | (?P<op>:[A-Z]+:)
      | (?P<brace>\{[A-Za-z]+\})
      | (?P<hex>0[xX][0-9A-Fa-f]+ | &[0-9A-Fa-f]+)
      | (?P<num>\d+(?:_[0-9A-Za-z]+)?)
      | (?P<str>"(?:[^"]*)")
      | (?P<char>'.')
      | (?P<id>[A-Za-z_$][A-Za-z0-9_$]*)
      | (?P<cmp><=|>=|<>|<<|>>|[<>=])
      | (?P<punct>[()+\-*/|&^~])
    """, re.VERBOSE)

    def _tokenize(self, text):
        toks = []
        i = 0
        while i < len(text):
            m = self._tok_re.match(text, i)
            if not m:
                raise AsmError("cannot lex expr at %r" % text[i:])
            i = m.end()
            if m.lastgroup is None:  # whitespace
                continue
            toks.append((m.lastgroup, m.group()))
        return toks

    def _peek(self):
        return self.toks[self.pos] if self.pos < len(self.toks) else (None, None)

    def _next(self):
        t = self.toks[self.pos]
        self.pos += 1
        return t

    # precedence climbing, loosest first
    def _parse_or(self):
        v = self._parse_and()
        while self._peek()[1] in (':LOR:', ':OR:', '|', ':EOR:', '^'):
            op = self._next()[1]
            r = self._parse_and()
            if op == ':LOR:':
                v = _to_bool(v) or _to_bool(r)
            elif op in (':OR:', '|'):
                v = _to_int(v) | _to_int(r)
            elif op in (':EOR:', '^'):
                v = _to_int(v) ^ _to_int(r)
        return v

    def _parse_and(self):
        v = self._parse_cmp()
        while self._peek()[1] in (':LAND:', ':AND:', '&'):
            op = self._next()[1]
            r = self._parse_cmp()
            if op == ':LAND:':
                v = _to_bool(v) and _to_bool(r)
            else:
                v = _to_int(v) & _to_int(r)
        return v

    def _parse_cmp(self):
        v = self._parse_shift()
        while self._peek()[1] in ('=', '<>', '<', '>', '<=', '>='):
            op = self._next()[1]
            r = self._parse_shift()
            a, b = _to_int(v), _to_int(r)
            v = {'=': a == b, '<>': a != b, '<': a < b, '>': a > b,
                 '<=': a <= b, '>=': a >= b}[op]
        return v

    def _parse_shift(self):
        v = self._parse_add()
        while self._peek()[1] in (':SHL:', ':SHR:', '<<', '>>'):
            op = self._next()[1]
            r = self._parse_add()
            if op in (':SHL:', '<<'):
                v = _to_int(v) << _to_int(r)
            else:
                v = _to_int(v) >> _to_int(r)
        return v

    def _parse_add(self):
        v = self._parse_mul()
        while self._peek()[1] in ('+', '-'):
            op = self._next()[1]
            r = self._parse_mul()
            if op == '+':
                v = _add(v, r)
            else:
                v = _to_int(v) - _to_int(r)
        return v

    def _parse_mul(self):
        v = self._parse_unary()
        while self._peek()[1] in ('*', '/', ':MOD:'):
            op = self._next()[1]
            r = self._parse_unary()
            if op == '*':
                v = _to_int(v) * _to_int(r)
            elif op == '/':
                v = _to_int(v) // _to_int(r)
            else:
                v = _to_int(v) % _to_int(r)
        return v

    def _parse_unary(self):
        kind, tok = self._peek()
        if tok in (':LNOT:',):
            self._next()
            return not _to_bool(self._parse_unary())
        if tok in (':NOT:', '~'):
            self._next()
            return ~_to_int(self._parse_unary()) & 0xFFFFFFFF
        if tok == '-':
            self._next()
            return -_to_int(self._parse_unary())
        if tok == '+':
            self._next()
            return self._parse_unary()
        if tok == ':DEF:':
            self._next()
            _, name = self._next()
            return self.env.is_declared(name)
        if tok == ':LEN:':
            self._next()
            v = self._parse_unary()
            return len(v) if isinstance(v, str) else len(str(v))
        return self._parse_atom()

    def _parse_atom(self):
        kind, tok = self._next()
        if tok == '(':
            v = self._parse_or()
            assert self._next()[1] == ')'
            return v
        if kind == 'brace':
            return {'{TRUE}': True, '{FALSE}': False}.get(tok, tok)
        if kind == 'hex':
            return int(tok.lstrip('&').replace('0x', '').replace('0X', ''), 16)
        if kind == 'num':
            if '_' in tok:
                base, digits = tok.split('_', 1)
                return int(digits, int(base))
            return int(tok)
        if kind == 'str':
            return tok[1:-1]
        if kind == 'char':
            return ord(tok[1])
        if kind == 'id':
            return self.env.value_of(tok)
        raise AsmError("unexpected token %r in expr" % (tok,))


def _to_bool(v):
    if isinstance(v, bool):
        return v
    if isinstance(v, int):
        return v != 0
    if isinstance(v, str):
        return v != ''
    return bool(v)


def _to_int(v):
    if isinstance(v, bool):
        return 1 if v else 0
    if isinstance(v, int):
        return v
    raise AsmError("expected int, got %r" % (v,))


def _add(a, b):
    if isinstance(a, str) or isinstance(b, str):
        return str(a) + str(b)
    return _to_int(a) + _to_int(b)


# ---------------------------------------------------------------------------
# Environment: assemble-time variable / constant store
# ---------------------------------------------------------------------------

class Environment:
    def __init__(self):
        self.vars = {}      # name -> value (GBLL/GBLA/GBLS/LCL*)
        self.declared = set()
        self.consts = {}    # EQU names -> value (or None if symbolic)
        self.reg_alias = {}  # RN/CN aliases -> target register token
        self.cond_mode = False  # when True, unknown symbols evaluate to False

    def declare(self, name, kind):
        self.declared.add(name)
        if name not in self.vars:
            self.vars[name] = {'L': False, 'A': 0, 'S': ''}[kind]

    def set(self, name, value):
        self.vars[name] = value
        self.declared.add(name)

    def is_declared(self, name):
        return name in self.declared or name in self.consts

    def value_of(self, name):
        if name in self.vars:
            return self.vars[name]
        if name in self.consts and self.consts[name] is not None:
            return self.consts[name]
        if self.cond_mode:
            # In a conditional, an undeclared logical behaves as FALSE, matching
            # armasm's declared-but-unset semantics (bootcpu.inc GBLL-declares
            # every CFG_* flag; unused ones default FALSE).
            return False
        # Unknown identifier: for expression contexts this is usually an
        # external link-time symbol. Raise so callers can decide to keep it
        # symbolic rather than fold it.
        raise AsmError("unknown symbol in assemble-time expr: %s" % name)


# ---------------------------------------------------------------------------
# Macro model
# ---------------------------------------------------------------------------

class Macro:
    def __init__(self, name, label_param, params, defaults, body):
        self.name = name
        self.label_param = label_param  # e.g. '$lab' or None
        self.params = params            # ['$reg', ...]
        self.defaults = defaults        # {'$reg': 'r0'} (armasm '$p="default"')
        self.body = body                # list of raw body lines


def split_args(text):
    """Split a comma-separated arg list at top level (respect "..", (), {})."""
    args = []
    depth = 0
    cur = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == '"':
            cur.append(c)
            i += 1
            while i < len(text):
                cur.append(text[i])
                if text[i] == '"':
                    break
                i += 1
            i += 1
            continue
        if c in '({':
            depth += 1
        elif c in ')}':
            depth -= 1
        if c == ',' and depth == 0:
            args.append(''.join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    if cur or args:
        args.append(''.join(cur).strip())
    return args


_SUBST_RE = re.compile(r'\$([A-Za-z_][A-Za-z0-9_]*)\.?')


def subst_params(line, bindings):
    """Textually substitute $param (and trailing '.' concatenation)."""
    def repl(m):
        name = m.group(1)
        if name in bindings:
            return bindings[name]
        return m.group(0)  # leave unknown $x untouched
    return _SUBST_RE.sub(repl, line)


# ---------------------------------------------------------------------------
# Source stack: files and macro expansions feed one line stream
# ---------------------------------------------------------------------------

class Source:
    def __init__(self, lines, name):
        self.lines = lines
        self.name = name
        self.idx = 0

    def next(self):
        if self.idx >= len(self.lines):
            return None
        ln = self.lines[self.idx]
        self.idx += 1
        return ln


# ---------------------------------------------------------------------------
# Translator
# ---------------------------------------------------------------------------

# directives that take the leading col-1 label as their operand name
LABEL_CONSUMING = {'EQU', 'SETL', 'SETA', 'SETS', 'RN', 'CN', 'FN', '*'}
# data / directive words handled by the backend emitter
DATA_DIRECTIVES = {'DCD', 'DCDU', 'DCB', 'DCW', 'DCWU', 'DCQ', 'DCFS', 'DCFD',
                   'SPACE', '%', '='}


class Translator:
    def __init__(self, incdirs, env=None):
        self.incdirs = incdirs
        self.env = env or Environment()
        self.macros = {}
        self.out = []
        self.srcstack = []
        # conditional stack: list of dicts {active, taken, parent_active}
        self.cond = []
        self.ended = False

    # -- source management --
    def push_file(self, path):
        with open(path, 'r', errors='replace') as f:
            lines = f.read().splitlines()
        self.srcstack.append(Source(lines, path))

    def push_lines(self, lines, name):
        self.srcstack.append(Source(lines, name))

    def find_include(self, fname):
        cands = [fname] + [os.path.join(d, fname) for d in self.incdirs]
        for c in cands:
            if os.path.isfile(c):
                return c
        raise AsmError("include not found: %s (searched %s)"
                       % (fname, self.incdirs))

    def _active(self):
        return all(c['active'] for c in self.cond)

    # -- top-level run --
    def run(self, path):
        self.push_file(path)
        self.out.append('\t.syntax unified')
        self.out.append('\t@ ==== translated from %s by armasm2gas ====' %
                        os.path.basename(path))
        while self.srcstack and not self.ended:
            src = self.srcstack[-1]
            raw = src.next()
            if raw is None:
                self.srcstack.pop()
                continue
            self.process(raw)
        return '\n'.join(self.out) + '\n'

    # -- per-line processing --
    def process(self, raw):
        line = strip_comment(raw)
        if line.strip() == '':
            return
        label, op, operands = self.parse_stmt(line)
        opu = op.upper() if op else ''

        # conditional control is evaluated even when inactive (to track nesting)
        if opu in ('IF', 'ELSE', 'ELIF', 'ENDIF') or line.lstrip()[:1] in '[|]' \
                and self._is_bracket_cond(line):
            self.handle_conditional(label, opu, operands, line)
            return

        active = self._active()

        # while inactive, still need to track macro nesting so a MACRO in a
        # dead branch does not get defined and its MEND is not mis-handled.
        if not active:
            if opu == 'MACRO':
                self.skip_macro()
            return

        # macro definition
        if opu == 'MACRO':
            self.capture_macro()
            return

        # includes
        if opu in ('INCLUDE', 'GET'):
            self.push_file(self.find_include(operands.strip()))
            return

        if opu == 'END':
            # END ends the current source file only
            self.srcstack.pop() if self.srcstack else None
            return

        # variable / const directives (may consume the label)
        if opu in ('GBLL', 'GBLA', 'GBLS'):
            self.env.declare(operands.strip(), opu[3])
            return
        if opu in ('LCLL', 'LCLA', 'LCLS'):
            self.env.declare(operands.strip(), opu[3])
            return
        if opu in ('SETL', 'SETA', 'SETS'):
            self.do_set(label, opu, operands)
            return
        if opu == 'EQU':
            self.do_equ(label, operands)
            return
        if opu in ('RN', 'CN', 'FN'):
            self.env.reg_alias[label] = self._alias_target(opu, operands)
            return

        # armasm diagnostic directive:  ! expr, "message"  (assemble-time warn/error).
        # If it is reached with an active branch it signals a real assembly condition;
        # we surface it as a comment so it never becomes junk in the GAS output.
        if op == '!' or (label == '!' and not op):
            self.out.append('\t@ armasm-diagnostic: %s' % operands.strip())
            return

        # macro invocation?
        if op and op in self.macros:
            self.expand_macro(self.macros[op], label, operands)
            return
        if label and label in self.macros:
            # label field is actually the macro name (no leading label param)
            self.expand_macro(self.macros[label], None, (op + ' ' + operands).strip())
            return

        # otherwise: real code/data line
        self.emit_code(label, op, operands, opu)

    # -- statement parse --
    def parse_stmt(self, line):
        if line[:1] and not line[0].isspace():
            parts = line.split(None, 1)
            label = parts[0]
            rest = parts[1] if len(parts) > 1 else ''
        else:
            label = None
            rest = line.strip()
        if rest:
            rp = rest.split(None, 1)
            op = rp[0]
            operands = rp[1] if len(rp) > 1 else ''
        else:
            op = ''
            operands = ''
        return label, op, operands

    # -- conditionals --
    def _is_bracket_cond(self, line):
        s = line.lstrip()
        if not s:
            return False
        # bracket-form: line begins with [ (IF), or is exactly | or ]
        if s[0] == '[':
            return True
        if s in ('|', ']'):
            return True
        return False

    def handle_conditional(self, label, opu, operands, line):
        s = line.strip()
        # normalise bracket form to IF/ELSE/ENDIF
        if s[0] == '[' and opu not in ('IF',):
            opu = 'IF'
            operands = s[1:].strip()
        elif s == '|':
            opu = 'ELSE'
        elif s == ']':
            opu = 'ENDIF'

        if opu == 'IF':
            parent = self._active()
            val = False
            if parent:
                val = _to_bool(self.safe_eval(operands, cond=True))
            self.cond.append({'active': parent and val,
                              'taken': val,
                              'parent': parent})
        elif opu == 'ELIF':
            c = self.cond[-1]
            if c['parent'] and not c['taken']:
                val = _to_bool(self.safe_eval(operands, cond=True))
                c['active'] = val
                c['taken'] = c['taken'] or val
            else:
                c['active'] = False
        elif opu == 'ELSE':
            c = self.cond[-1]
            c['active'] = c['parent'] and not c['taken']
            c['taken'] = True
        elif opu == 'ENDIF':
            self.cond.pop()

    def safe_eval(self, text, cond=False):
        self.env.cond_mode = cond
        try:
            return ExprEval(self.env).eval(text)
        except AsmError as e:
            raise AsmError("eval failed for %r: %s" % (text, e))
        finally:
            self.env.cond_mode = False

    # -- macro capture / skip / expand --
    def _read_until_mend(self):
        body = []
        depth = 1
        while self.srcstack:
            raw = self.srcstack[-1].next()
            if raw is None:
                raise AsmError("EOF inside MACRO")
            stripped = strip_comment(raw).strip().upper()
            if stripped == 'MACRO':
                depth += 1
            elif stripped == 'MEND':
                depth -= 1
                if depth == 0:
                    break
            body.append(raw)
        return body

    def skip_macro(self):
        self._read_until_mend()

    def capture_macro(self):
        # next line is the prototype
        proto_raw = None
        while self.srcstack:
            proto_raw = self.srcstack[-1].next()
            if proto_raw is None:
                self.srcstack.pop()
                continue
            if strip_comment(proto_raw).strip() != '':
                break
        proto = strip_comment(proto_raw)
        label_param = None
        if proto[:1] and not proto[0].isspace():
            # leading label param present
            parts = proto.split(None, 1)
            label_param = parts[0]
            proto = parts[1] if len(parts) > 1 else ''
        proto = proto.strip()
        pp = proto.split(None, 1)
        name = pp[0]
        arglist = pp[1] if len(pp) > 1 else ''
        params = []
        defaults = {}
        for a in split_args(arglist):
            if not a:
                continue
            if '=' in a:
                pn, dv = a.split('=', 1)
                params.append(pn.strip())
                defaults[pn.strip()] = dv.strip().strip('"')
            else:
                params.append(a.strip())
        body = self._read_until_mend()
        self.macros[name] = Macro(name, label_param, params, defaults, body)

    def expand_macro(self, mac, label, operands):
        args = split_args(operands) if operands.strip() else []
        bindings = {}
        if mac.label_param:
            bindings[mac.label_param.lstrip('$')] = label or ''
        for i, p in enumerate(mac.params):
            pn = p.lstrip('$')
            if i < len(args) and args[i] != '':
                # armasm strips the delimiting quotes from a double-quoted macro
                # argument, so `PRTLN "hi"` + body `= "$text",0` -> `= "hi",0`
                # (not the doubled-quote `= ""hi"",0`). Only when fully wrapped.
                a = args[i]
                if len(a) >= 2 and a[0] == '"' and a[-1] == '"' and a.count('"') == 2:
                    a = a[1:-1]
                bindings[pn] = a
            elif p in mac.defaults:
                bindings[pn] = mac.defaults[p]
            else:
                bindings[pn] = ''
        expanded = [subst_params(l, bindings) for l in mac.body]
        self.push_lines(expanded, 'macro:%s' % mac.name)

    # -- variable set / equ --
    def do_set(self, name, opu, operands):
        val = self.safe_eval(operands)
        self.env.set(name, val)

    def do_equ(self, name, operands):
        try:
            val = self.safe_eval(operands)
            self.env.consts[name] = val
            self.out.append('\t.equ %s, %s' % (name, self._fmt(val)))
        except AsmError:
            # symbolic (references external link-time symbol): translate ops
            self.env.consts[name] = None
            self.out.append('\t.equ %s, %s' % (name, self.xlate_expr_text(operands)))

    def _alias_target(self, opu, operands):
        # 'RN 0' -> r0 ; 'CN 7' -> c7 ; 'FN 0' -> s0/d0 (rare)
        v = operands.strip()
        try:
            n = _to_int(self.safe_eval(v))
        except AsmError:
            return v
        return {'RN': 'r%d' % n, 'CN': 'c%d' % n, 'FN': 's%d' % n}[opu]

    # -- expression -> GAS text (for symbolic EQU/immediates) --
    def xlate_expr_text(self, text):
        t = text
        t = t.replace(':LOR:', '||').replace(':LAND:', '&&')
        t = t.replace(':OR:', '|').replace(':AND:', '&').replace(':EOR:', '^')
        t = t.replace(':SHL:', '<<').replace(':SHR:', '>>')
        t = t.replace(':NOT:', '~').replace(':MOD:', '%')
        t = t.replace('{TRUE}', '1').replace('{FALSE}', '0')
        # fold known consts left as-is (GAS resolves .equ symbols)
        return t.strip()

    def _fmt(self, v):
        if isinstance(v, bool):
            return '1' if v else '0'
        if isinstance(v, int):
            return '0x%x' % v if v >= 0 else str(v)
        return str(v)

    # -- code / data emission --
    _LOCALREF_RE = re.compile(r'%([FBAT]{0,2})(\d+)')

    def emit_code(self, label, op, operands, opu):
        prefix = ''
        if label is not None:
            # numeric local label definition (e.g. "9" or "1")
            if re.fullmatch(r'\d+', label):
                self.out.append('%s:' % label)
            else:
                self.out.append('%s:' % label)
            if op == '':
                return

        # data directives
        if opu in DATA_DIRECTIVES:
            self.emit_data(opu, operands)
            return

        # drop no-op directives
        if opu in ('ROUT', 'PROC', 'ENDP', 'ENTRY', 'KEEP', 'REQUIRE8',
                   'PRESERVE8', 'NOFP', 'CODE32', 'ARM', 'ALIGN' and False):
            return
        if opu == 'ALIGN':
            self.emit_align(operands)
            return
        if opu == 'AREA':
            self.emit_area(operands)
            return
        if opu == 'LTORG':
            self.out.append('\t.ltorg')
            return
        if opu in ('IMPORT', 'EXTERN'):
            self.out.append('\t.extern %s' % operands.split(',')[0].strip())
            return
        if opu in ('EXPORT', 'GLOBAL'):
            self.out.append('\t.global %s' % operands.split(',')[0].strip())
            return
        if opu in ('THUMB', 'CODE16'):
            self.out.append('\t.thumb')
            return
        if opu == 'ASSERT':
            return

        # a plain instruction line
        stmt = (op + (' ' + operands if operands else '')).strip()
        stmt = self.fix_instr(stmt)
        self.out.append('\t%s' % stmt)

    def fix_instr(self, stmt):
        # register aliases
        for alias, tgt in self.env.reg_alias.items():
            stmt = re.sub(r'\b%s\b' % re.escape(alias), tgt, stmt)
        # local label refs %FT9 %BT1 %F9 %B1 -> 9f / 1b
        def lref(m):
            dirn, num = m.group(1), m.group(2)
            if 'B' in dirn:
                return num + 'b'
            return num + 'f'  # forward default
        stmt = self._LOCALREF_RE.sub(lref, stmt)
        # armasm hex literals use a leading '&' (e.g. #&7000000); GAS wants 0x.
        # armasm never uses a bare '&' for bitwise-AND in operands (that is :AND:),
        # so this substitution is unambiguous.
        stmt = re.sub(r'&([0-9A-Fa-f]+)', r'0x\1', stmt)
        return stmt

    def emit_align(self, operands):
        operands = operands.strip()
        if not operands:
            self.out.append('\t.balign 4')
            return
        parts = split_args(operands)
        try:
            n = _to_int(self.safe_eval(parts[0]))
        except AsmError:
            n = 4
        self.out.append('\t.balign %d' % n)

    def emit_area(self, operands):
        parts = [p.strip() for p in operands.split(',')]
        name = parts[0].strip('|')
        attrs = [p.upper() for p in parts[1:]]
        is_code = 'CODE' in attrs
        self.out.append('\t@ AREA %s' % name)
        self.out.append('\t.text' if is_code else '\t.data')
        for a in parts[1:]:
            au = a.upper()
            if au.startswith('ALIGN'):
                try:
                    n = int(au.split('=', 1)[1])
                    self.out.append('\t.balign %d' % (1 << n))
                except (IndexError, ValueError):
                    pass

    def emit_data(self, opu, operands):
        if opu in ('DCD', 'DCDU'):
            self.out.append('\t.word %s' % self.xlate_data_list(operands))
        elif opu in ('DCW', 'DCWU'):
            self.out.append('\t.hword %s' % self.xlate_data_list(operands))
        elif opu == 'DCQ':
            self.out.append('\t.quad %s' % self.xlate_data_list(operands))
        elif opu in ('DCB', '='):
            self.emit_bytes(operands)
        elif opu in ('SPACE', '%'):
            self.out.append('\t.space %s' % self.xlate_expr_text(operands))
        else:
            self.out.append('\t@ TODO data %s %s' % (opu, operands))

    def xlate_data_list(self, operands):
        items = split_args(operands)
        return ', '.join(self.xlate_expr_text(it) for it in items)

    def emit_bytes(self, operands):
        # mixed "string", n, n  -> .ascii / .byte
        for it in split_args(operands):
            it = it.strip()
            if it.startswith('"') and it.endswith('"'):
                self.out.append('\t.ascii %s' % it)
            elif it:
                self.out.append('\t.byte %s' % self.xlate_expr_text(it))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_env(config_path, defines, incdirs):
    env = Environment()
    # apply -D SYM[=val]
    for d in defines:
        if '=' in d:
            k, v = d.split('=', 1)
            try:
                env.set(k, int(v, 0))
            except ValueError:
                env.set(k, v)
        else:
            env.declare(d, 'L')
            env.set(d, True)
    return env


def main(argv=None):
    ap = argparse.ArgumentParser(description="Translate armasm to GAS")
    ap.add_argument('input')
    ap.add_argument('-I', dest='incdirs', action='append', default=[])
    ap.add_argument('-D', dest='defines', action='append', default=[])
    ap.add_argument('--config', default=None)
    ap.add_argument('-o', dest='output', default=None)
    args = ap.parse_args(argv)

    env = build_env(args.config, args.defines, args.incdirs)
    tr = Translator(args.incdirs, env)
    if args.config:
        tr.push_file(args.config)  # processed first via run()? handle below
    try:
        # If a config file is given, translate it first (for its side effects on
        # env) then the main input, sharing env+macros.
        if args.config:
            tr.srcstack = []
            tr.push_file(args.config)
            tr.out.append('\t@ ---- config: %s ----' % args.config)
            while tr.srcstack and not tr.ended:
                raw = tr.srcstack[-1].next()
                if raw is None:
                    tr.srcstack.pop()
                    continue
                tr.process(raw)
        text = tr.run(args.input)
    except AsmError as e:
        print("armasm2gas: ERROR: %s" % e, file=sys.stderr)
        return 1

    if args.output:
        with open(args.output, 'w') as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
