// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// phekeys.cpp -- OURS: PhoneLab's type_chars (phfs op 14). Command line = 8 hex digits of modifiers, then the text.
// Each character goes to the focused window group as a key event with its code and no scan code (the FEP passes a
// key that is not one of its keypad keys to the editor as it is); a line feed = Enter. Exit reason = characters typed.
#include <e32base.h>
#include <w32std.h>

TInt E32Main()
	{
	TBuf<520> cmd;
	if (User::CommandLineLength() > cmd.MaxLength())
		return KErrOverflow;
	User::CommandLine(cmd);
	if (cmd.Length() < 8)
		return KErrArgument;
	TUint32 mods = 0;
	if (TLex(cmd.Left(8)).Val(mods, EHex) != KErrNone)
		return KErrArgument;
	RWsSession ws;
	TInt r = ws.Connect();
	if (r != KErrNone)
		return r;
	TInt n = 8;
	for (; n < cmd.Length(); ++n)
		{
		TKeyEvent k;
		const TUint c = cmd[n];
		const TBool enter = c == 0x0a;
		const TBool keypad = (c >= '0' && c <= '9') || c == '*' || c == '#';
		k.iCode = enter ? (TUint)EKeyEnter : c;
		k.iScanCode = enter ? EStdKeyEnter : 0;
		k.iModifiers = enter ? 0 : (mods & 0x7fffffff);
		k.iRepeats = (keypad && (mods & 0x80000000)) ? 1 : 0;	// bit 31: a keypad character as a repeat (a long press)
		ws.SimulateKeyEvent(k);
		ws.Flush();
		User::After(40000);									// the editor's pace (an echo per character)
		}
	ws.Close();
	return n - 8;
	}
