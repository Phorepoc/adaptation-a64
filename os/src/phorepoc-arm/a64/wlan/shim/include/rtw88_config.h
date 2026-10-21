/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

/* rtw88_config.h -- OURS: the kernel "config" the vendored rtw88 is built with (forced in via -include). */
#ifndef RTW88_CONFIG_H
#define RTW88_CONFIG_H
#define CONFIG_RTW88_DEBUG 1
#define __KERNEL__ 1
/* The A64 runs little-endian. Linux's byteorder headers define __LITTLE_ENDIAN (<linux/byteorder/little_endian.h>); our
 * shim did not, so every "#ifdef __LITTLE_ENDIAN" bitfield in rtw88 took its big-endian layout (found 2026-10-17, boot
 * 952): the PHY table condition word (main.h struct rtw_phy_cond -- pos/neg landed on address bits 0/1, so table entries
 * with an odd address or bit 1 set were taken for condition headers and they and the entries after them were skipped:
 * RF 0x1C/0xDF/0x31/0x32/0x52/0xB1/0xB2/0xB5/0x82/0x83, odd MAC-table bytes), the efuse TX power diff nibbles (main.h
 * rtw_2g_*_pwr_idx_diff), the 8703B PHY status (rtw8703b.h phy_rx_agc_info gain/trsw, ...). */
#define __LITTLE_ENDIAN 1234
#define __LITTLE_ENDIAN_BITFIELD 1
#endif
