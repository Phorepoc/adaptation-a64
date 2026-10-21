/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

/* OURS: the A64 memory map as RUNTIME linear bases (the kernel maps each window with DPlatChunkHw, a64_display.cpp).
 * Physical addresses: DE 0x01000000 (3M) | SYSCTL..PWM 0x01C00000 | DSI/DPHY 0x01CA0000 | R_* 0x01F00000. */
#ifndef PH_A64_MEMORYMAP_H
#define PH_A64_MEMORYMAP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uintptr_t ph_a64_de, ph_a64_io, ph_a64_dsi, ph_a64_r;
#ifdef __cplusplus
}
#endif
#define A64_DE_ADDR     (ph_a64_de)
#define A64_SYSCTL_ADDR (ph_a64_io + 0x00000)
#define A64_TCON0_ADDR  (ph_a64_io + 0x0c000)
#define A64_CCU_ADDR    (ph_a64_io + 0x20000)
#define A64_PIO_ADDR    (ph_a64_io + 0x20800)
#define A64_PWM_ADDR    (ph_a64_io + 0x21400)
#define A64_DSI_ADDR    (ph_a64_dsi + 0x0000)
#define A64_DPHY_ADDR   (ph_a64_dsi + 0x1000)
#define A64_RTWI_ADDR   (ph_a64_r + 0x2400)
#define A64_RPIO_ADDR   (ph_a64_r + 0x2c00)
#define A64_RSB_ADDR    (ph_a64_r + 0x3400)
#define A64_RPWM_ADDR   (ph_a64_r + 0x3800)
#endif
