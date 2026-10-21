// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// tldd.cpp -- OURS (NGA G1b): which User::LoadLogicalDevice freezes the phone? Step 1 loads a driver file that does
// not exist anywhere; then a display channel open/close (the kernel logs "[disp] DDisplayPddA64 created" = step 1
// returned); step 2 loads "display0.ldd" (absent as a file; its LDD is built into ekern). The exit reason (kernel
// log) = 1000 * -r1 + -r2 when both return.
#include <e32base.h>
#include <dispchannel.h>

TInt E32Main()
	{
	User::After(20000000);
	TInt r1 = User::LoadLogicalDevice(_L("tldd_nonexistent.ldd"));
	RDisplayChannel ch;
	if (ch.Open(0) == KErrNone)
		ch.Close();
	TInt r2 = User::LoadLogicalDevice(_L("display0.ldd"));
	return 1000 * -r1 + -r2;
	}
