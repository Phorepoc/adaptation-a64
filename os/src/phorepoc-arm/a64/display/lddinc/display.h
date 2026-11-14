/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

// OURS: the display LDD header with KDisplayCBMax = 3 lives in drivers/display.h beside this file; the Syborg
// framebuffer driver includes it as <display.h>, the LDD as <drivers/display.h> -- both must see the SAME copy (a
// mismatch skews DDisplayLdd's layout between the LDD and the PDD: the 2026-10-09 "unsupported rotation 0" crash).
#include <drivers/display.h>
