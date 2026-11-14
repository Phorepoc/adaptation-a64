// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// ekern_media_glue.cpp -- Track B M7 media-driver bring-up glue.
//
// Brings up the REAL local-media stack as built-in kernel extensions, the same
// pattern as ekern_fb_glue.cpp (M5.5): call each DECLARE_STANDARD_EXTENSION entry
// directly from the supervisor thread (the context the ROM extension-start DFC
// would use), in ROM order:
//
//   1. locmedia.cpp (ELOCD) InitExtension     -- media HAL entry
//   2. Kern::InstallLogicalDevice(CreateLogicalDevice())  -- the 'LocDrv' LDD
//      factory. In a real ROM this happens when User::LoadLogicalDevice("ELOCD")
//      loads the extension-LDD by file; we have no ELOCD.LDD file in ROM (the
//      fileserver's LoadLogicalDevice then returns KErrNotFound, which
//      StartupThread tolerates), so install the factory here.
//   3. Kern::InstallPhysicalDevice(CreatePhysicalDevice()) -- the
//      'Media.MEDNVMEMORY' PDD factory (same reasoning: no med*.pdd file).
//   4. syborg_mednvmemory.cpp InitExtension   -- creates the DPrimaryMediaBase,
//      its DFC queue, and LocDrv::RegisterMediaDevice(EFixedMedia1, drive C:).
//
// The colliding per-module entry symbols (InitExtension / KernelModuleEntry /
// Create{Logical,Physical}Device -- each module normally being its own DLL) are
// made unique with objcopy --redefine-sym in link_ekern.sh.
#include <kernel/kernel.h>

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);

TInt InitExtension_locmedia();
TInt InitExtension_mednvme();
DLogicalDevice* CreateLogicalDevice_locmedia();
DPhysicalDevice* CreatePhysicalDevice_mednvme();

extern "C" void ekseam_load_media_drivers()
	{
	ekseam_puts("\n[EKERN][med] === locmedia (ELOCD) extension init ===\n");
	TInt r = InitExtension_locmedia();
	ekseam_puts("[EKERN][med] locmedia InitExtension r="); ekseam_puthex((unsigned)r);

	DLogicalDevice* ld = CreateLogicalDevice_locmedia();
	r = ld ? Kern::InstallLogicalDevice(ld) : KErrNoMemory;
	ekseam_puts("[EKERN][med] InstallLogicalDevice(LocDrv) r="); ekseam_puthex((unsigned)r);

	DPhysicalDevice* pd = CreatePhysicalDevice_mednvme();
	r = pd ? Kern::InstallPhysicalDevice(pd) : KErrNoMemory;
	ekseam_puts("[EKERN][med] InstallPhysicalDevice(Media.MEDNVMEMORY) r="); ekseam_puthex((unsigned)r);

	ekseam_puts("[EKERN][med] === mednvmemory extension init (register drive C:) ===\n");
	r = InitExtension_mednvme();
	ekseam_puts("[EKERN][med] mednvmemory InitExtension r="); ekseam_puthex((unsigned)r);
	}
