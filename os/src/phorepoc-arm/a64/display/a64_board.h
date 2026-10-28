// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// a64_board.h -- what the A64 display driver needs from the board it runs on: the panel behind the MIPI DSI port,
// its backlight and the PMIC rails that power it. A board implements these in board/kernel/ (compiled into the kernel
// by tools/kernel/link_ekern.sh) and gives the panel's size in board/kernel/a64_board_config.h:
//   #define A64_PANEL_W 720
//   #define A64_PANEL_H 1440
#ifndef A64_BOARD_H
#define A64_BOARD_H

#include "a64_board_config.h"

#ifdef __cplusplus
extern "C" {
#endif
int a64_board_pmic_init(void);                          // the display's power rails on
int a64_board_panel_reset(int high);                    // the panel's reset line
int a64_board_panel_init(void);                         // the panel's init sequence over DSI (after the D-PHY is up)
int a64_board_backlight_enable(unsigned percent);
int a64_board_backlight_ticks(unsigned ticks);          // the PWM duty in timer ticks (0 = off)
int a64_board_backlight_disable(void);
#ifdef __cplusplus
}
#endif

#endif
