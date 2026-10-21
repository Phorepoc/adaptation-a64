// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// ekern_nga_glue.cpp -- OURS (GPU plan rung G1b): the NGA kernel side, linked
// into ekern on the boards with a display channel (a64, virt: tools/kernel/link_ekern.sh): the surface manager
// (DECLARE_EXTENSION_LDD + DECLARE_STANDARD_EXTENSION) and the Graphics Resource sg extension + sgdevice LDD -- REAL
// 2010 sources whose entry points the link renames. The boards call ekseam_load_nga_drivers from their driver seam.
#include <kernel/kernel.h>

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);

TInt InitExtension_surfmgr();
DLogicalDevice* CreateLogicalDevice_surfmgr();
TInt InitExtension_sgext();
DLogicalDevice* CreateLogicalDevice_sgdev();

extern "C" void ekseam_load_nga_drivers()
	{
	TInt r = InitExtension_surfmgr();
	ekseam_puts("[EKERN][nga] surface manager extension r="); ekseam_puthex((unsigned)r);
	DLogicalDevice* ld = CreateLogicalDevice_surfmgr();
	r = ld ? Kern::InstallLogicalDevice(ld) : KErrNoMemory;
	ekseam_puts("[EKERN][nga] surface manager LDD r="); ekseam_puthex((unsigned)r);
	r = InitExtension_sgext();
	ekseam_puts("[EKERN][nga] sg extension r="); ekseam_puthex((unsigned)r);
	ld = CreateLogicalDevice_sgdev();
	r = ld ? Kern::InstallLogicalDevice(ld) : KErrNoMemory;
	ekseam_puts("[EKERN][nga] sgdevice LDD r="); ekseam_puthex((unsigned)r);
	}
