// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// ekern_input_glue.cpp -- Track B input-driver bring-up glue.
//
// Loads the REAL in-source Syborg input drivers as built-in kernel extensions
// (the fb/media-glue pattern: static link + call the DECLARE_STANDARD_EXTENSION
// entry from the supervisor thread at StartKernelServer):
//   keyboard/syborg_keyboard.cpp -- DKeyboardPs2Soc: EIntKeyboard ISR -> RxDfc
//        -> TRawEvent EKeyDown/EKeyUp -> Kern::AddEvent (-> EWSRV's event queue)
//   pointer/syborg_pointer.cpp   -- the pointer (touch) equivalent.
// Entry symbols are made unique with objcopy --redefine-sym in link_ekern.sh.
#include <e32def.h>
#include <kernel/kernel.h>

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);

TInt InitExtension_keyboard();
TInt InitExtension_pointer();

extern "C" void ekseam_load_input_drivers()
	{
	ekseam_puts("\n[EKERN][inp] === syborg keyboard extension init ===\n");
	TInt r = InitExtension_keyboard();
	ekseam_puts("[EKERN][inp] keyboard InitExtension r="); ekseam_puthex((unsigned)r);
	ekseam_puts("[EKERN][inp] === syborg pointer extension init ===\n");
	r = InitExtension_pointer();
	ekseam_puts("[EKERN][inp] pointer InitExtension r="); ekseam_puthex((unsigned)r);
	}
