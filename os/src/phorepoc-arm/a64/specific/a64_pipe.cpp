// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// a64_pipe.cpp -- the Open C pipe(): the REAL 2010 pipe LDD (kernel/eka/drivers/pipe/dpipe.cpp, compiled unmodified with only its
// entry points renamed, tools/kernel/link_ekern.sh) installed as a built-in device "SymbianPipe". euser's RPipe::Create does
// User::LoadLogicalDevice("PIPELIB"); there is no PIPELIB.LDD file in the ROM, and the loader's built-in-device answer
// (toolchain/efile_exe/ldr_builtin_devices.cpp: "pipelib" -> "symbianpipe") turns that into KErrAlreadyExists once the device is
// installed. Without it pipe(), popen() and everything built on them (zsh pipelines and $(...)) fail with ENOENT ("LDR: load
// PIPELIB.LDD -> -1"). The x86 board does the same (xk_pipe.cpp).
#include <kernel/kern_priv.h>

extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(unsigned);

DLogicalDevice* CreateLogicalDevice_pipe();		// dpipe.cpp (DECLARE_STANDARD_LDD, renamed)

extern "C" void A64PipeInstall()
	{
	NKern::ThreadEnterCS();
	DLogicalDevice* ld = CreateLogicalDevice_pipe();
	const TInt r = ld ? Kern::InstallLogicalDevice(ld) : KErrNoMemory;
	NKern::ThreadLeaveCS();
	ekseam_puts("[EKERN][pipe] InstallLogicalDevice(SymbianPipe) r="); ekseam_puthex((unsigned)r);
	}
