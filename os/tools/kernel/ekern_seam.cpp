// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// ekern_seam.cpp -- the modern-toolchain glue seam for linking the from-source EKA2
// kernel (Track B M4). Everything here bridges a difference between the 2010 RVCT/GCCE
// build and our modern arm-none-eabi EABI toolchain; none of it is Symbian logic.
//
//  1. Name-mangling aliases: the hand-written .cia files reference GCC2-mangled names
//     (Foo__Fv) but the .cpp define the same functions with EABI mangling (_Z...).
//     `.set` maps the old names onto the real definitions.
//  2. Sized operator delete (_ZdlPvj): modern GCC emits calls to the C++14 sized delete,
//     which Symbian's gcchelp.cpp (unsized _ZdlPv only) doesn't provide -> forward it.
//  3. HwInit0(): the uniprocessor early hardware-init hook is `extern` in nk_entry.cia
//     but has no definition in the 2010 source (SMP has NIrq::HwInit0). Minimal stub for
//     bring-up; real early init happens in AsicInitialise / the variant.
//  4. Arm::DefaultDomainAccess: MMU domain-access word the multiple-model xmmu references;
//     provide the supervisor default. TODO: confirm the exact value against a real ROM.

#include <e32def.h>
#include <kernel/kern_priv.h>   // DThread/DProcess/DCodeSeg for the user-thread field dump
#include <memmodel.h>
#ifdef __SMP__
#include <kernel/arm/arm_types.h>   // SFullArmRegSet (the ExcFault(0) dump)
#endif           // DMemModelProcess (iLocalPageDir/iOsAsid) for the addr-space check

// --- 1. mangling aliases (GCC2/RVCT .cia names -> EABI .cpp definitions) --------------
// Done at link time via `ld --defsym` (see tools/kernel/link_ekern.sh) because cross-TU
// `.set` aliases to symbols defined in another object don't reliably propagate:
//   Des8PanicBadDesType__Fv    = _Z19Des8PanicBadDesTypev
//   Des8PanicPosOutOfRange__Fv = _Z22Des8PanicPosOutOfRangev
//   Check__14DBTraceFilter2Ul  = _ZN14DBTraceFilter25CheckEm
//   _ZN1K9MsgInfoE (.cia)      = _ZN1K7MsgInfoE (sglobals.cpp's K::MsgInfo)

// --- crash-monitor stubs (the exmon module is a separate ROM component; no-ops for a
// first boot -- the kernel only touches these on a fault, not during bring-up) ---------
__asm__(
  ".global TheMonitorPtr\n .data\n .balign 4\n TheMonitorPtr: .word 0\n .text\n"
);
// Monitor::Leave(int) -- provide the raw mangled symbol.
extern "C" void _ZN7Monitor5LeaveEi() {}
// _E32Dll -- the variant DLL entry (we link the variant statically for now); success.
extern "C" int _E32Dll(int /*aReason*/) { return 0; }

// --- 2. sized operator delete -> unsized ---------------------------------------------
void operator delete(TAny* aPtr) __NO_THROW;              // provided by gcchelp.cpp
void operator delete(TAny* aPtr, TUint /*aSize*/) __NO_THROW { operator delete(aPtr); }
void operator delete[](TAny* aPtr) __NO_THROW;
void operator delete[](TAny* aPtr, TUint /*aSize*/) __NO_THROW { operator delete[](aPtr); }

// BOARD=virt (link_ekern.sh) passes -DEKSEAM_UART=0xC6002000u: the PL011's data register (bootstrap/virt.inc)
#ifndef EKSEAM_UART
#define EKSEAM_UART 0xC6006004u
#define EKSEAM_UART_S "0xC6006004"
#endif
// --- raw UART tracing (the kernel's own Kern::Printf needs the debug channel up; this is
//     a direct write to the syborg UART virtual address the bootstrap mapped) -------------
// EKSEAM_UART_LSR (BOARD=a64: a 16550): the line status register's offset -- wait for THR empty before each byte
// (a PL011 under QEMU never fills, so virt/syborg write blind)
// A board without a console can also keep the trace in RAM (BOARD=a64: a log the PinePhone's U-Boot saves to the SD
// card after a watchdog reset, a64_glue.cpp). Weak: other boards define none.
extern "C" void ekseam_board_log(unsigned c) __attribute__((weak));
extern "C" void ekseam_board_panic() __attribute__((weak));
// a user-visible notification (level 0 info .. 3 error): the board shows it if it can (PinePhone: the top bar)
extern "C" void ekseam_board_notify(int aLevel, const char* aText) __attribute__((weak));
static char DieName[32];					// the dying thread's process, for the notification
static inline void ekseam_uart_put(unsigned c)
	{
	if (ekseam_board_log) ekseam_board_log(c);
	volatile unsigned* uart = (volatile unsigned*)EKSEAM_UART;
#ifdef EKSEAM_UART_LSR
	// NEVER wait (2026-10-06): an unbounded THR-empty spin cost ~87 us a byte at 115200 -- from inside kernel critical
	// sections, through hundreds of KB of S60 boot probes -- and hangs the whole system for good if the UART stops
	// draining. The board log (USB/SD) is the real channel: a byte the 16550 has no room for is dropped.
	if (!(*(volatile unsigned*)(EKSEAM_UART + EKSEAM_UART_LSR) & 0x20))
		return;
#endif
	*uart = c;
	}
// QUIET after the first user thread (2026-10-06): the kernel bring-up probes below ([ctor], DThread::Create...,
// SetName, [ipc] ...) repeat for every process and thread an S60/SSM boot makes -- hundreds of KB that slowed the
// boot, interleaved across 4 cores into an unreadable log, and pushed the interesting lines out of the SD replay.
// From stage 4 a line starting with a probe prefix is dropped together with its continuation pieces (values,
// indented sub-lines); deaths, panics, faults, [proc] create, the monitor and every board line stay.
// -DEKSEAM_VERBOSE keeps everything.
static volatile int EkQuiet, EkDrop;
static bool ekseam_is_probe(const char* s)
	{
	while (*s == '\n') ++s;
	static const char* const KProbe[] = { "[ctor]", "[EKERN][mtx]", "[EKERN][kpi]", "[EKERN][cs]", "[EKERN][arg]",
		"[EKERN][thr]", "[EKERN][ipc]", "[EKERN][cpu]", "[EKERN] SetName", "[EKERN] u+", "[EKERN] *** EpocThreadFunction",
		"[EKERN] entering real", "[cs] CodeSeg", "[cs] ProcessLoaded", "[EKERN]   desc word0", "[EKERN]   type nibble", 0 };
	for (int i = 0; KProbe[i]; ++i)
		{
		const char* a = s; const char* b = KProbe[i];
		while (*b && *a == *b) { ++a; ++b; }
		if (!*b) return true;
		}
	// "[EKERN]" + 2+ spaces + a capital = the nested create/ctor traces ("[EKERN]   DProcess::Create...");
	// lower case after the spaces = the thread-death report ("[EKERN]     exit type=") -- kept; ustk[ = its stack dump
	const char* k = "[EKERN]";
	while (*k && *s == *k) { ++s; ++k; }
	if (*k) return false;
	int sp = 0;
	while (*s == ' ') { ++s; ++sp; }
	return (sp >= 2 && *s >= 'A' && *s <= 'Z') || (sp >= 2 && s[0] == 'u' && s[1] == 's' && s[2] == 't' && s[3] == 'k');
	}
extern "C" void ekseam_puts(const char* s)
	{
	if (EkQuiet)
		{
		const char* t = s;
		while (*t == '\n') ++t;
		if (*t == '[')
			EkDrop = ekseam_is_probe(s);		// a new line decides; anything else continues the last one
		if (EkDrop)
			return;
		}
	const char* p = s;
	while (*p) ekseam_uart_put((unsigned char)*p++);
	}
extern "C" void ekseam_puthex(unsigned v)
	{
	if (EkQuiet && EkDrop)
		return;
	char buf[12]; int n=0;
	buf[n++]='0'; buf[n++]='x';
	for (int i=28;i>=0;i-=4){ unsigned d=(v>>i)&0xF; buf[n++] = d<10 ? '0'+d : 'a'+d-10; }
	buf[n++]='\n'; buf[n]=0;
	for (int i=0;i<n;i++) ekseam_uart_put((unsigned char)buf[i]);
	}
// A pure virtual call in kernel code (2026-10-09, the NGA boot reset): the objects reference __cxa_pure_virtual WEAKLY
// and nothing defined it, so every abstract class's vtable held 0 -- a pure virtual call jumped to address 0 (the
// A64 boot ROM: a full reset, the DRAM content lost, no fault printed). Now it names its caller and faults properly.
extern "C" void __cxa_pure_virtual()
	{
	ekseam_puts("[EKERN] *** PURE VIRTUAL CALL from LR=");
	ekseam_puthex((unsigned)(unsigned long)__builtin_return_address(0));
	Kern::Fault("PURE-VIRTUAL", 0);
	}

// Boot-stage signal for boards without a console (BOARD=a64: the PinePhone's RGB LED + vibrator,
// src/phorepoc-arm/a64/specific/a64_glue.cpp). Weak: other boards define none. 3 = kernel server up,
// 4 = first user thread, 5 = media drivers loaded, 6 = first user process about to start, 9 = kernel fault.
extern "C" void ekseam_board_stage(int aStage) __attribute__((weak));
static inline void ekseam_stage(int aStage)
	{
#ifndef EKSEAM_VERBOSE
	if (aStage == 4) EkQuiet = 1;		// the first user thread: from here the bring-up probes are noise
#endif
	if (ekseam_board_stage) ekseam_board_stage(aStage);
	}

// --- dump the current NThread's iCsCount(+0x30)/iCsFunction(+0x34) via TheScheduler ------
// TheScheduler (global @ its own symbol) holds iCurrentThread at +0x130 (from ThreadLeaveCS
// disasm). iCsFunction==ECSExitPending(-1=0xffffffff) is what triggers the illegal exit of the
// system-critical initial thread. NB: DoCsFunction zeroes iCsFunction on entry, so a dump AT
// the fault shows 0 -- sample the EARLIER boot points to catch when it becomes -1.
extern "C" void ekseam_dump_cs(const char* tag)
	{
#ifdef __SMP__
	// SMP: TheScheduler's layout differs (per-CPU subschedulers) -- +0x130 is not the current thread there, and
	// dereferencing it took an intermittent data abort (pinephone-port.md P3 rung 2). Ask the nanokernel.
	unsigned cur = (unsigned)(unsigned long)NKern::CurrentThread();
#else
	unsigned sched = (unsigned)(unsigned long)&TheScheduler;
	unsigned cur = *(volatile unsigned*)(sched + 0x130);
#endif
	ekseam_puts(tag);
	ekseam_puts(" curNThr="); ekseam_puthex(cur);
	if (cur)
		{
		ekseam_puts("        iCsCount="); ekseam_puthex(*(volatile unsigned*)(cur + 0x30));
		ekseam_puts("        iCsFunction="); ekseam_puthex(*(volatile unsigned*)(cur + 0x34));
		}
	}

// --- kpinfo clobber probe ------------------------------------------------------------
// TProcessCreateInfo offset +344 = iRootNameLength (verified with our toolchain). P::KernelInfo (sinit.cpp:159) writes 9 there; later
// DProcess::Create (:206) reads it as the length for iFileName.Mid(11,9). At the fault it is
// 0xc8004c80 (a kern-data pointer), so something clobbers +344 in the window between :159 and
// :206. g_kpinfo (=&kpinfo) is captured in the P::KernelInfo wrap below; dumping +344 after
// each intervening call bisects WHICH call scribbles over it.
static volatile unsigned g_kpinfo = 0;
extern "C" void ekseam_dump_kpinfo(const char* tag)
	{
	ekseam_puts(tag);
	ekseam_puts(" &kpinfo="); ekseam_puthex(g_kpinfo);
	if (g_kpinfo)
		{ ekseam_puts("  [+344]iRootNameLength="); ekseam_puthex(*(volatile unsigned*)(g_kpinfo + 344)); }
	}

// --- FIX: TDesC8::Mid(TInt,TInt) .cia miscompilation --------------------------------
// The hand-written naked EABI Mid(aPos,aLength) returns TPtrC8 by sret (r0=&ret, r1=this,
// r2=aPos, r3=aLength). Modern GCC injects `mov r3,r0` at entry (stashing the sret pointer
// so it can `mov r0,r3` on return, per the C++ memory-return ABI) -- but that CLOBBERS r3,
// which the naked asm uses as the incoming aLength. Result: f.word0 = &f|EPtrC (0xd8004c80)
// instead of aLength|EPtrC, so DProcess::Create's kernel-process name descriptor is malformed
// and ValidateName panics the initial thread (KERN 4). Only the 3-arg Mid is affected (Left/
// Right/Mid(aPos) put their last arg in r2). Ptr() is single-arg/scalar-return, unaffected.
// Provide a correct plain-C++ replacement and redirect all callers via `ld --wrap`.
// (Same family as the uc_realx/cumem .cia toolchain gotchas.)
extern "C" const unsigned char* _ZNK6TDesC83PtrEv(const void* aThis);  // TDesC8::Ptr() const
extern "C" void __wrap__ZNK6TDesC83MidEii(unsigned* aRet, const void* aThis, int aPos, int aLength)
	{
	aRet[0] = (unsigned)aLength | 0x10000000u;                              // length | (EPtrC<<28)
	aRet[1] = (unsigned)(unsigned long)(_ZNK6TDesC83PtrEv(aThis) + aPos);   // Ptr() + aPos
	}

// K::MutexCreate probe: the multiple-MM M::Init2 -> MmuBase::Init2 creates RamAlloc then
// HwChunk mutexes; HwChunk's create returns an error -> Panic(EHwChunkMutexCreateFailed)=MMU 9.
// Print each call's name (a _LIT/TLitC8: word0=len|type, inline text at +4), order, and result.
extern "C" void ekseam_putdes(const void* d)
	{
	if (!d) { ekseam_puts("(null)"); return; }
	unsigned w0 = *(const volatile unsigned*)d;
	unsigned len = w0 & 0x0FFFFFFFu;
	const char* p = (const char*)d + 4;             // inline data for EBufC/TLitC
	ekseam_uart_put('"'); for (unsigned i=0;i<len && i<24;i++) ekseam_uart_put((unsigned char)p[i]); ekseam_uart_put('"');
	}
extern "C" int __real__ZN1K11MutexCreateERP6DMutexRK6TDesC8P7DObjectij(void**, const void*, void*, int, unsigned);
extern "C" int __wrap__ZN1K11MutexCreateERP6DMutexRK6TDesC8P7DObjectij(void** aMutex, const void* aName, void* owner, int visible, unsigned order)
	{
	ekseam_puts("[EKERN][mtx] K::MutexCreate name="); ekseam_putdes(aName);
	ekseam_puts(" order="); ekseam_puthex(order);
	int r = __real__ZN1K11MutexCreateERP6DMutexRK6TDesC8P7DObjectij(aMutex, aName, owner, visible, order);
	ekseam_puts("[EKERN][mtx]   -> r="); ekseam_puthex((unsigned)r);
	ekseam_puts("[EKERN][mtx]   -> mutex="); ekseam_puthex(aMutex ? (unsigned)(unsigned long)*aMutex : 0);
	return r;
	}

// --- 3. HwInit0 early hook -----------------------------------------------------------
// Our entry is nkern's nk_entry.cia _E32Startup, which stores RomHeaderAddress/
// SuperPageAddress but -- unlike kernel/arm/k_entry.cia (kc_exe.lib), which real
// ekern builds use -- never calls PP::InitSuperPageFromRom. Without it the superpage
// iDebugMask (kernel+user trace masks), kernel config flags, BTrace filter and
// disabled capabilities are never copied from the ROM header (TRACE_MASK was dead:
// ExecHandler::DebugMask() returned 0). Call it here, same pre-ctor timing as k_entry.
extern "C" unsigned long RomHeaderAddress;
extern "C" unsigned long SuperPageAddress;
extern "C" void __real__ZN2PP20InitSuperPageFromRomEmm(unsigned long, unsigned long);
extern "C" void HwInit0()
	{
	ekseam_puts("\n[EKERN] _E32Startup reached HwInit0 -- from-source kernel is running!\n");
	__real__ZN2PP20InitSuperPageFromRomEmm(RomHeaderAddress, SuperPageAddress);
	ekseam_puts("[EKERN] superpage inited from ROM header (iDebugMask etc.)\n");
	}

// A global constructor -- proves the __CTOR_LIST__ loop runs before KernelMain.
struct _EkSeamCtor { _EkSeamCtor() { ekseam_puts("[EKERN] C++ static constructors running\n"); } };
static _EkSeamCtor _ek_seam_ctor;

// --- KernelMain tracing via ld --wrap ------------------------------------------------
// __wrap_<sym> intercepts calls to <sym>; __real_<sym> is the real one. Prints a marker,
// then chains. Wired with `ld --wrap KernelMain` (see link_ekern.sh).
extern "C" void __real_KernelMain();
extern "C" void __wrap_KernelMain()
	{
	ekseam_puts("[EKERN] KernelMain entered\n");
	__real_KernelMain();
	ekseam_puts("[EKERN] KernelMain returned (unexpected)\n");
	}

// KernelMain's phases: P::CreateVariant() -> NKern::Init0() -> K::InitialiseMicrokernel().
extern "C" void _ZN1P13CreateVariantEv();
extern "C" void __real__ZN1P13CreateVariantEv();
extern "C" void __wrap__ZN1P13CreateVariantEv()
	{ ekseam_puts("[EKERN]  P::CreateVariant...\n"); __real__ZN1P13CreateVariantEv();
	  ekseam_puts("[EKERN]  P::CreateVariant done\n"); }

extern "C" void __real__ZN5NKern5Init0EPv(void*);
extern "C" void __wrap__ZN5NKern5Init0EPv(void* p)
	{ ekseam_puts("[EKERN]  NKern::Init0...\n"); __real__ZN5NKern5Init0EPv(p);
	  ekseam_puts("[EKERN]  NKern::Init0 done\n"); }

extern "C" int __real__ZN1K21InitialiseMicrokernelEv();
extern "C" int __wrap__ZN1K21InitialiseMicrokernelEv()
	{ ekseam_puts("[EKERN]  K::InitialiseMicrokernel...\n");
	  int r = __real__ZN1K21InitialiseMicrokernelEv();
	  ekseam_puts("[EKERN]  K::InitialiseMicrokernel done\n"); return r; }

// P::CreateVariant's phases (putils.cpp): BTrace::Init0 -> InitVarExt(.data) ->
// InitVarExt(variant) -> FindRomRootDirectory.
extern "C" void __real__ZN6BTrace5Init0Ev();
extern "C" void __wrap__ZN6BTrace5Init0Ev()
	{ ekseam_puts("[EKERN]   BTrace::Init0...\n"); __real__ZN6BTrace5Init0Ev();
	  ekseam_puts("[EKERN]   BTrace::Init0 done\n"); }

// K::InitialiseMicrokernel's early phases (sinit.cpp): M::Init1 (memory model) ->
// A::Init1 (enables interrupts) -> P::KernelInfo -> heap -> first process/thread.
extern "C" void __real__ZN1M5Init1Ev();
extern "C" void __wrap__ZN1M5Init1Ev()
	{ ekseam_puts("[EKERN]   M::Init1...\n"); __real__ZN1M5Init1Ev();
	  ekseam_puts("[EKERN]   M::Init1 done\n"); }
extern "C" void __real__ZN1A5Init1Ev();
extern "C" void __wrap__ZN1A5Init1Ev()
	{ ekseam_puts("[EKERN]   A::Init1 (enables interrupts)...\n"); __real__ZN1A5Init1Ev();
	  ekseam_puts("[EKERN]   A::Init1 done\n"); }

extern "C" void __real__ZN1P10KernelInfoER18TProcessCreateInfoRPvS3_(void*, void*, void*);
extern "C" void __wrap__ZN1P10KernelInfoER18TProcessCreateInfoRPvS3_(void* a, void* b, void* c)
	{ ekseam_puts("[EKERN]   P::KernelInfo...\n");
	  __real__ZN1P10KernelInfoER18TProcessCreateInfoRPvS3_(a, b, c);
	  // a = &kpinfo (TProcessCreateInfo&); b = &kern_stack_addr, c = &kern_heap_addr.
	  g_kpinfo = (unsigned)(unsigned long)a;
	  ekseam_puts("[EKERN]   kern_stack_addr="); ekseam_puthex(*(unsigned*)b);
	  ekseam_puts("[EKERN]   kern_heap_addr ="); ekseam_puthex(*(unsigned*)c);
	  ekseam_dump_kpinfo("[EKERN][kpi] after P::KernelInfo:");
	  ekseam_puts("[EKERN]   P::KernelInfo done (heap next)\n"); }
extern "C" void* __real__ZN1P10NewProcessEv();
extern "C" void* __wrap__ZN1P10NewProcessEv()
	{ ekseam_puts("[EKERN]   P::NewProcess...\n");
	  void* r = __real__ZN1P10NewProcessEv();
	  ekseam_dump_kpinfo("[EKERN][kpi] after P::NewProcess:");
	  ekseam_puts("[EKERN]   P::NewProcess done\n"); return r; }

// Initial null-thread / scheduler bring-up (the current fault site). Bracket the chain:
//   P::NewProcess -> ... -> DProcess::NewThread -> DArmPlatProcess::GetNewThread ->
//   DThread::Create -> DThread::DoCreate -> NKern::ThreadCreate -> NThread::Create ->
//   NThreadBase::Create -> NKern::Init(NThread).  A reset exception fires somewhere in here.
extern "C" int __real__ZN8DProcess9NewThreadERP7DThreadR17SThreadCreateInfoPi10TOwnerType(void*, void*, void*, void*, int);
extern "C" int __wrap__ZN8DProcess9NewThreadERP7DThreadR17SThreadCreateInfoPi10TOwnerType(void* a, void* b, void* c, void* d, int e)
	{ ekseam_puts("[EKERN]   DProcess::NewThread...\n");
	  int r = __real__ZN8DProcess9NewThreadERP7DThreadR17SThreadCreateInfoPi10TOwnerType(a,b,c,d,e);
	  ekseam_puts("[EKERN]   DProcess::NewThread done\n"); return r; }

extern "C" int __real__ZN15DArmPlatProcess12GetNewThreadERP7DThreadR17SThreadCreateInfo(void*, void*, void*);
extern "C" int __wrap__ZN15DArmPlatProcess12GetNewThreadERP7DThreadR17SThreadCreateInfo(void* a, void* b, void* c)
	{ unsigned sp; __asm__ volatile("mov %0, sp" : "=r"(sp));
	  ekseam_puts("[EKERN]    GetNewThread: sp="); ekseam_puthex(sp);
	  int r = __real__ZN15DArmPlatProcess12GetNewThreadERP7DThreadR17SThreadCreateInfo(a,b,c);
	  ekseam_dump_kpinfo("[EKERN][kpi] after GetNewThread:");
	  ekseam_puts("[EKERN]    DArmPlatProcess::GetNewThread done\n"); return r; }

extern "C" int __real__ZN7DThread6CreateER17SThreadCreateInfo(void*, void*);
extern "C" int __wrap__ZN7DThread6CreateER17SThreadCreateInfo(void* a, void* b)
	{ ekseam_puts("[EKERN]    DThread::Create...\n");
	  int r = __real__ZN7DThread6CreateER17SThreadCreateInfo(a,b);
	  ekseam_puts("[EKERN]    DThread::Create done\n");
	  ekseam_dump_kpinfo("[EKERN][kpi] after DThread::Create:");
	  ekseam_dump_cs("[EKERN][cs] after DThread::Create:");
#ifdef EKSEAM_FLUSH_ON_PROC
	  { SThreadCreateInfo& ci = *(SThreadCreateInfo*)b;	// (debug: every thread start into the SD log)
	    char nm[40]; TInt i = 0;
	    for (; i < ci.iName.Length() && i < 38; ++i) nm[i] = (char)ci.iName[i];
	    nm[i++] = '\n'; nm[i] = 0;
	    ekseam_puts("[dbg] thread "); ekseam_puts(nm);
	    if (ekseam_board_panic) ekseam_board_panic(); }
#endif
	  return r; }

extern "C" int __real__ZN7DThread8DoCreateER17SThreadCreateInfo(void*, void*);
extern "C" int __wrap__ZN7DThread8DoCreateER17SThreadCreateInfo(void* a, void* b)
	{ ekseam_puts("[EKERN]     DThread::DoCreate...\n");
	  int r = __real__ZN7DThread8DoCreateER17SThreadCreateInfo(a,b);
	  ekseam_puts("[EKERN]     DThread::DoCreate done\n"); return r; }

// (Removed the supervisor DoCreate sub-step probes -- AllocateSupervisorStack / ClaimMessagePool
//  / DMemModelChunk::Adjust wraps + the HW-PD/PTE/TTBR dumps. They pinned the abort to a chunk
//  mapped at the wrong PTE index because the kernel linear section base (ROM header iKernelLimit)
//  was not 1MB-aligned; fixed in tools/rombuild.py.)
extern "C" int __real__ZN5NKern12ThreadCreateEP7NThreadR18SNThreadCreateInfo(void*, void*);
extern "C" int __wrap__ZN5NKern12ThreadCreateEP7NThreadR18SNThreadCreateInfo(void* a, void* b)
	{ ekseam_puts("[EKERN]     NKern::ThreadCreate...\n");
	  int r = __real__ZN5NKern12ThreadCreateEP7NThreadR18SNThreadCreateInfo(a,b);
	  ekseam_puts("[EKERN]     NKern::ThreadCreate done\n"); return r; }

extern "C" int __real__ZN7NThread6CreateER18SNThreadCreateInfoi(void*, void*, int);
extern "C" int __wrap__ZN7NThread6CreateER18SNThreadCreateInfoi(void* a, void* b, int c)
	{ ekseam_puts("[EKERN]      NThread::Create...\n");
	  int r = __real__ZN7NThread6CreateER18SNThreadCreateInfoi(a,b,c);
	  ekseam_puts("[EKERN]      NThread::Create done\n"); return r; }

extern "C" int __real__ZN11NThreadBase6CreateER18SNThreadCreateInfoi(void*, void*, int);
extern "C" int __wrap__ZN11NThreadBase6CreateER18SNThreadCreateInfoi(void* a, void* b, int c)
	{ ekseam_puts("[EKERN]       NThreadBase::Create...\n");
	  int r = __real__ZN11NThreadBase6CreateER18SNThreadCreateInfoi(a,b,c);
	  ekseam_puts("[EKERN]       NThreadBase::Create done\n"); return r; }

extern "C" void __real__ZN5NKern4InitEP7NThreadR18SNThreadCreateInfo(void*, void*);
extern "C" void __wrap__ZN5NKern4InitEP7NThreadR18SNThreadCreateInfo(void* a, void* b)
	{ ekseam_puts("[EKERN]       NKern::Init(NThread)...\n");
	  __real__ZN5NKern4InitEP7NThreadR18SNThreadCreateInfo(a,b);
	  ekseam_puts("[EKERN]       NKern::Init(NThread) done\n");
	  ekseam_dump_kpinfo("[EKERN][kpi] after NKern::Init:");
	  ekseam_dump_cs("[EKERN][cs] after NKern::Init:"); }

// __ArmVectorReset diagnostic: the from-source kernel reaches the ARM *reset* vector
// during initial-thread bring-up. Dump the CPU state so we learn what really happened:
// current mode (CPSR), the mode we came from (SPSR), and the banked LRs of the exception
// modes -- whichever LR is a sane kernel code address (0x8002xxxx) is the faulting PC.
// Reached via `ld --wrap __ArmVectorReset` redirecting the `.word __ArmVectorReset` in the
// vector table (entry.h) to here. No stack is used (SP is not valid at the vector); a small
// stackless hex printer returns through r8.
// (Reset-vector dumper removed: the reset landing proved layout-sensitive, so the faulting
//  PC is now captured with QEMU's own `-d int,cpu_reset` exception log instead.)

// Bisect the DThread ctor chain (DThread::DThread calls, in order: DObject, RObjectIx,
// RArrayBase, TDfc, TBufCBase, TPriListBase, NThreadBase). The wild-branch corruption is in
// one of these -- whichever prints "enter" but never "exit" (after the GetNewThread sp= line)
// is the culprit. (These also run for the DProcess in P::NewProcess -> bounded earlier noise.)
extern "C" void __real__ZN7DObjectC1Ev(void*);
extern "C" void __wrap__ZN7DObjectC1Ev(void* t)
	{ ekseam_puts("[ctor] DObject...\n"); __real__ZN7DObjectC1Ev(t);
	  ekseam_puts("[ctor] DObject done\n"); }
extern "C" void __real__ZN4TDfcC1EPFvPvES0_P7TDfcQuei(void*, void*, void*, void*, int);
extern "C" void __wrap__ZN4TDfcC1EPFvPvES0_P7TDfcQuei(void* t, void* a, void* b, void* c, int d)
	{ ekseam_puts("[ctor] TDfc...\n"); __real__ZN4TDfcC1EPFvPvES0_P7TDfcQuei(t,a,b,c,d);
	  ekseam_puts("[ctor] TDfc done\n"); }
extern "C" void __real__ZN11NThreadBaseC1Ev(void*);
extern "C" void __wrap__ZN11NThreadBaseC1Ev(void* t)
	{ ekseam_puts("[ctor] NThreadBase...\n"); __real__ZN11NThreadBaseC1Ev(t);
	  ekseam_puts("[ctor] NThreadBase done\n"); }

// The DThread ctor calls its base/member subobject ctors via the C2 (base-object) mangled
// names, NOT C1 (see link_ekern.sh). C1/C2 fold to one
// address but `ld --wrap` matches the symbol NAME, so the C1 wraps above never fired for the
// bases. These C2 wraps bisect the ctor tail after TDfc: whichever prints "..." but never
// "done" (after the "GetNewThread: sp=" line) is where the hang is.
extern "C" void __real__ZN10TBufCBase8C2Ev(void*);
extern "C" void __wrap__ZN10TBufCBase8C2Ev(void* t)
	{ ekseam_puts("[ctor] TBufCBase8(C2)...\n"); __real__ZN10TBufCBase8C2Ev(t);
	  ekseam_puts("[ctor] TBufCBase8(C2) done\n"); }
extern "C" void __real__ZN12TPriListBaseC2Ei(void*, int);
extern "C" void __wrap__ZN12TPriListBaseC2Ei(void* t, int n)
	{ ekseam_puts("[ctor] TPriListBase(C2)...\n"); __real__ZN12TPriListBaseC2Ei(t, n);
	  ekseam_puts("[ctor] TPriListBase(C2) done\n"); }
extern "C" void __real__ZN11NThreadBaseC2Ev(void*);
extern "C" void __wrap__ZN11NThreadBaseC2Ev(void* t)
	{ ekseam_puts("[ctor] NThreadBase(C2)...\n"); __real__ZN11NThreadBaseC2Ev(t);
	  ekseam_puts("[ctor] NThreadBase(C2) done\n"); }
// GetNewThread constructs the DThread via the C2 variant. All four subobject ctors above
// complete, so the hang is in the ctor TAIL (after NThreadBase) or GetNewThread's post-ctor
// code -- both pure stores + `pop {..,pc}`. If "[ctor] DThread(C2) done" prints, DThread::DThread
// returned cleanly and the corrupt-PC branch is in GetNewThread's tail; if it does NOT print,
// the wild branch is DThread::DThread's own return.
extern "C" void __real__ZN7DThreadC2Ev(void*);
extern "C" void __wrap__ZN7DThreadC2Ev(void* t)
	{ ekseam_puts("[ctor] DThread(C2)...\n"); __real__ZN7DThreadC2Ev(t);
	  ekseam_puts("[ctor] DThread(C2) done\n"); }

// Post-DThread::Create sequence in K::InitialiseMicrokernel (sinit.cpp): pP->Create ->
// M::Init2 -> A::Init2 -> K::CreateObjectContainers. The wild branch to __ArmVectorReset
// (ncutils.cpp:120 FAULT) happens somewhere after "DThread::Create done"; whichever of these
// prints "..." but not "done" brackets it.
#ifndef EKSEAM_MIN_USTACK
#define EKSEAM_MIN_USTACK 0x14000
#endif
extern "C" int __real__ZN8DProcess6CreateEiR18TProcessCreateInfoP5HBuf8(void*, int, void*, void*);
extern "C" int __wrap__ZN8DProcess6CreateEiR18TProcessCreateInfoP5HBuf8(void* a, int b, void* c, void* d)
	{ ekseam_dump_kpinfo("[EKERN][kpi] DProcess::Create enter:");
	  // Minimum main-thread user stack (2026-10-14): apps declare their stack for RVCT frames (SkyForce Reloaded: 0x2000,
	  // fine on stock RVCT builds); our GCC-built avkon/CONE chain overflows it in the first draw (data abort at the guard page).
	  // Raise any user exe's main stack to the Qt-app size. -DEKSEAM_MIN_USTACK=0 disables.
	  if (!b && c && EKSEAM_MIN_USTACK && ((TProcessCreateInfo*)c)->iStackSize < EKSEAM_MIN_USTACK)
		((TProcessCreateInfo*)c)->iStackSize = EKSEAM_MIN_USTACK;
	  // c = &aInfo as the CALLEE received it. Dump the pointer and the three fields Mid uses:
	  // iFileName.word0 (@+0), iRootNameOffset (@+340), iRootNameLength (@+344). If c differs
	  // from g_kpinfo (0xc8004dc8) or these fields differ from EBuf/11/9, the arg is the bug.
	  ekseam_puts("[EKERN][arg] DProcess::Create aInfo(c)="); ekseam_puthex((unsigned)(unsigned long)c);
	  if (c) {
	    ekseam_puts("[EKERN][arg]   iFileName.word0="); ekseam_puthex(*(volatile unsigned*)((char*)c+0));
	    ekseam_puts("[EKERN][arg]   [+340]iRootNameOffset="); ekseam_puthex(*(volatile unsigned*)((char*)c+340));
	    ekseam_puts("[EKERN][arg]   [+344]iRootNameLength="); ekseam_puthex(*(volatile unsigned*)((char*)c+344));
	    // the process root name (iFileName is a TBuf8<256>: data @+8) -- makes a
	    // multi-process boot log followable: "[proc] create <name>"
	    unsigned off = *(volatile unsigned*)((char*)c+340), len = *(volatile unsigned*)((char*)c+344);
	    if (off < 256 && len < 64) {
	      char nm[72]; unsigned i;
	      for (i = 0; i < len; i++) nm[i] = ((char*)c)[8 + off + i];
	      nm[i++] = '\n'; nm[i] = 0;
	      ekseam_puts("[proc] create "); ekseam_puts(nm);
#ifdef EKSEAM_FLUSH_ON_PROC
	      if (ekseam_board_panic) ekseam_board_panic();	// (debug: the SD log out at every process start)
#endif
	    }
	  }
	  ekseam_dump_cs("[EKERN][cs] DProcess::Create enter:");
	  ekseam_puts("[EKERN]   DProcess::Create...\n");
	  int r = __real__ZN8DProcess6CreateEiR18TProcessCreateInfoP5HBuf8(a,b,c,d);
	  ekseam_puts("[EKERN]   DProcess::Create done r="); ekseam_puthex((unsigned)r); return r; }
// Bracket the sinit.cpp:207-224 window (DProcess::Create returns .. M::Init2). These calls are
// intra-sinit.o bl's EXCEPT the cross-object ones below, which --wrap can catch:
//   new TDfcQue (:212) -> TDfcQue::TDfcQue() ctor (marker => alloc succeeded; absent => null,
//     so K::SvMsgQ==0 -> KErrNoMemory), and pP->AddThread(*pT) (:221).
extern "C" void __real__ZN7TDfcQueC1Ev(void*);
extern "C" void __wrap__ZN7TDfcQueC1Ev(void* t)
	{ ekseam_puts("[EKERN]   TDfcQue::TDfcQue (SvMsgQ alloc ok)...\n"); __real__ZN7TDfcQueC1Ev(t);
	  ekseam_puts("[EKERN]   TDfcQue::TDfcQue done\n"); }
extern "C" int __real__ZN8DProcess9AddThreadER7DThread(void*, void*);
extern "C" int __wrap__ZN8DProcess9AddThreadER7DThread(void* a, void* b)
	{ ekseam_puts("[EKERN]   DProcess::AddThread...\n");
	  int r = __real__ZN8DProcess9AddThreadER7DThread(a, b);
	  ekseam_puts("[EKERN]   DProcess::AddThread done r="); ekseam_puthex((unsigned)r); return r; }

// P::StartExtensions builds `new RArray<SExtInit1EntryPoint>(--K::ExtensionCount)` (putils.cpp:269)
// as the extension entry-point array. K::ExtensionCount = number of ROM kernel extensions counted
// in P::CreateVariant (incl. the variant); the pre-decrement excludes the variant. Our minimal
// from-source ROM carries ONLY the variant -> ExtensionCount==1 -> --==0 -> RArray granularity 0
// trips RArrayBase's `aGranularity>0` assert (Panic TCdtPanic 127), killing the system-critical
// supervisor thread -> K::Fault(ESystemThreadPanic)=KERN 4. With no non-variant extensions there
// is nothing to start, so skip the body (leaves K::ExtensionArray NULL, which nothing else reads
// on this minimal boot). REMOVE once the ROM carries real kernel extensions. K::ExtensionCount =
// _ZN1K14ExtensionCountE @0xc800136c (verified against the StartExtensions disasm).
extern "C" { extern int _ZN1K14ExtensionCountE; }
extern "C" void __real__ZN1P15StartExtensionsEv();
extern "C" void __wrap__ZN1P15StartExtensionsEv()
	{
	if (_ZN1K14ExtensionCountE <= 1)
		{ ekseam_puts("[EKERN] P::StartExtensions: only the variant, no extensions to start -- skipping\n");
		  return; }
	__real__ZN1P15StartExtensionsEv();
	}

// Final SupervisorThread steps (sinit.cpp:404-411): M::Init4() then K::StartKernelServer().
// Markers to confirm the from-source kernel reaches the end of boot / kernel-server start.
extern "C" void __real__ZN1M5Init4Ev();
extern "C" void __wrap__ZN1M5Init4Ev()
	{ ekseam_puts("[EKERN] M::Init4...\n"); __real__ZN1M5Init4Ev();
	  ekseam_puts("[EKERN] M::Init4 done\n"); }
// exstart.cpp is now linked into ekern (StartSecondary + StartSecondaryDfc + InitExtension). It is
// normally a kernel extension whose Init1 enqueues StartSecondaryDfc on SvMsgQ; the supervisor DFC
// loop then runs StartSecondary -> Kern::ProcessCreate + Resume of the ROM iSecondaryFile (our
// minisec user EXE). Since we skip StartExtensions (no ROM extensions), enqueue it ourselves right
// before StartKernelServer arms the barrier + enters the DFC loop (TDfcQue::ThreadFunction).
// NB: StartSecondaryDfc's global ctor runs at kernel static-init (exstart is statically linked into
// ekern, not loaded as a DLL later), so its DfcQ was bound to Kern::SvMsgQue()==NULL and Enque() is a
// no-op. Rather than fix the DFC queue, just CALL StartSecondary directly here -- we are already on
// the supervisor thread (the exact context the DFC would run in). (--wrap routes this through the
// StartSecondary tracing wrap below.) Done before entering the barrier/DFC loop.
extern "C" void _Z14StartSecondaryPv(void*);
extern "C" void ekseam_load_fb_driver();   // M5.5: real svpframebuffer kext (ekern_fb_glue.cpp)
extern "C" void ekseam_load_media_drivers(); // M7: locmedia+mednvmemory kexts (ekern_media_glue.cpp)
extern "C" void ekseam_load_input_drivers(); // keyboard+pointer kexts (ekern_input_glue.cpp)
extern "C" void __real__ZN1K17StartKernelServerEv();
extern "C" void __wrap__ZN1K17StartKernelServerEv()
	{ ekseam_puts("[EKERN] *** K::StartKernelServer -- from-source kernel reached kernel-server start! ***\n");
	  ekseam_stage(3);
	  ekseam_load_fb_driver();   // M5.5: load the REAL Syborg fb driver as a kernel extension (was ekseam_fb_test)
	  ekseam_load_media_drivers(); // M7: locmedia (ELOCD) + syborg mednvmemory -> drive C: registered
	  ekseam_stage(5);
	  ekseam_load_input_drivers(); // syborg keyboard + pointer -> Kern::AddEvent
	  ekseam_stage(6);
	  ekseam_puts("[EKERN] launching the first user process (secondary) directly...\n");
	  _Z14StartSecondaryPv(0);
	  __real__ZN1K17StartKernelServerEv();
	  ekseam_puts("[EKERN] K::StartKernelServer returned\n"); }

// ExecHandler::DebugPrint(TAny* aDes, TInt aMode) -- the kernel side of RDebug::Print (slow exec
// #86). If our minisec user EXE reaches user mode and issues its `swi 0x56`, this fires: it proves
// user-mode execution + the user->kernel SWI/exec dispatch works in the from-source kernel.
// PP::InitSuperPageFromRom(aRomHeader, aSuperPage) -- copies iTraceMask -> superpage
// iDebugMask. Print args + the mask word to see why DebugMask() returns 0.
extern "C" void __real__ZN2PP20InitSuperPageFromRomEmm(unsigned long, unsigned long);
extern "C" void __wrap__ZN2PP20InitSuperPageFromRomEmm(unsigned long aRomHeader, unsigned long aSuperPage)
	{
	ekseam_puts("[EKERN] PP::InitSuperPageFromRom romHdr="); ekseam_puthex((unsigned)aRomHeader);
	ekseam_puts("[EKERN]   superPage="); ekseam_puthex((unsigned)aSuperPage);
	ekseam_puts("[EKERN]   romHdr->iTraceMask[0]="); ekseam_puthex(*(volatile unsigned*)(aRomHeader + 256));
	__real__ZN2PP20InitSuperPageFromRomEmm(aRomHeader, aSuperPage);
	}

// Read an 8-bit kernel-exec name descriptor (TDesC8, as the IPC execs receive) into buf.
static void ekseam_read_des8(const void* d, char* buf, unsigned bufsz)
	{
	// kumemget (LDRT when the caller is user mode), as the kernel's own Kern::KUDesGet: a plain kernel ldr of the
	// user descriptor faulted under the flexible model (pinephone-port.md, flexible S60).
	unsigned w0 = 0, type, len, i;
	kumemget(&w0, d, 4);
	type = w0 >> 28; len = w0 & 0x0FFFFFFF;
	const char* p = 0;
	if (type == 0) p = (const char*)d + 4;
	else if (type == 3) p = (const char*)d + 8;
	else kumemget(&p, (const char*)d + (type == 1 ? 4 : 8), 4);
	i = len < bufsz - 1 ? len : bufsz - 1;
	if (i) kumemget(buf, p, i);
	buf[i] = 0;
	}

// ExecHandler::ServerCreate{,WithOptions} -- print each server name created, so we can see
// whether "!FileServer" ever enters the global server container.
extern "C" int __real__ZN11ExecHandler23ServerCreateWithOptionsEPK6TDesC8iii(
	const void*, int, int, int);
extern "C" int __wrap__ZN11ExecHandler23ServerCreateWithOptionsEPK6TDesC8iii(
	const void* aName, int aMode, int aRole, int aOpts)
	{
	char buf[24] = {0};
	if (aName) ekseam_read_des8(aName, buf, sizeof(buf));
	ekseam_puts("\n[EKERN][ipc] ServerCreateWithOptions name='"); ekseam_puts(buf); ekseam_puts("'");
	int r = __real__ZN11ExecHandler23ServerCreateWithOptionsEPK6TDesC8iii(aName, aMode, aRole, aOpts);
	ekseam_puts("[EKERN][ipc]   -> r="); ekseam_puthex((unsigned)r);
	return r;
	}
extern "C" int __real__ZN11ExecHandler12ServerCreateEPK6TDesC8i(const void*, int);
extern "C" int __wrap__ZN11ExecHandler12ServerCreateEPK6TDesC8i(const void* aName, int aMode)
	{
	char buf[24] = {0};
	if (aName) ekseam_read_des8(aName, buf, sizeof(buf));
	ekseam_puts("\n[EKERN][ipc] ServerCreate name='"); ekseam_puts(buf); ekseam_puts("'");
	int r = __real__ZN11ExecHandler12ServerCreateEPK6TDesC8i(aName, aMode);
	ekseam_puts("[EKERN][ipc]   -> r="); ekseam_puthex((unsigned)r);
	return r;
	}

// ExecHandler::SessionCreate(aServer, aMsgSlots, aSecurityPolicy, aMode) -- the kernel
// side of RSessionBase::CreateSession. estart's RFs::Connect fails KErrNotFound here;
// print the result (and the first user-descriptor word of the name it looked up) to see
// whether the global server FindByName misses, and whether the name arrived intact.
extern "C" int __real__ZN11ExecHandler13SessionCreateERK6TDesC8iPK15TSecurityPolicyi(
	void* aServer, int aMsgSlots, const void* aSec, int aMode);
extern "C" int __wrap__ZN11ExecHandler13SessionCreateERK6TDesC8iPK15TSecurityPolicyi(
	void* aServer, int aMsgSlots, const void* aSec, int aMode)
	{
	// aServer is a ref to the USER-side name descriptor (estart's TLitC16 "!FileServer").
	// We run in estart's context here (its page tables active), so read it directly:
	// word0 = length|type<<28; TLitC/TBufC chars follow inline at +4 (UTF-16).
	if (aServer)
		{
		char buf[24] = {0};
		ekseam_read_des8(aServer, buf, sizeof(buf));
		ekseam_puts("\n[EKERN][ipc] SessionCreate name='"); ekseam_puts(buf); ekseam_puts("'\n");
		}
	int r = __real__ZN11ExecHandler13SessionCreateERK6TDesC8iPK15TSecurityPolicyi(
		aServer, aMsgSlots, aSec, aMode);
	ekseam_puts("[EKERN][ipc] SessionCreate r="); ekseam_puthex((unsigned)r);
	return r;
	}

// ExecHandler::DebugMask() -- what userland (sf_main DebugReg gate) sees. If this
// returns 0 despite TRACE_MASK, the ROM-header iTraceMask -> superpage copy is broken.
extern "C" unsigned __real__ZN11ExecHandler9DebugMaskEv();
extern "C" unsigned __wrap__ZN11ExecHandler9DebugMaskEv()
	{
	unsigned r = __real__ZN11ExecHandler9DebugMaskEv();
	ekseam_puts("[EKERN] ExecHandler::DebugMask -> "); ekseam_puthex(r);
	return r;
	}

extern "C" void __real__ZN11ExecHandler10DebugPrintEPvi(void*, int);
extern "C" void __wrap__ZN11ExecHandler10DebugPrintEPvi(void* aDes, int aMode)
	{ static bool once;
	  if (!once)
		{
		once = true;
		ekseam_puts("\n[EKERN] *** ExecHandler::DebugPrint reached -- USER-MODE SWI/exec works! (first call; banner suppressed hereafter) mode=");
		ekseam_puthex((unsigned)aMode);
		}
	  // user RDebug goes to the board UART only; forward the loader's failure lines (efile ldr_builtin_devices.cpp,
	  // "LDR: load <name> -> <err>") to the seam log too -- PhoneLab sees those. Prefix-filtered: no general flood.
	  if (aDes)
		{
		char buf[160];
		ekseam_read_des8(aDes, buf, sizeof(buf));
		// "LDR:" (the loader's failures) and "PH:" (any OURS diagnostic meant for PhoneLab -- keep such lines rare)
		// + the on-phone test programs' own tags (wlanjoin, ssltest, mixtest, dsatest; phwebtest / [js] = the browser)
		static const char* const KPass[] = { "LDR:", "PH:", "[wlanjoin]", "[ssltest]", "[mixtest]", "[dsatest]", "[senstest]", "phwebtest", "[js]", "CSysApLightsController", "[phvolume]",
			// (2026-10-18, the S^4 shell on the phone) Qt's qWarning/qCritical texts (QtLockedFile..., QtSingleCoreApplication...) and Orbit's
			"Qt", "Hb", "QObject", "QPixmap", "QFont", "_WSEVENT" };
		for (unsigned i = 0; i < sizeof(KPass) / sizeof(KPass[0]); ++i)
			{
			const char* p = KPass[i];
			int k = 0;
			while (p[k] && buf[k] == p[k])
				++k;
			if (!p[k])
				{ ekseam_puts("\n[user] "); ekseam_puts(buf); break; }
			}
		}
	  __real__ZN11ExecHandler10DebugPrintEPvi(aDes, aMode); }

// StartSecondary runs on the SvMsgQ DFC thread and creates+resumes the secondary user process.
// The TDfc holds &StartSecondary, so --wrap redirects the DFC's call here. Bracket it (the many
// existing DProcess::Create / DThread::Create / DCodeSeg wraps also fire for the user process).
extern "C" void __real__Z14StartSecondaryPv(void*);
extern "C" void __wrap__Z14StartSecondaryPv(void* p)
	{ ekseam_puts("\n[EKERN] === StartSecondary: creating the first USER process ===\n");
	  __real__Z14StartSecondaryPv(p);
	  ekseam_puts("[EKERN] === StartSecondary returned (secondary resumed) ===\n"); }

// DProcess::Create returns KErrNotFound(-5) for the kernel process AFTER the SetNames succeed.
// Bracket the sub-calls that can return it: DMemModelProcess::DoCreate, then DCodeSeg::Create ->
// DEpocCodeSeg::DoCreate (XIP codeseg for the ROM kernel). Whichever prints a nonzero r is it.
extern "C" int __real__ZN16DMemModelProcess8DoCreateEiR18TProcessCreateInfo(void*, int, void*);
extern "C" int __wrap__ZN16DMemModelProcess8DoCreateEiR18TProcessCreateInfo(void* a, int b, void* c)
	{ ekseam_puts("[EKERN]    DMemModelProcess::DoCreate...\n");
	  int r = __real__ZN16DMemModelProcess8DoCreateEiR18TProcessCreateInfo(a,b,c);
	  ekseam_puts("[EKERN]    DMemModelProcess::DoCreate done r="); ekseam_puthex((unsigned)r); return r; }
extern "C" int __real__ZN8DCodeSeg6CreateER18TCodeSegCreateInfoP8DProcess(void*, void*, void*);
extern "C" int __wrap__ZN8DCodeSeg6CreateER18TCodeSegCreateInfoP8DProcess(void* a, void* b, void* c)
	{ ekseam_puts("[EKERN]    DCodeSeg::Create...\n");
	  int r = __real__ZN8DCodeSeg6CreateER18TCodeSegCreateInfoP8DProcess(a,b,c);
	  ekseam_puts("[EKERN]    DCodeSeg::Create done r="); ekseam_puthex((unsigned)r); return r; }
extern "C" int __real__ZN12DEpocCodeSeg8DoCreateER18TCodeSegCreateInfoP8DProcess(void*, void*, void*);
extern "C" int __wrap__ZN12DEpocCodeSeg8DoCreateER18TCodeSegCreateInfoP8DProcess(void* a, void* b, void* c)
	{ ekseam_puts("[EKERN]     DEpocCodeSeg::DoCreate...\n");
	  int r = __real__ZN12DEpocCodeSeg8DoCreateER18TCodeSegCreateInfoP8DProcess(a,b,c);
	  ekseam_puts("[EKERN]     DEpocCodeSeg::DoCreate done r="); ekseam_puthex((unsigned)r); return r; }

extern "C" void __real__ZN1M5Init2Ev();
extern "C" void __wrap__ZN1M5Init2Ev()
	{ ekseam_puts("[EKERN]   M::Init2...\n"); __real__ZN1M5Init2Ev();
	  ekseam_puts("[EKERN]   M::Init2 done\n"); }
extern "C" void __real__ZN1A5Init2Ev();
extern "C" void __wrap__ZN1A5Init2Ev()
	{ ekseam_puts("[EKERN]   A::Init2...\n"); __real__ZN1A5Init2Ev();
	  ekseam_puts("[EKERN]   A::Init2 done\n"); }
extern "C" int __real__ZN1K22CreateObjectContainersEv();
extern "C" int __wrap__ZN1K22CreateObjectContainersEv()
	{ ekseam_puts("[EKERN]   K::CreateObjectContainers...\n");
	  int r = __real__ZN1K22CreateObjectContainersEv();
	  ekseam_puts("[EKERN]   K::CreateObjectContainers done\n"); return r; }

// DThread::Exit() -- the system-critical initial thread should NEVER exit during boot; it does,
// raising K::ESystemThreadPanic. Print WHO called Exit (caller LR) to find the real trigger.
extern "C" void __real__ZN7DThread4ExitEv(void*);
extern "C" void __wrap__ZN7DThread4ExitEv(void* t)
	{
	ekseam_puts("\n[EKERN] *** DThread::Exit called from LR="); ekseam_puthex((unsigned)__builtin_return_address(0));
	if (t)
		{	// (2026-10-09, the NGA boot death: a thread exit here was the last log line before the restart)
		DThread* th = (DThread*)t;
		TBuf8<96> fn;
		th->FullName(fn);
		fn.Append('\0');
		ekseam_puts("[EKERN]     exiting='"); ekseam_puts((const char*)fn.Ptr()); ekseam_puts("'\n");
		ekseam_puts("[EKERN]     type="); ekseam_puthex((unsigned)th->iExitType);
		ekseam_puts("[EKERN]     reason="); ekseam_puthex((unsigned)th->iExitReason);
		ekseam_puts("[EKERN]     flags="); ekseam_puthex((unsigned)th->iFlags);
		ekseam_puts("[EKERN]     process flags="); ekseam_puthex(th->iOwningProcess ? (unsigned)th->iOwningProcess->iFlags : 0u);
#ifdef EKSEAM_FLUSH_ON_PROC
		if (ekseam_board_panic) ekseam_board_panic();
#endif
		}
	__real__ZN7DThread4ExitEv(t);
	}

// ExecHandler::LeaveStart -- every User::Leave enters here (debug, KDEFS=-DEKSEAM_FLUSH_ON_PROC; 2026-10-09: the NGA
// window server's CWsTop::RunServerL left with no trace). For the window server process only, bounded: the user sp +
// the top 12 words of its stack (User::Leave pushes the reason's callee-saved register + its caller's return address).
extern "C" void* __real__ZN11ExecHandler10LeaveStartEv();
extern "C" void* __wrap__ZN11ExecHandler10LeaveStartEv()
	{
#ifdef EKSEAM_FLUSH_ON_PROC
	static TInt n;
	DThread* th = TheCurrentThread;
	DProcess* p = th ? th->iOwningProcess : NULL;
	if (p && n < 40)
		{
		TBuf8<96> fn;
		p->Name(fn);
		if (fn.Length() >= 5 && fn.Left(5).CompareF(_L8("EWSRV")) == 0)
			{
			++n;
			unsigned uregs[2] = {0, 0};
			asm volatile("stmia %0, {sp, lr}^" : : "r"(uregs) : "memory");
			ekseam_puts("[dbg] LEAVE in EWSRV sp_usr="); ekseam_puthex(uregs[0]);
			ekseam_puts("[dbg]   lr_usr="); ekseam_puthex(uregs[1]);
			const unsigned sp = uregs[0];
			if (sp >= 0x00400000u && sp < 0x40000000u && !(sp & 3))
				{
				const unsigned first = (0x1000u - (sp & 0xFFFu)) / 4;
				for (unsigned i = 0; i < 12 && i < first; ++i)
					{
					unsigned w = 0;
					if (K::USafeRead((const TAny*)(sp + i * 4), &w, 4)) { ekseam_puts("[dbg]   (unreadable)\n"); break; }
					ekseam_puts("[dbg]   ustk "); ekseam_puthex(w);
					}
				}
			if (ekseam_board_panic) ekseam_board_panic();
			}
		}
#endif
	return __real__ZN11ExecHandler10LeaveStartEv();
	}

// ExecHandler::PopTrapFrame -- the leave of THIS euser (no __LEAVE_EQUALS_THROW__: uc_trp.cia User::Leave saves
// aReason in r4 and pops the trap frame here; TTrap::UnTrap pops too, at every normal TRAP exit). Debug
// (EKSEAM_FLUSH_ON_PROC), window server process only: the user r4 from the SWI frame (top of the supervisor stack,
// nkernsmp vectors.cia saves r0-r14_usr) -- a value in -1..-60 = a leave code; with lr_usr = the return into
// User::Leave and pc, both mappable with the ROM symbols.
extern "C" void* __real__ZN11ExecHandler12PopTrapFrameEv();
extern "C" void* __wrap__ZN11ExecHandler12PopTrapFrameEv()
	{
#ifdef EKSEAM_FLUSH_ON_PROC
	static TInt n;
	DThread* th = TheCurrentThread;
	DProcess* p = th ? th->iOwningProcess : NULL;
	if (p && n < 30 && th->iNThread.iStackBase)
		{
		const SThreadExcStack* f = (const SThreadExcStack*)((TLinAddr)th->iNThread.iStackBase + th->iNThread.iStackSize) - 1;
		const TInt r4 = (TInt)f->iR4;
		if (ETrue)		// (was: r4 in -1..-60 -- nothing matched; log every pop of the first 30)
			{
			TBuf8<96> fn;
			p->Name(fn);
			if (fn.Length() >= 5 && fn.Left(5).CompareF(_L8("EWSRV")) == 0)
				{
				++n;
				ekseam_puts("[dbg] LEAVE? EWSRV r4="); ekseam_puthex((unsigned)r4);
				ekseam_puts("[dbg]   lr_usr="); ekseam_puthex(f->iR14usr);
				ekseam_puts("[dbg]   pc="); ekseam_puthex(f->iR15);
				const unsigned sp = f->iR13usr;
				ekseam_puts("[dbg]   sp_usr="); ekseam_puthex(sp);
				if (sp >= 0x00400000u && sp < 0x40000000u && !(sp & 3))
					{
					const unsigned first = (0x1000u - (sp & 0xFFFu)) / 4;
					for (unsigned i = 0; i < 16 && i < first; ++i)
						{
						unsigned w = 0;
						if (K::USafeRead((const TAny*)(sp + i * 4), &w, 4)) break;
						if (w >= 0x80000000u && w < 0x88000000u)
							{ ekseam_puts("[dbg]   ustk "); ekseam_puthex(w); }
						}
					}
				if (ekseam_board_panic) ekseam_board_panic();
				}
			}
		}
#endif
	return __real__ZN11ExecHandler12PopTrapFrameEv();
	}

// DObject::SetName(const TDesC8*) -- DProcess::Create calls this with the process name; the
// .cia Kern::ValidateName panics (Des8PanicBadDesType) if the descriptor type nibble >=5.
// Dump the name pointer + first word (length|type<<28) + head bytes to see if it's malformed.
extern "C" int __real__ZN7DObject7SetNameEPK6TDesC8(void*, void*);
extern "C" int __wrap__ZN7DObject7SetNameEPK6TDesC8(void* obj, void* aName)
	{
	ekseam_puts("\n[EKERN] SetName aName="); ekseam_puthex((unsigned)(unsigned long)aName);
	if (aName)
		{
		unsigned w0 = *(volatile unsigned*)aName;
		ekseam_puts("[EKERN]   desc word0(len|type)="); ekseam_puthex(w0);
		ekseam_puts("[EKERN]   type nibble="); ekseam_puthex(w0>>28);
		}
	int r = __real__ZN7DObject7SetNameEPK6TDesC8(obj, aName);
	ekseam_puts("[EKERN] SetName done r="); ekseam_puthex((unsigned)r);
	return r;
	}

// NKern::ThreadKill(NThread*) -- the nano-thread kill entry (EXPORT_C, called from ekern's
// DThread::Die). Something kills the INITIAL thread during DProcess::Create -> ESystemThreadPanic.
// Print the caller LR to find the trigger site.
extern "C" void __real__ZN5NKern10ThreadKillEP7NThread(void*);
extern "C" void __wrap__ZN5NKern10ThreadKillEP7NThread(void* t)
	{
	ekseam_puts("\n[EKERN] *** NKern::ThreadKill(NThread*) from LR="); ekseam_puthex((unsigned)(unsigned long)__builtin_return_address(0));
	__real__ZN5NKern10ThreadKillEP7NThread(t);
	}
// Also DThread::Die(TExitType,TInt,const TDesC8&) -- the ekern-level thread death entry.
extern "C" void __real__ZN7DThread3DieE9TExitTypeiRK6TDesC8(void*, int, int, void*);
extern "C" void __wrap__ZN7DThread3DieE9TExitTypeiRK6TDesC8(void* a, int b, int c, void* d)
	{
	ekseam_puts("\n[EKERN] *** DThread::Die from LR="); ekseam_puthex((unsigned)(unsigned long)__builtin_return_address(0));
	// DObject layout: vptr+0, iAsyncDeleteNext+4 (DBase), iAccessCount+8, iOwner+12,
	// 4 packed TUint8s +16, HBuf* iName+20. HBuf word0 = len|type<<28, chars at +4.
	if (a)
		{
		// DThread::FullName ("process::thread") -- the hand-coded DObject offset (+20 = iName) was the 2010 UP
		// layout; on the SMP/flexible kernel it read an empty name for every death (2026-10-07)
		TBuf8<96> fn;
		((DThread*)a)->FullName(fn);
		fn.Append('\0');
		ekseam_puts("[EKERN]     thread='");
		ekseam_puts((const char*)fn.Ptr());
		{	// the process name (up to its "[uid]") for the board's notification below
		TInt k = 0;
		for (; k < fn.Length() - 1 && k < 31 && fn[k] != '[' && fn[k] != ':'; ++k) DieName[k] = (char)fn[k];
		DieName[k] = 0;
		}
		ekseam_puts("'\n");
		}
#ifdef EKSEAM_DIE_TRACE
	// DIE_TRACE=<n> (link_ekern.sh): also dump the user stack for the n-th thread death of the boot
	// even when it is not a panic -- answers "who called User::Exit / exit()?". One chosen death
	// only: kernel threads die too, and their banked sp_usr is stale (reading it faults the kernel).
	static unsigned deaths;
	if (b == 2 || ++deaths == EKSEAM_DIE_TRACE)
#elif defined(EKSEAM_DIE_TRACE_NAME)
	// DIE_TRACE_NAME=<process name> (link_ekern.sh): dump the user stack whenever a thread of that process dies, panic or not
	// -- "where does this app call User::Exit?" (2026-10-12: X-plore's loader exits 0 silently). Only user processes are
	// named here, so sp_usr is the thread's own (the kernel-thread caveat above does not apply).
	static const char KDieTraceName[] = EKSEAM_DIE_TRACE_NAME;
	TBool named = ETrue;
	for (TInt k = 0; named && (KDieTraceName[k] || DieName[k]); ++k)
		named = KDieTraceName[k] == DieName[k];
	if (b == 2 || named)
#else
	if (b == 2 /*EExitPanic*/)
#endif
		{
		// Banked user-mode sp/lr readable from SVC via the ^ form. lr_usr is a return
		// address inside the user-side panic call chain; then dump the top of the user
		// stack (bounded) so return addresses can be matched against the link maps.
		unsigned uregs[2] = {0, 0};
		asm volatile("stmia %0, {sp, lr}^" : : "r"(uregs) : "memory");
		ekseam_puts("[EKERN]     sp_usr="); ekseam_puthex(uregs[0]);
		ekseam_puts("[EKERN]     lr_usr="); ekseam_puthex(uregs[1]);
		// the thread's own user stack (base = lowest address): "sp below base" = overflow, and by how much (2026-10-10:
		// the sensor server's SSY thread died at base-0x98 with a 4 KB stack -- the size is CenRep 0x1020507E key 2)
		ekseam_puts("[EKERN]     ustack_base="); ekseam_puthex((unsigned)((DThread*)a)->iUserStackRunAddress);
		ekseam_puts("[EKERN]     ustack_size="); ekseam_puthex((unsigned)((DThread*)a)->iUserStackSize);
		unsigned sp = uregs[0];
		if (sp >= 0x00400000u && sp < 0x40000000u && !(sp & 3))
			{
			// bounded to sp's own page too (2026-10-06): c32exe died with sp 0xa8 below the page end, the 48-word read
			// crossed into the unmapped page and USafeRead's magic recovery does not hold on this death path -- the
			// whole kernel faulted (seen only once the PinePhone Phorepoc ROM got c32exe running)
			const unsigned first = (0x1000u - (sp & 0xFFFu)) / 4;
			for (unsigned i = 0; i < 48 && i < first; ++i)
				{
				unsigned w = 0;
				if (K::USafeRead((const TAny*)(sp + i * 4), &w, 4)) break;	// unmapped (stack overflow: sp sits in the guard page)
				if (w >= 0x80000000u && w < 0x88000000u)	// XIP ROM code (the PinePhone S60 ROM reaches ~0x8620_0000 with Qt 4.8 + QtDeclarative)
					{
					ekseam_puts("[EKERN]     ustk[+"); ekseam_puthex(i * 4);
					ekseam_puts("]="); ekseam_puthex(w);
					}
				}
			// Raw dump of the caller frames above (bounded): the panicking TParse local
			// (field table + UTF-16 name buffer) lives here. Non-zero words only.
			// Stay inside the thread's stack page: never cross the next 4K boundary
			// past 0x1000 bytes (a small 0x2000 stack ends unmapped -> kernel dabt).
			unsigned limit = (0x1000u - (sp & 0xFFFu)) / 4;
			if (limit > 0x400) limit = 0x400;
			unsigned printed = 0;
			for (unsigned i = 0; i < limit && printed < 300; ++i)
				{
				unsigned w = 0;
				if (K::USafeRead((const TAny*)(sp + i * 4), &w, 4))
					{
					ekseam_puts("[EKERN]     (user stack unreadable at sp -- stack overflow into the guard page?)\n");
					break;
					}
				if (w && w != 0x29292929u)
					{
					ekseam_puts("[EKERN] u+"); ekseam_puthex(i * 4);
					ekseam_puts("="); ekseam_puthex(w);
					++printed;
					}
				}
			}
		}
	ekseam_puts("[EKERN]     exit type=");  ekseam_puthex((unsigned)b);   // 0=Kill 1=Terminate 2=Panic
	ekseam_puts("[EKERN]     reason=");     ekseam_puthex((unsigned)c);
	if (d)
		{                                    // aCategory: an 8-bit descriptor (word0 = len|type)
		unsigned w0 = *(volatile unsigned*)d;
		unsigned len = w0 & 0x0FFFFFFF;
		// type 0 (TBufC/TLitC): chars inline after word0; type 1 (TPtrC): the
		// char POINTER is the second word.
		// type 2 (TPtr): pointer is the THIRD word; type 3 (TBuf, what a user-side
		// User::Panic arrives as): chars inline after len+maxlen; type 4 (EBufCPtr):
		// third word -> TBufCBase (chars after its length word).
		unsigned ty = w0 >> 28;
		const char* p = ty == 1 ? *(const char* volatile*)((const char*)d + 4)
		              : ty == 2 ? *(const char* volatile*)((const char*)d + 8)
		              : ty == 3 ? (const char*)d + 8
		              : ty == 4 ? *(const char* volatile*)((const char*)d + 8) + 4
		                        : (const char*)d + 4;
		char buf[24]; unsigned i;
		for (i = 0; i < len && i < sizeof(buf) - 1; ++i) buf[i] = p[i];
		buf[i] = 0;
		ekseam_puts("[EKERN]     category='"); ekseam_puts(buf); ekseam_puts("'\n");
		if (b == 2 && ekseam_board_notify)
			{	// "Panic Telephone KERN-EXEC 3" -> the board's status bar (PinePhone: a64_bars.cpp)
			char t[64]; unsigned n = 0;
			const char* parts[3] = { "Panic ", DieName, " " };
			for (int q = 0; q < 3; ++q)
				for (const char* s = parts[q]; *s && n < 40; ++s) t[n++] = *s;
			for (const char* s = buf; *s && n < 56; ++s) t[n++] = *s;
			t[n++] = ' ';
			unsigned v = c < 0 ? (unsigned)-c : (unsigned)c;
			if (c < 0) t[n++] = '-';
			char dg[12]; int m = 0;
			do { dg[m++] = (char)('0' + v % 10); v /= 10; } while (v && m < 11);
			while (m && n < 63) t[n++] = dg[--m];
			t[n] = 0;
			ekseam_board_notify(3, t);
			}
		}
	// a panic is often the last thing before a reboot (a system-critical process): the board writes its log out NOW
	// (PinePhone: the SD log, otherwise flushed on a timer that the reset beats; 2026-10-07)
	if (b == 2 && ekseam_board_panic)
		ekseam_board_panic();
	__real__ZN7DThread3DieE9TExitTypeiRK6TDesC8(a,b,c,d);
	}

// InvalidExecHandler -- the slot the exec dispatcher jumps to for an out-of-range
// or invalid SWI. At the jump (vectors.cia slow_swi "ldrls pc,[r4,#-8]") r12 still
// holds swi#<<9 and lr = slow_swi_exit; the USER return address is in the saved
// frame. Print the SWI number BEFORE the KERN-EXEC 27 panic so the failing exec
// call is named. Naked: grab r12 before the C prologue can spill it.
extern "C" void ekseam_invalid_exec_report(unsigned r12val)
	{
	ekseam_puts("\n[EKERN] *** InvalidExecHandler: swi#="); ekseam_puthex(r12val >> 9);
	}
extern "C" void __real__Z18InvalidExecHandlerv();
extern "C" __attribute__((naked)) void __wrap__Z18InvalidExecHandlerv()
	{
	asm volatile(
		"stmfd sp!, {r0-r3, lr}\n\t"
		"mov r0, r12\n\t"
		"bl ekseam_invalid_exec_report\n\t"
		"ldmfd sp!, {r0-r3, lr}\n\t"
		"b __real__Z18InvalidExecHandlerv\n\t");
	}

// K::Fault(aFault) -- the KERN-category fault entry. Print the fault reason AND the caller's
// return address (the real fault site's PC), so `nm` maps it to the function that raised it.
extern "C" void __real__ZN1K5FaultENS_6TFaultE(int);
extern "C" void __wrap__ZN1K5FaultENS_6TFaultE(int aFault)
	{
	// the line FIRST, then stage 9 (the board's fault flush): 2026-10-09 the flush never landed and the line was lost
	ekseam_puts("\n[EKERN] *** K::Fault reason="); ekseam_puthex((unsigned)aFault);
	ekseam_puts("[EKERN]     called from LR="); ekseam_puthex((unsigned)(unsigned long)__builtin_return_address(0));
	{	// the faulting context (DThread::Exit: the exiting thread itself), compact: "<process>::<thread> <category>
		// <reason>" -- the LAST line before stage 9, which the A64 board keeps in its RTC tail across the restart
	DThread* th = TheCurrentThread;
	if (th)
		{
		TBuf8<96> fn;
		th->FullName(fn);
		TBuf8<64> s;
		for (TInt i = 0; i < fn.Length() && s.Length() < 40; ++i)
			{
			if (fn[i] == '[') { while (i < fn.Length() && fn[i] != ':') ++i; --i; continue; }
			s.Append(fn[i]);
			}
		s.Append(' ');
		for (TInt i = 0; i < th->iExitCategory.Length() && s.Length() < 54; ++i)
			s.Append(th->iExitCategory[i]);
		s.Append(' ');
		s.AppendNum(th->iExitReason);
		s.Append(' ');
		s.AppendNum((TInt)th->iExitType);
		s.Append('\n');
		s.Append('\0');
		ekseam_puts("[EKERN] fault in ");
		ekseam_puts((const char*)s.Ptr());
		}
	}
	ekseam_stage(9);
	ekseam_dump_cs("[EKERN][cs] at K::Fault:");
	__real__ZN1K5FaultENS_6TFaultE(aFault);
	}

// Kern::Fault(category, reason) -- catch kernel faults and print the category (the crash
// monitor is stubbed, so a fault otherwise just halts silently).
extern "C" void __real__ZN4Kern5FaultEPKci(const char*, int);
extern "C" void __wrap__ZN4Kern5FaultEPKci(const char* cat, int reason)
	{
	ekseam_puts("\n[EKERN] *** Kern::Fault: ");
	ekseam_puts(cat ? cat : "(null)");
	ekseam_puts(" ***\n");
	ekseam_stage(9);
	__real__ZN4Kern5FaultEPKci(cat, reason);
	}

// DThread::EpocThreadFunction(TAny*) -- the FIRST code a newly-created thread runs (nkern
// sets NThread::iFunction = EpocThreadFunction; the first-run trampoline calls it). If the
// secondary USER thread is scheduled at all, this fires. It is __declspec(naked) with a strict
// entry contract (r0->creation info == sp, r4->current NThread, r11->top of sup stack), so we
// CANNOT use a C wrap; instead a fully register-transparent asm trampoline prints a marker
// ('[U]') straight to the UART (saving/restoring everything, incl. sp) then tail-calls the real
// one. Answers: is the user thread ever scheduled? (marker present) vs faults on user-mode
// transition (marker present but no [USERLAND]/DebugPrint) vs never scheduled (no marker).
// Dump the just-started thread's user-execution fields from its NThread pointer (r4 at entry).
// Answers whether the user stack + entry are sane before EpocThreadFunction writes the user stack.
// Section-map phys-peek: read a 32-bit word at PHYSICAL address `phys` by temporarily writing a
// 1MB SECTION PDE (scratch kernel VA -> phys, non-cacheable) into the kernel page directory, doing
// a TLB flush, reading, then restoring the PDE. Non-cacheable so we see the fresh page-table memory
// the kernel wrote. Used to read PTEs (the page tables aren't trivially addressable by VA here).
// `pdLin` = kernel linear address of the page directory the CPU currently uses for the scratch VA.
// The scratch VA (0x30000000) is in the LOCAL range (< 1GB, below any TTBRC split), so it is
// resolved via TTBR0 = the current process PD; write the temp section PDE into THAT PD's linear map.
extern "C" unsigned ekseam_phys_peek(unsigned phys, unsigned pdLin)
	{
	const unsigned scratchVA = 0x30000000u;             // local range, unused by this minimal process
	volatile unsigned* pd = (volatile unsigned*)pdLin;
	unsigned idx = scratchVA >> 20;                     // 0x300
	unsigned saved = pd[idx];
	pd[idx] = (phys & 0xFFF00000u) | 0x00000C02u;       // section, AP=11 (svr rw), domain0, C=B=0
	__asm__ volatile("mcr p15,0,%0,c7,c10,4" :: "r"(0)); // DSB
	__asm__ volatile("mcr p15,0,%0,c8,c7,0"  :: "r"(0)); // TLBIALL
	unsigned val = *(volatile unsigned*)(scratchVA + (phys & 0x000FFFFFu));
	pd[idx] = saved;
	__asm__ volatile("mcr p15,0,%0,c7,c10,4" :: "r"(0));
	__asm__ volatile("mcr p15,0,%0,c8,c7,0"  :: "r"(0));
	return val;
	}

extern "C" void ekseam_dump_thread(void* aNThread)
	{
	DThread* t = _LOFF(aNThread, DThread, iNThread);
	if (t->iThreadType != EThreadUser) return;   // only trace the USER secondary; kernel threads are noise
	NThreadBase* nt = (NThreadBase*)aNThread;
	ekseam_puts("\n[EKERN] *** EpocThreadFunction: USER thread first-run ***\n");
	ekseam_puts("[EKERN][thr] NThr.Attributes(iSpare2)="); ekseam_puthex((unsigned)nt->Attributes());
	ekseam_puts("[EKERN][thr] NThr.iAddressSpace=");       ekseam_puthex((unsigned)(unsigned long)nt->iAddressSpace);
	ekseam_puts("[EKERN][thr] iThreadType=");   ekseam_puthex((unsigned)t->iThreadType);
	ekseam_puts("[EKERN][thr] iUserStackRun="); ekseam_puthex((unsigned)t->iUserStackRunAddress);
	ekseam_puts("[EKERN][thr] iUserStackSize=");ekseam_puthex((unsigned)t->iUserStackSize);
	ekseam_puts("[EKERN][thr] iFlags=");        ekseam_puthex((unsigned)t->iFlags);
	DProcess* p = t->iOwningProcess;
	ekseam_puts("[EKERN][thr] iOwningProcess=");ekseam_puthex((unsigned)(unsigned long)p);
	if (p && p->iCodeSeg)
		{
		ekseam_puts("[EKERN][thr] iCodeSeg=");      ekseam_puthex((unsigned)(unsigned long)p->iCodeSeg);
		ekseam_puts("[EKERN][thr] iEntryPtVeneer=");ekseam_puthex((unsigned)p->iCodeSeg->iEntryPtVeneer);
		}
	// Address-space check: confirm the process page dir is loaded into TTBR0 / ContextID (the
	// 2026-09-18 __MEMMODEL_MULTIPLE__/DoProcessSwitch fix). Once TTBR0 is the user PD, the user
	// stack + code + high vectors are all mapped -- the from-source kernel enters user mode from
	// here and minisec's swi 0x56 round-trips through the SVC/exec path. (The former page-table /
	// vector / phys-peek probes that root-caused this live in git history + ekern-boot-trace.txt;
	// ekseam_phys_peek is kept above as a reusable MMU-debug tool.)
	unsigned ttbr0, ctxid;
	__asm__ volatile("mrc p15,0,%0,c2,c0,0" : "=r"(ttbr0));
	__asm__ volatile("mrc p15,0,%0,c13,c0,1" : "=r"(ctxid));
	ekseam_puts("[EKERN][thr] TTBR0=");           ekseam_puthex(ttbr0);
	ekseam_puts("[EKERN][thr] ContextID(ASID)="); ekseam_puthex(ctxid);
	ekseam_puts("[EKERN] entering real EpocThreadFunction -> user mode\n");
	ekseam_stage(4);
	}

extern "C" void __real__ZN7DThread18EpocThreadFunctionEPv(void*);
extern "C" __attribute__((naked, used)) void __wrap__ZN7DThread18EpocThreadFunctionEPv(void*)
	{
	__asm__ volatile(
		"stmfd sp!, {r0-r12, lr}\n"     // scratch below sp (creation info is at/above sp)
		"mov   r0, r4\n"               // r4 = current NThread -> dump (user threads only)
		"bl    ekseam_dump_thread\n"
		"ldmfd sp!, {r0-r12, lr}\n"
		"b     __real__ZN7DThread18EpocThreadFunctionEPv\n"
		".ltorg\n");
	}

// ExcFault(TAny* aExcInfo) -- the C entry the ARM abort/undef vector handlers call once they
// have built a TArmExcInfo on the supervisor stack (vectors.cia -> b ExcFault). If the user
// thread's user-mode entry / user-stack fill takes a DATA/PREFETCH/UNDEF exception (rather than
// a genuine reset), THIS fires with the exact type + faulting PC + fault address. Offsets into
// TArmExcInfo (kernel/arm/arm.h): iCpsr@0, iExcCode@4 (0=pabt,1=dabt,2=undef), iFaultAddress@48,
// iFaultStatus@52, iR15(faulting PC)@88.
// Exc::Dispatch(TAny* aPtr, NThread*) -- the USER-thread exception path (ckernel.cpp).
// aPtr is the TArmExcInfo (non-SMP layout, arm.h): iExcCode@+4 (0=pabt,1=dabt,2=undef),
// iFaultAddress@+48, iFaultStatus@+52, iR15@+88. One bounded print per exception, BEFORE
// the kernel turns it into a KERN-EXEC 3 panic, so the aborting PC/address are visible.
#ifdef __SMP__
// SWP/SWPB emulation (PinePhone, 2026-10-06): ARMv8-A removed SWP from AArch32 -- UNDEFINED on the real Cortex-A53
// (QEMU's A53 model still executes it, so the S60 ROM ran there). The 2010 userland uses it in its lock primitives
// (euser: `swp r1, r1, [r0]; subs r1, #1; strlt`, the first user process died on it). Linux has the same problem
// and the same answer (arch/arm/kernel/swp_emulate.c): on a user UNDEF whose instruction is SWP, do the swap with
// an exclusive load/store loop and step the PC. SMP TArmExcInfo: r0..r12 = words 6..18, r13 = 19, r14 = 20,
// iExcCode = 21, r15 = 22, cpsr = 23.
static unsigned SwpEmulated;
static bool CondPasses(unsigned aCond, unsigned aCpsr)
	{
	const bool n = aCpsr & (1u << 31), z = aCpsr & (1u << 30), c = aCpsr & (1u << 29), v = aCpsr & (1u << 28);
	switch (aCond >> 1)
		{
		case 0: return z == !(aCond & 1);
		case 1: return c == !(aCond & 1);
		case 2: return n == !(aCond & 1);
		case 3: return v == !(aCond & 1);
		case 4: return (c && !z) == !(aCond & 1);
		case 5: return (n == v) == !(aCond & 1);
		case 6: return (!z && n == v) == !(aCond & 1);
		default: return true;
		}
	}
static bool EmulateSwp(volatile unsigned* e)
	{
	if (e[21] != 2 || (e[23] & (1u << 5)))		// undef only, ARM state only
		return false;
	const unsigned pc = e[22];
	const unsigned instr = *(volatile unsigned*)pc;
	if ((instr & 0x0FB00FF0u) != 0x01000090u)
		return false;
	const unsigned cond = instr >> 28, rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF, rm = instr & 0xF;
	const bool byte = instr & (1u << 22);
	if (rd == 15 || rn == 15 || rm == 15)
		return false;
	unsigned reg[16];
	for (int i = 0; i < 13; ++i) reg[i] = e[6 + i];
	reg[13] = e[19]; reg[14] = e[20];
	if (CondPasses(cond, e[23]))
		{
		const unsigned addr = reg[rn], newv = reg[rm];
		unsigned old, fail;
		if (byte)
			do { __asm__ volatile("ldrexb %0, [%2]\n strexb %1, %3, [%2]" : "=&r"(old), "=&r"(fail) : "r"(addr), "r"(newv) : "memory"); } while (fail);
		else
			{
			if (addr & 3) return false;
			do { __asm__ volatile("ldrex %0, [%2]\n strex %1, %3, [%2]" : "=&r"(old), "=&r"(fail) : "r"(addr), "r"(newv) : "memory"); } while (fail);
			}
		__asm__ volatile("dmb" ::: "memory");
		if (rd < 13) e[6 + rd] = old; else if (rd == 13) e[19] = old; else e[20] = old;
		}
	e[22] = pc + 4;
	if (++SwpEmulated <= 4 || (SwpEmulated & 0xFFF) == 0)
		{ ekseam_puts("[EKERN][swp] emulated SWP at pc="); ekseam_puthex(pc); ekseam_puts("[EKERN][swp]   count="); ekseam_puthex(SwpEmulated); }
	return true;
	}

// Misaligned LDRD/STRD/LDM/STM emulation (PinePhone, 2026-10-26). SCTLR.A is off (a64/config.inc), so LDR/STR/LDRH/STRH
// work on any address, but the doubleword and multiple forms still need word alignment. The 2010 userland was built by
// RVCT, which reads a 64-bit member as two LDRs; g++ uses LDRD/LDM, and some code reads records that sit only 2-aligned
// in a buffer (MdS: mdcserializationbuffer pads to even sizes; the harvester and the MPX collection server died in
// CMdEProperty::NewLC, fsr 1). Linux answers the same way (arch/arm/mm/alignment.c): on a user alignment fault whose
// instruction is one of these, do the access bytewise and step the PC. Each touched page is checked with ATS1CUR/W
// first, so a bad address still ends in the normal KERN-EXEC 3.
static unsigned AlignEmulated;
static bool UserCanAccess(unsigned aAddr, bool aWrite)
	{
	unsigned par;
	if (aWrite)
		__asm__ volatile("mcr p15, 0, %1, c7, c8, 3\n isb\n mrc p15, 0, %0, c7, c4, 0" : "=r"(par) : "r"(aAddr) : "memory");
	else
		__asm__ volatile("mcr p15, 0, %1, c7, c8, 2\n isb\n mrc p15, 0, %0, c7, c4, 0" : "=r"(par) : "r"(aAddr) : "memory");
	return !(par & 1);
	}
static bool UserRangeOk(unsigned aAddr, unsigned aLen, bool aWrite)
	{
	return UserCanAccess(aAddr, aWrite) && UserCanAccess(aAddr + aLen - 1, aWrite);
	}
static unsigned LoadWord(unsigned aAddr)
	{
	const volatile unsigned char* p = (const volatile unsigned char*)aAddr;
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
	}
static void StoreWord(unsigned aAddr, unsigned aValue)
	{
	volatile unsigned char* p = (volatile unsigned char*)aAddr;
	p[0] = aValue; p[1] = aValue >> 8; p[2] = aValue >> 16; p[3] = aValue >> 24;
	}
static unsigned GetReg(volatile unsigned* e, unsigned r, unsigned aPc)
	{
	return r < 13 ? e[6 + r] : r == 13 ? e[19] : r == 14 ? e[20] : aPc + 8;
	}
static void SetReg(volatile unsigned* e, unsigned r, unsigned v)
	{
	if (r < 13) e[6 + r] = v; else if (r == 13) e[19] = v; else if (r == 14) e[20] = v; else e[22] = v;
	}
static bool EmulateAlign(volatile unsigned* e)
	{
	// a data abort, alignment fault (short-descriptor FS = 0b00001), from user mode in ARM state
	if (e[21] != 1 || (e[1] & 0x40F) != 1 || (e[23] & 0x1F) != 0x10 || (e[23] & (1u << 5)))
		return false;
	const unsigned pc = e[22];
	if (!UserCanAccess(pc, false))
		return false;
	const unsigned instr = *(volatile unsigned*)pc;
	if ((instr >> 28) == 0xF)
		return false;
	const unsigned rn = (instr >> 16) & 0xF;
	const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
	unsigned base = GetReg(e, rn, pc);
	bool pcLoaded = false;
	if ((instr & 0x0E1000D0u) == 0x000000D0u)	// LDRD (bits 6:5 = 10) / STRD (11), immediate or register offset
		{
		const bool store = instr & (1u << 5);
		const unsigned rt = (instr >> 12) & 0xF;
		if ((rt & 1) || rt == 14 || rn == 15)
			return false;
		const unsigned off = (instr & (1u << 22)) ? (((instr >> 4) & 0xF0) | (instr & 0xF)) : GetReg(e, instr & 0xF, pc);
		const unsigned moved = u ? base + off : base - off;
		const unsigned addr = p ? moved : base;
		if (!UserRangeOk(addr, 8, store))
			return false;
		if (store)
			{ StoreWord(addr, GetReg(e, rt, pc)); StoreWord(addr + 4, GetReg(e, rt + 1, pc)); }
		else
			{ const unsigned lo = LoadWord(addr), hi = LoadWord(addr + 4); SetReg(e, rt, lo); SetReg(e, rt + 1, hi); }
		if (!p || w)
			SetReg(e, rn, moved);
		}
	else if ((instr & 0x0E000000u) == 0x08000000u)	// LDM / STM (not the user-bank S forms)
		{
		if (instr & (1u << 22))
			return false;
		const bool load = instr & (1u << 20);
		const unsigned list = instr & 0xFFFF;
		unsigned n = 0;
		for (unsigned b = list; b; b &= b - 1) ++n;
		if (!n || rn == 15)
			return false;
		unsigned addr = u ? (p ? base + 4 : base) : (p ? base - 4 * n : base - 4 * n + 4);
		const unsigned end = u ? base + 4 * n : base - 4 * n;
		if (!UserRangeOk(addr, 4 * n, !load))
			return false;
		if (w)
			SetReg(e, rn, end);			// (a loaded base below then wins, as on the CPU)
		for (unsigned r = 0; r < 16; ++r)
			{
			if (!(list & (1u << r)))
				continue;
			if (load)
				{
				const unsigned v = LoadWord(addr);
				if (r == 15)
					{ e[22] = v & ~1u; if (v & 1) e[23] |= 1u << 5; pcLoaded = true; }
				else
					SetReg(e, r, v);
				}
			else
				StoreWord(addr, GetReg(e, r, pc));
			addr += 4;
			}
		}
	else
		return false;
	if (!pcLoaded)
		e[22] = pc + 4;
	if (++AlignEmulated <= 4 || (AlignEmulated & 0xFFF) == 0)
		{ ekseam_puts("[EKERN][align] emulated misaligned access at pc="); ekseam_puthex(pc); ekseam_puts("[EKERN][align]   count="); ekseam_puthex(AlignEmulated); }
	return true;
	}
#endif

#if defined(__SMP__) && defined(__CPU_HAS_VFP)
// VFP short vectors (Let's Golf 2's black screen, 2026-10-27). ARM11's VFPv2 runs a data-processing instruction over a
// vector of FPSCR.LEN+1 registers (bank-wise, FPSCR.STRIDE apart); RVCT game code uses it for matrix maths (bic/orr
// FPSCR.LEN, vmsr, vmul/vmla.f32 on 4 lanes, vmsr back). ARMv8's AArch32 has no short vectors: the Cortex-A53 drops the
// LEN/STRIDE write and runs every instruction on lane 0 only (the game's MVP came out with one value per column). The
// loader (efile's sf_lepoc patch, ph_vfpvec.cpp) turns each LEN-setting `vmsr fpscr, Rt` of a RAM-loaded ARM image into
// UDF #0x560<Rt>; here that trap interprets the instructions with vector semantics until the guest writes LEN back to
// 0 -- what the Cortex-A8-era VFP support code did in software. Single precision only; anything it does not know ends
// the interpretation at that instruction (lane-0 behaviour again, logged).
TInt HandleVFPOperation(TAny* aPtr);
static unsigned VfpVecTraps, VfpVecBails;
static inline unsigned VfpFpexc() { unsigned v; __asm__ volatile(".fpu vfp\n vmrs %0, fpexc" : "=r"(v)); return v; }
static inline unsigned VfpGetFpscr() { unsigned v; __asm__ volatile(".fpu vfp\n vmrs %0, fpscr" : "=r"(v)); return v; }
static inline void VfpSetFpscr(unsigned v) { __asm__ volatile(".fpu vfp\n vmsr fpscr, %0" : : "r"(v) : "memory"); }
static inline void VfpSave(unsigned* s) { __asm__ volatile(".fpu vfp\n vstmia %0, {s0-s31}" : : "r"(s) : "memory"); }
static inline void VfpLoad(const unsigned* s) { __asm__ volatile(".fpu vfp\n vldmia %0, {s0-s31}" : : "r"(s) : "memory"); }

// one scalar operation on the hardware (the user's FPSCR rounding / flush / cumulative flags apply): s0 = d, s1 = n, s2 = m
static unsigned VfpScalar(unsigned aKind, unsigned d, unsigned n, unsigned m)
	{
	unsigned r = d;
#define PH_VOP(ins) __asm__ volatile(".fpu vfp\n vmov s0, %1\n vmov s1, %2\n vmov s2, %3\n " ins "\n vmov %0, s0" \
	                                 : "=r"(r) : "r"(d), "r"(n), "r"(m) : "memory")
	switch (aKind)
		{
		case 0: PH_VOP("vmla.f32 s0, s1, s2"); break;
		case 1: PH_VOP("vmls.f32 s0, s1, s2"); break;
		case 2: PH_VOP("vnmls.f32 s0, s1, s2"); break;
		case 3: PH_VOP("vnmla.f32 s0, s1, s2"); break;
		case 4: PH_VOP("vmul.f32 s0, s1, s2"); break;
		case 5: PH_VOP("vnmul.f32 s0, s1, s2"); break;
		case 6: PH_VOP("vadd.f32 s0, s1, s2"); break;
		case 7: PH_VOP("vsub.f32 s0, s1, s2"); break;
		case 8: PH_VOP("vdiv.f32 s0, s1, s2"); break;
		case 9: r = m; break;
		case 10: PH_VOP("vabs.f32 s0, s2"); break;
		case 11: PH_VOP("vneg.f32 s0, s2"); break;
		case 12: PH_VOP("vsqrt.f32 s0, s2"); break;
		case 13: PH_VOP("vcmp.f32 s1, s2"); break;		// the flags land in FPSCR; d unchanged
		case 14: PH_VOP("vcmpe.f32 s1, s2"); break;
		}
#undef PH_VOP
	return r;
	}

static unsigned VfpVecReg(unsigned aReg, unsigned aIndex, unsigned aStride)
	{
	return (aReg & ~7u) | ((aReg + aIndex * aStride) & 7u);
	}

static bool EmulateVfpVector(volatile unsigned* e, void* aInfo)
	{
	if (e[21] != 2 || (e[23] & (1u << 5)) || (e[23] & 0x1F) != 0x10)
		return false;
	unsigned pc = e[22];
	if (!UserRangeOk(pc, 4, false) || (LoadWord(pc) & 0xFFFFFFF0u) != 0xE7F560F0u)
		return false;
	if (!(VfpFpexc() & (1u << 30)))
		{
		HandleVFPOperation(aInfo);			// load this thread's VFP context; the UDF traps again with VFP on
		return true;
		}
	NKern::Lock();
	if (!(VfpFpexc() & (1u << 30)))
		{
		NKern::Unlock();
		return true;
		}
	unsigned s[32];
	VfpSave(s);
	unsigned vbits = 0;						// the guest's LEN|STRIDE (the hardware FPSCR never holds them)
	bool done = false;
	const char* why = 0;
	for (int steps = 0; steps < 1024 && !done; ++steps)
		{
		if (!UserRangeOk(pc, 4, false))
			{ why = "pc"; break; }
		const unsigned op = LoadWord(pc);
		const bool udf = (op & 0xFFFFFFF0u) == 0xE7F560F0u;
		if (!udf && (op >> 28) != 0xE)
			{ why = "cond"; break; }
		if (udf || (op & 0x0FFF0FFFu) == 0x0EE10A10u)					// vmsr fpscr, Rt
			{
			const unsigned v = GetReg(e, udf ? (op & 0xF) : ((op >> 12) & 0xF), pc);
			vbits = v & 0x00370000u;
			VfpSetFpscr(v & ~0x00370000u);
			pc += 4;
			if (!vbits)
				done = true;
			continue;
			}
		if ((op & 0x0FFF0FFFu) == 0x0EF10A10u)							// vmrs Rt, fpscr
			{
			const unsigned v = VfpGetFpscr() | vbits, rt = (op >> 12) & 0xF;
			if (rt == 15)
				e[23] = (e[23] & 0x0FFFFFFFu) | (v & 0xF0000000u);
			else
				SetReg(e, rt, v);
			pc += 4;
			continue;
			}
		if ((op & 0x0F000000u) == 0x0A000000u)							// b
			{
			int off = op & 0xFFFFFF;
			if (off & 0x800000)
				off -= 0x1000000;
			pc = pc + 8 + off * 4;
			continue;
			}
		if ((op & 0x0FE00F7Fu) == 0x0E000A10u)							// vmov Sn <-> Rt
			{
			const unsigned sn = (((op >> 16) & 0xF) << 1) | ((op >> 7) & 1), rt = (op >> 12) & 0xF;
			if (rt == 15)
				{ why = "vmov pc"; break; }
			if (op & (1u << 20))
				SetReg(e, rt, s[sn]);
			else
				s[sn] = GetReg(e, rt, pc);
			pc += 4;
			continue;
			}
		if ((op & 0x0E000F00u) == 0x0C000A00u && (op & 0x01A00000u) != 0)		// vldm/vstm/vldr/vstr single
			{
			const bool p = op & (1u << 24), u = op & (1u << 23), w = op & (1u << 21), load = op & (1u << 20);
			const unsigned rn = (op >> 16) & 0xF, sd = (((op >> 12) & 0xF) << 1) | ((op >> 22) & 1), imm = op & 0xFF;
			unsigned base = rn == 15 ? (pc + 8) & ~3u : GetReg(e, rn, pc), addr, count;
			if (p && !w)
				{ addr = u ? base + imm * 4 : base - imm * 4; count = 1; }
			else if (!p && u)
				{ addr = base; count = imm; }
			else if (p && !u && w)
				{ addr = base - imm * 4; count = imm; }
			else
				{ why = "vldm form"; break; }
			if (!count || sd + count > 32 || !UserRangeOk(addr, count * 4, !load))
				{ why = "vldm range"; break; }
			for (unsigned i = 0; i < count; ++i)
				{
				if (load)
					s[sd + i] = LoadWord(addr + i * 4);
				else
					StoreWord(addr + i * 4, s[sd + i]);
				}
			if (w && !(p && !w))
				SetReg(e, rn, u ? base + imm * 4 : base - imm * 4);
			pc += 4;
			continue;
			}
		if ((op & 0x0F000F10u) == 0x0E000A00u)							// data processing, single precision
			{
			const unsigned sd = (((op >> 12) & 0xF) << 1) | ((op >> 22) & 1);
			const unsigned sn = (((op >> 16) & 0xF) << 1) | ((op >> 7) & 1);
			const unsigned sm = ((op & 0xF) << 1) | ((op >> 5) & 1);
			const unsigned pb = (op >> 23) & 1, qb = (op >> 21) & 1, rb = (op >> 20) & 1, sb = (op >> 6) & 1;
			int kind = -1;
			bool imm = false;
			if (!pb)
				kind = (int)((((qb << 1) | rb) << 1) | sb);
			else if (!qb && !rb && !sb)
				kind = 8;
			else if (qb && rb)
				{
				const unsigned opc2 = (op >> 16) & 0xF, b7 = (op >> 7) & 1;
				if (!sb)
					imm = true;
				else if (opc2 == 0)
					kind = b7 ? 10 : 9;
				else if (opc2 == 1)
					kind = b7 ? 12 : 11;
				else if (opc2 == 4 || opc2 == 5)
					{
					// vcmp is never vectorised: one compare, against Sm or +0.0
					VfpScalar(b7 ? 14 : 13, 0, s[sd], opc2 == 5 ? 0 : s[sm]);
					pc += 4;
					continue;
					}
				}
			if (kind < 0 && !imm)
				{ why = "vfp op"; break; }
			const unsigned len = (vbits >> 16) & 7, stride = ((vbits >> 20) & 3) == 3 ? 2 : 1;
			const unsigned n = (sd >> 3) == 0 ? 1 : len + 1;			// bank 0 destination = scalar
			const bool mScalar = (sm >> 3) == 0;
			unsigned immv = 0;
			if (imm)
				{
				const unsigned i8 = (((op >> 16) & 0xF) << 4) | (op & 0xF);
				immv = ((i8 & 0x80u) << 24) | ((i8 & 0x40u) ? 0x3E000000u : 0x40000000u) | ((i8 & 0x3Fu) << 19);
				}
			for (unsigned i = 0; i < n; ++i)
				{
				const unsigned di = VfpVecReg(sd, i, stride), ni = VfpVecReg(sn, i, stride);
				const unsigned mi = mScalar ? sm : VfpVecReg(sm, i, stride);
				s[di] = imm ? immv : VfpScalar((unsigned)kind, s[di], s[ni], s[mi]);
				}
			pc += 4;
			continue;
			}
		if ((op & 0x0C000000u) == 0 && ((op & (1u << 25)) || !(op & 0x10u)) && !(op & (1u << 20)))
			{																// integer data processing, no flags
			const unsigned opc = (op >> 21) & 0xF, rd = (op >> 12) & 0xF, rn = (op >> 16) & 0xF;
			unsigned b;
			if (op & (1u << 25))
				{
				const unsigned rot = ((op >> 8) & 0xF) * 2, i8 = op & 0xFF;
				b = rot ? (i8 >> rot) | (i8 << (32 - rot)) : i8;
				}
			else
				{
				const unsigned m = GetReg(e, op & 0xF, pc), sh = (op >> 7) & 0x1F, ty = (op >> 5) & 3;
				if (ty == 0) b = m << sh;
				else if (ty == 1) b = sh ? m >> sh : 0;
				else if (ty == 2) b = (unsigned)((int)m >> (sh ? sh : 31));
				else if (sh) b = (m >> sh) | (m << (32 - sh));
				else { why = "rrx"; break; }
				}
			const unsigned a = GetReg(e, rn, pc);
			unsigned r;
			switch (opc)
				{
				case 0: r = a & b; break;
				case 1: r = a ^ b; break;
				case 2: r = a - b; break;
				case 3: r = b - a; break;
				case 4: r = a + b; break;
				case 12: r = a | b; break;
				case 13: r = b; break;
				case 14: r = a & ~b; break;
				case 15: r = ~b; break;
				default: r = 0; why = "alu op"; break;
				}
			if (why)
				break;
			if (rd == 15)
				{ why = "alu pc"; break; }
			SetReg(e, rd, r);
			pc += 4;
			continue;
			}
		if ((op & 0x0F400000u) == 0x05000000u && !(op & (1u << 21)))		// ldr/str word, [Rn, #+-imm12]
			{
			const unsigned rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF, imm = op & 0xFFF;
			const unsigned base = rn == 15 ? (pc + 8) : GetReg(e, rn, pc);
			const unsigned addr = (op & (1u << 23)) ? base + imm : base - imm;
			const bool load = op & (1u << 20);
			if (rd == 15 || !UserRangeOk(addr, 4, !load))
				{ why = "ldr"; break; }
			if (load)
				SetReg(e, rd, LoadWord(addr));
			else
				StoreWord(addr, GetReg(e, rd, pc));
			pc += 4;
			continue;
			}
		why = "instruction";
		break;
		}
	if (!done)
		VfpSetFpscr(VfpGetFpscr() & ~0x00370000u);
	VfpLoad(s);
	e[22] = pc;
	NKern::Unlock();
	++VfpVecTraps;
	if (why && (++VfpVecBails <= 8 || (VfpVecBails & 0xFFF) == 0))
		{ ekseam_puts("[EKERN][vfpvec] stopped ("); ekseam_puts(why); ekseam_puts(") at pc="); ekseam_puthex(pc); }
	if (VfpVecTraps <= 2 || (VfpVecTraps & 0xFFFF) == 0)
		{ ekseam_puts("[EKERN][vfpvec] emulated a short-vector block, traps="); ekseam_puthex(VfpVecTraps); }
	return true;
	}
#endif

extern "C" void __real__ZN3Exc8DispatchEPvP7NThread(void*, void*);
extern "C" void __wrap__ZN3Exc8DispatchEPvP7NThread(void* aInfo, void* aNThread)
	{
	volatile unsigned* e = (volatile unsigned*)aInfo;
#ifdef __SMP__
	if (EmulateSwp(e) || EmulateAlign(e))
		return;
#ifdef __CPU_HAS_VFP
	if (EmulateVfpVector(e, aInfo))
		return;
	if (e[21] == 2 && !(e[23] & (1u << 5)))	// a user VFP/NEON UNDEF = the kernel's lazy per-thread VFP enable
		{									// (Exc::Dispatch -> HandleVFPOperation): routine, no dump (2026-10-06)
		const unsigned op = *(volatile unsigned*)e[22], h2 = (op >> 24) & 0xF, cp = (op >> 8) & 0xF;
		if (((h2 == 0xC || h2 == 0xD || h2 == 0xE) && (cp == 10 || cp == 11))
			|| ((op >> 28) == 0xF && (h2 == 2 || h2 == 3 || (h2 == 4 && !((op >> 20) & 1)))))
			{
			__real__ZN3Exc8DispatchEPvP7NThread(aInfo, aNThread);
			return;
			}
		}
	if (e[21] == 2 && (e[23] & (1u << 5)))	// the same for Thumb-2 (ckernel.cpp's Thumb decode, built with __MARM_INTERWORK__):
		{									// NO print here -- a log line wakes the USB DFC, HandleVFPOperation's NKern::Unlock
											// then reschedules, the SMP switch saves FPEXC with EN clear and the instruction
											// traps again: every print = one more trap, forever (phone boot 430)
		const unsigned short* p = (const unsigned short*)e[22];
		const unsigned h1 = p[0] >> 8, cp = (p[1] >> 8) & 0xF, h2 = h1 & 0xF;
		if (((h1 & 0xE0) == 0xE0 && (h2 == 0xC || h2 == 0xD || h2 == 0xE) && (cp == 10 || cp == 11))
			|| h1 == 0xEF || h1 == 0xFF || h1 == 0xF9)
			{
			__real__ZN3Exc8DispatchEPvP7NThread(aInfo, aNThread);
			return;
			}
		}
#endif
#endif
	ekseam_puts("\n[EKERN] *** Exc::Dispatch (user exception) ***\n");
#ifdef __SMP__	// the SMP TArmExcInfo (arm.h): iFaultAddress@0, iFaultStatus@1, iR14@20, iExcCode@21, iR15@22
	const int kCode = 21, kPc = 22, kFar = 0, kFsr = 1, kLr = 20;
#else
	const int kCode = 1, kPc = 22, kFar = 12, kFsr = 13, kLr = 16;
#endif
	ekseam_puts("[EKERN][exc] code(0=pabt,1=dabt,2=undef)="); ekseam_puthex(e[kCode]);
	ekseam_puts("[EKERN][exc] pc=");    ekseam_puthex(e[kPc]);
	ekseam_puts("[EKERN][exc] far=");   ekseam_puthex(e[kFar]);
	ekseam_puts("[EKERN][exc] fsr=");   ekseam_puthex(e[kFsr]);
	ekseam_puts("[EKERN][exc] lr_usr=");ekseam_puthex(e[kLr]);
#ifdef __SMP__
	{	// does the faulting address translate NOW (stale TLB) or is the table really empty? ATS1CUW -> PAR
		unsigned ttbr0, ttbcr, cid, par, mp;
		__asm__ volatile("mrc p15,0,%0,c2,c0,0" : "=r"(ttbr0));
		__asm__ volatile("mrc p15,0,%0,c2,c0,2" : "=r"(ttbcr));
		__asm__ volatile("mrc p15,0,%0,c13,c0,1" : "=r"(cid));
		__asm__ volatile("mrc p15,0,%0,c0,c0,5" : "=r"(mp));
		__asm__ volatile("mcr p15,0,%1,c7,c8,3\n isb\n mrc p15,0,%0,c7,c4,0" : "=r"(par) : "r"(e[kFar]));
		ekseam_puts("[EKERN][exc] mpidr="); ekseam_puthex(mp);
		ekseam_puts("[EKERN][exc] ttbr0="); ekseam_puthex(ttbr0);
		ekseam_puts("[EKERN][exc] ttbcr="); ekseam_puthex(ttbcr);
		ekseam_puts("[EKERN][exc] contextidr="); ekseam_puthex(cid);
		ekseam_puts("[EKERN][exc] PAR(ATS1CUW far)="); ekseam_puthex(par);
	}
	if ((e[23] & 0x1F) == 0x10)
		{	// a user fault: the ROM code addresses among the top 64 stack words = the likely callers (no frame pointers
			// to walk); symbolize them with the image's .map. At most 12 lines.
		unsigned sp = e[19], shown = 0;
		for (unsigned i = 0; i < 64 && shown < 12; ++i, sp += 4)
			{
			if ((sp & 0xFFF) == 0 || i == 0)
				if (!UserCanAccess(sp, false))
					break;
			const unsigned w = *(volatile unsigned*)sp;
			if (w >= 0x80000000u && w < 0x90000000u)
				{ ekseam_puts("[EKERN][exc] stack ret?="); ekseam_puthex(w); ++shown; }
			}
		}
#endif
	__real__ZN3Exc8DispatchEPvP7NThread(aInfo, aNThread);
	}

extern "C" void __real_ExcFault(void*);
extern "C" void __wrap_ExcFault(void* aExcInfo)
	{
	volatile unsigned* e = (volatile unsigned*)aExcInfo;
	ekseam_puts("\n[EKERN] *** ExcFault: exception dispatched ***\n");
	if (ekseam_board_notify)
		ekseam_board_notify(3, "KERNEL FAULT: exception (see the log)");
#ifdef __SMP__	// the SMP TArmExcInfo (as the Exc::Dispatch wrapper): far@0 fsr@1 r0..r12@6..18 r13@19 r14@20 code@21 pc@22 cpsr@23
	const int kCode = 21, kFar = 0, kFsr = 1, kCpsr = 23;
#else
	const int kCode = 1, kFar = 12, kFsr = 13, kCpsr = 0;
#endif
	ekseam_puts("[EKERN][exc] iExcCode(0=pabt,1=dabt,2=undef)="); ekseam_puthex(e ? e[kCode] : 0);
	ekseam_puts("[EKERN][exc] iR15 faulting PC="); ekseam_puthex(e ? e[22] : 0);
	ekseam_puts("[EKERN][exc] iFaultAddress=");    ekseam_puthex(e ? e[kFar] : 0);
	ekseam_puts("[EKERN][exc] iFaultStatus=");     ekseam_puthex(e ? e[kFsr] : 0);
	ekseam_puts("[EKERN][exc] iCpsr=");            ekseam_puthex(e ? e[kCpsr] : 0);
#ifdef __SMP__
	if (!e)
		{	// the SMP vectors' kernel-mode crash path saves everything in this CPU's SFullArmRegSet and calls
			// ExcFault(0) (nkernsmp/arm/vectors.cia): dump that (2026-10-06)
		const TSubScheduler& ss = TheSubSchedulers[NKern::CurrentCpu()];
		const SFullArmRegSet* rs = ss.iSSX.iRegs;
		ekseam_puts("[EKERN][exc] ExcFault(0): the CPU's saved register set, cpu="); ekseam_puthex((unsigned)NKern::CurrentCpu());
		if (rs)
			{
			const unsigned* w = (const unsigned*)&rs->iN;
			static const char* const KN[] = { "r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12","r13","r14",
				"r15(pc)","cpsr","r13svc","r14svc","spsrsvc","r13abt","r14abt","spsrabt","r13und","r14und","spsrund" };
			for (int i = 0; i < 26; ++i) { ekseam_puts("[EKERN][exc] "); ekseam_puts(KN[i]); ekseam_puts("="); ekseam_puthex(w[i]); }
			ekseam_puts("[EKERN][exc] iExcCode="); ekseam_puthex((unsigned)rs->iExcCode);
			const unsigned* sp = (const unsigned*)w[13];
			if (sp && ((unsigned)sp & 3) == 0 && (unsigned)sp >= 0x80000000u)
				for (int i = 0; i < 24; ++i) { ekseam_puts("[EKERN][exc] stk="); ekseam_puthex(sp[i]); }
			}
		NThread* t = ss.iCurrentThread;
		if (t)
			{
			TBuf8<64> n;
			_LOFF(t, DThread, iNThread)->TraceAppendFullName(n, EFalse);
			n.Append(0);
			ekseam_puts("[EKERN][exc] thread="); ekseam_puts((const char*)n.Ptr()); ekseam_puts("\n");
			}
		}
	if (e)
		{	// a PC of 0 = a call through a null pointer: the caller is in r14 (2026-10-06, the Phorepoc SSM boot)
		ekseam_puts("[EKERN][exc] r14 (caller)="); ekseam_puthex(e[20]);
		ekseam_puts("[EKERN][exc] r13="); ekseam_puthex(e[19]);
		for (int i = 0; i < 13; ++i) { ekseam_puts("[EKERN][exc] r"); char b[3] = { (char)('0' + i / 10), (char)('0' + i % 10), 0 }; ekseam_puts(b); ekseam_puts("="); ekseam_puthex(e[6 + i]); }
		const unsigned* sp = (const unsigned*)e[19];		// the faulting mode's stack: return addresses for romsym
		if (sp && ((unsigned)sp & 3) == 0)
			for (int i = 0; i < 24; ++i) { ekseam_puts("[EKERN][exc] stk="); ekseam_puthex(sp[i]); }
		}
#endif
	__real_ExcFault(aExcInfo);
	}

// --- entry trampoline (diagnostic) ---------------------------------------------------
// Point the ROM image's iEntryPoint here instead of _E32Startup: print a marker to the
// virtual UART (proving the bootstrap's jump reaches our kernel code in virtual space),
// preserving r0/r1 (aRomHeader/aSuperPage), then branch to the real _E32Startup.
extern "C" void _E32Startup(unsigned, unsigned);
extern "C" __attribute__((naked, used)) void _ekern_entry()
	{
	__asm__ volatile(
		"stmfd sp!, {r0, r1, lr}\n"
		"ldr   r4, =" EKSEAM_UART_S "\n"        // virtual UART0 data register (EKSEAM_UART)
		"adr   r5, 2f\n"
		"1:\n ldrb r6, [r5], #1\n cmp r6, #0\n beq 3f\n str r6, [r4]\n b 1b\n"
		"3:\n ldmfd sp!, {r0, r1, lr}\n"
		"b     _E32Startup\n"
		".ltorg\n"
		"2:\n .asciz \"\\n[EKERN] entry trampoline reached -- jumping into _E32Startup\\n\"\n"
		".align 2\n");
	}

// --- variant DLL entry (_E32Dll) -----------------------------------------------------
// The variant's code (VariantInitialise/AsicInitialise/Syborg::*) is statically linked
// into ekern, so the ROM "variant file" is just a TRomImageHeader whose export table
// points at those ekern functions and whose iEntryPoint is this minimal _E32Dll.
// InitData calls it with KModuleEntryReasonVariantInit0; the return value is the mode
// bitmask A::CreateVariant uses for staged VariantInitialise(i) calls (0 = just create
// the Asic via ordinal 1). CHECK_ROM_ENTRY_POINT only checks the header iFlags (Eka2),
// not this code, so a plain function suffices.
// It returns __Variant_Flags__ as the real kernel/arm/v_entry.cpp does: the variant's constructor (a static ctor,
// already run by ekern's startup) sets bit 0 under __SMP__ (__VARIANT_SUPPORTS_NANOKERNEL_INTERFACE_BLOCK__) so the
// kernel asks VariantInitialise(1) for the VIB; the UP variant leaves it 0 (just the Asic).
extern "C" { TInt __Variant_Flags__ = 0; }
extern "C" int _variant_e32dll(int /*aReason*/) { return __Variant_Flags__; }

// --- 4. MMU domain-access default (multiple model) -----------------------------------
// Arm::DefaultDomainAccess -- supervisor is a client of domain 0 (KSupervisorInitialDomainAccess).
namespace Arm { extern TUint32 DefaultDomainAccess; TUint32 DefaultDomainAccess = 0x00000001u; }

// --- 5. Static-destructor registration (flexible model) -------------------------------------
// The flexible memory model has file-scope objects with destructors (maddressspace.cpp, mrom.cpp ...): g++
// registers them through __aeabi_atexit(obj, dtor, &__dso_handle). The kernel never exits, so nothing
// consumes the registrations (the x64 port does the same: x64s_ncsched.cpp).
#ifdef __MEMMODEL_FLEXIBLE__
extern "C" { void* __dso_handle = 0; int __aeabi_atexit(void*, void (*)(void*), void*) { return 0; } }
#endif

// Code-segment lifetime trace (PinePhone flexible S60: libc's seg is destroyed while still a dependency --
// pinephone-port.md rung 3). Prints the seg address + root name at add-dependency, loaded and destruct.
static void ekseam_seg(const char* tag, DCodeSeg* s)
	{
	char nm[40]; unsigned i = 0;
	if (s && s->iFileName)
		{
		const TUint8* p = s->iRootName.Ptr(); unsigned n = s->iRootName.Length();
		for (; i < n && i < sizeof(nm) - 1; ++i) nm[i] = p[i];
		}
	nm[i] = 0;
	ekseam_puts(tag); ekseam_puthex((unsigned)(unsigned long)s); ekseam_puts(" "); ekseam_puts(nm);
	ekseam_puts(" acc="); ekseam_puthex(s ? (unsigned)s->iAccessCount : 0);
	}
extern "C" TInt __real__ZN11ExecHandler20CodeSegAddDependencyEPvS0_(TAny*, TAny*);
extern "C" TInt __wrap__ZN11ExecHandler20CodeSegAddDependencyEPvS0_(TAny* aImp, TAny* aExp)
	{
	TInt r = __real__ZN11ExecHandler20CodeSegAddDependencyEPvS0_(aImp, aExp);
	ekseam_seg("[cs] dep imp=", (DCodeSeg*)aImp); ekseam_seg("[cs]     exp=", (DCodeSeg*)aExp);
	ekseam_puts("[cs]     r="); ekseam_puthex((unsigned)r);
	return r;
	}
extern "C" void __real__ZN12DEpocCodeSeg8DestructEv(DCodeSeg*);
extern "C" void __wrap__ZN12DEpocCodeSeg8DestructEv(DCodeSeg* aSeg)
	{
	ekseam_seg("[cs] DESTRUCT ", aSeg);
	__real__ZN12DEpocCodeSeg8DestructEv(aSeg);
	}
extern "C" TInt __real__ZN11ExecHandler13ProcessLoadedER18TProcessCreateInfo(TAny*);
extern "C" TInt __wrap__ZN11ExecHandler13ProcessLoadedER18TProcessCreateInfo(TAny* aInfo)
	{
	TInt r = __real__ZN11ExecHandler13ProcessLoadedER18TProcessCreateInfo(aInfo);
	ekseam_puts("[cs] ProcessLoaded r="); ekseam_puthex((unsigned)r);
	return r;
	}
extern "C" TInt __real__ZN11ExecHandler13CodeSegLoadedER18TCodeSegCreateInfo(TAny*);
extern "C" TInt __wrap__ZN11ExecHandler13CodeSegLoadedER18TCodeSegCreateInfo(TAny* aInfo)
	{
	TInt r = __real__ZN11ExecHandler13CodeSegLoadedER18TCodeSegCreateInfo(aInfo);
	ekseam_puts("[cs] CodeSegLoaded r="); ekseam_puthex((unsigned)r);
	return r;
	}

#ifdef EKSEAM_GT_NANOWAIT
// K::DoNanoWait on the ARM generic timer (BOARD=virt; the A64 has it too). The 2010 version is a calibrated
// busy loop (PP::NanoWaitCal ns per 2-instruction iteration) -- meaningless under QEMU, where it ran a 1 ms wait in
// microseconds and K::InitAP's 500 x 1 ms wait for a booting AP expired at once (EInit2APTimeout, SMP rung 9).
// Here: spin until CNTPCT has advanced by aInterval ns at CNTFRQ.
extern "C" void __wrap__ZN1K10DoNanoWaitEm(TUint32 aInterval)
	{
	TUint32 frq, lo, hi;
	asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(frq));
	asm volatile("isb; mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
	TUint64 start = ((TUint64)hi << 32) | lo;
	TUint64 ticks = ((TUint64)aInterval * frq) / 1000000000u;
	TUint64 now;
	do	{
		asm volatile("isb; mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
		now = ((TUint64)hi << 32) | lo;
		} while (now - start < ticks);
	}
#endif

#ifdef EKSEAM_PDTRACE
// PageDirectoryAllocator::Alloc/Free trace (P3 rung 3, the in-RAM ROM: a process ran with TTBR0 != its ASID's
// mapped page directory). One line per call.
extern "C" int __real__ZN22PageDirectoryAllocator5AllocEjRm(void*, unsigned, unsigned long&);
extern "C" int __wrap__ZN22PageDirectoryAllocator5AllocEjRm(void* aThis, unsigned aOsAsid, unsigned long& aPd)
	{
	int r = __real__ZN22PageDirectoryAllocator5AllocEjRm(aThis, aOsAsid, aPd);
	ekseam_puts("[pd] Alloc asid="); ekseam_puthex(aOsAsid);
	ekseam_puts("[pd]   r="); ekseam_puthex(r);
	ekseam_puts("[pd]   pdPhys="); ekseam_puthex(aPd);
	return r;
	}
extern "C" void __real__ZN22PageDirectoryAllocator4FreeEj(void*, unsigned);
extern "C" void __wrap__ZN22PageDirectoryAllocator4FreeEj(void* aThis, unsigned aOsAsid)
	{
	ekseam_puts("[pd] Free asid="); ekseam_puthex(aOsAsid);
	__real__ZN22PageDirectoryAllocator4FreeEj(aThis, aOsAsid);
	}
#endif

// OUT OF MEMORY (2026-10-07): S60 showed "out of memory" on the PinePhone with 411 MB free -- a process at ITS
// limit, not the system. Every chunk grow/create refused with KErrNoMemory is logged with the caller and sizes.
static void ekseam_oom(const char* aWhat, int a1, int a2)
	{
	TBuf8<96> fn;
	Kern::CurrentThread().FullName(fn);
	fn.Append('\0');
	ekseam_puts("[oom] "); ekseam_puts(aWhat); ekseam_puts(" -> KErrNoMemory in '"); ekseam_puts((const char*)fn.Ptr());
	ekseam_puts("' a1="); ekseam_puthex((unsigned)a1); ekseam_puts(" a2="); ekseam_puthex((unsigned)a2);
	ekseam_puts(" freeRAM="); ekseam_puthex((unsigned)Kern::FreeRamInBytes());
	ekseam_puts("\n");
	}
// a failed kernel heap allocation (the 1 MB kernel heap ran dry under S60, 2026-10-07): size + the heap's committed
// size, rate-limited to the first 16
extern "C" void* __real__ZN4Kern5AllocEi(TInt);
extern "C" void* __wrap__ZN4Kern5AllocEi(TInt aSize)
	{
	void* p = __real__ZN4Kern5AllocEi(aSize);
	static TInt fails = 0;
	if (!p && aSize > 0 && fails < 16)
		{
		++fails;
		ekseam_puts("[oom] Kern::Alloc failed size="); ekseam_puthex((unsigned)aSize);
		ekseam_puts(" kheap="); ekseam_puthex((unsigned)K::Allocator->Size());
		ekseam_puts(" freeRAM="); ekseam_puthex((unsigned)Kern::FreeRamInBytes());
		ekseam_puts("\n");
		}
	return p;
	}
// ...and on a refused create, every chunk mapped into the process (flexible model: DMemModelProcess::iChunks,
// fields from memmodel.h) -- with RAM free, a -4 is the process's virtual address space: what reserves it?
static void ekseam_oom_chunks()
	{
#ifdef __MEMMODEL_FLEXIBLE__
	DMemModelProcess* p = (DMemModelProcess*)&Kern::CurrentProcess();
	TUint total = 0;
	ekseam_puts("[oom]   process chunks="); ekseam_puthex((unsigned)p->iChunkCount); ekseam_puts("\n");
	for (TInt i = 0; i < p->iChunkCount; ++i)
		{
		DMemModelChunk* c = p->iChunks[i].iChunk;
		TBuf8<96> n;
		c->FullName(n);
		n.Append('\0');
		total += (TUint)c->MaxSize();
		ekseam_puts("[oom]   base="); ekseam_puthex((unsigned)c->Base(p));
		ekseam_puts(" size="); ekseam_puthex((unsigned)c->Size());
		ekseam_puts(" max="); ekseam_puthex((unsigned)c->MaxSize());
		ekseam_puts(" "); ekseam_puts((const char*)n.Ptr()); ekseam_puts("\n");
		}
	ekseam_puts("[oom]   total max="); ekseam_puthex(total); ekseam_puts("\n");
#endif
	}
extern "C" int __real__ZN11ExecHandler11ChunkAdjustEP6DChunkiii(DChunk*, int, int, int);
extern "C" int __wrap__ZN11ExecHandler11ChunkAdjustEP6DChunkiii(DChunk* aChunk, int aType, int a1, int a2)
	{
	int r = __real__ZN11ExecHandler11ChunkAdjustEP6DChunkiii(aChunk, aType, a1, a2);
	if (r == KErrNoMemory)
		{
		ekseam_oom("ChunkAdjust", a1, a2);
		ekseam_puts("[oom]   chunk max size="); ekseam_puthex((unsigned)aChunk->MaxSize());
		ekseam_puts(" size="); ekseam_puthex((unsigned)aChunk->Size()); ekseam_puts(" type="); ekseam_puthex((unsigned)aType);
		ekseam_puts("\n");
		}
	return r;
	}
extern "C" int __real__ZN11ExecHandler11ChunkCreateE10TOwnerTypePK6TDesC8R12TChunkCreate(int, const TDesC8*, TChunkCreate&);
extern "C" int __wrap__ZN11ExecHandler11ChunkCreateE10TOwnerTypePK6TDesC8R12TChunkCreate(int aType, const TDesC8* aName, TChunkCreate& aInfo)
	{
#ifdef EKSEAM_FLUSH_ON_PROC
	// (debug, the NGA boot death: every user chunk into the SD log before and after)
	ekseam_puts("[dbg] chunk create att="); ekseam_puthex((unsigned)aInfo.iAtt);
	ekseam_puts("[dbg]   max="); ekseam_puthex((unsigned)aInfo.iMaxSize);
	if (ekseam_board_panic) ekseam_board_panic();
#endif
	int r = __real__ZN11ExecHandler11ChunkCreateE10TOwnerTypePK6TDesC8R12TChunkCreate(aType, aName, aInfo);
#ifdef EKSEAM_FLUSH_ON_PROC
	ekseam_puts("[dbg]   chunk create r="); ekseam_puthex((unsigned)r);
	if (ekseam_board_panic) ekseam_board_panic();
#endif
	if (r == KErrNoMemory)
		{
		ekseam_oom("ChunkCreate", aInfo.iMaxSize, aInfo.iInitialTop);
		ekseam_oom_chunks();
		}
	return r;
	}
