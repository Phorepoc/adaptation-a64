/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

/* rxregs_dump.h -- OURS (2026-10-17): one list of RX-relevant RTL8703B registers, dumped the same way by both chip cores
 * (rtl8703b/ = WIFI_DRV=8723cs, a64wlan_glue.c = WIFI_DRV=rtw88) at the Join, so a boot pair diffs line by line:
 *   [rxregs <tag>] mac 0064=........ 0100=........ ...
 * Include once, after defining RXD_R32(addr), RXD_RF(addr) (RF path A, 20 bits) and RXD_LOG(fmt, ...). */
#ifndef RXREGS_DUMP_H
#define RXREGS_DUMP_H

/* MAC: clock/pad (0x24-0x2c AFE + xtal, 0x64-0x6c pad/GPIO/ant), CR, TXDMA map, RXDMA agg/mode, HWSEQ, RCR, RX filters,
 * coex tables, BT/antenna (0x764, 0x76c), response rates */
static const unsigned short rxd_mac[] = {
	0x0000, 0x0004, 0x0024, 0x0028, 0x002c, 0x0040, 0x004c, 0x0064, 0x0068, 0x006c, 0x0070, 0x0074,
	0x0100, 0x010c, 0x0280, 0x0284, 0x0290, 0x0420, 0x0440, 0x0550, 0x0608, 0x060c, 0x0610, 0x0618,
	0x061c, 0x063c, 0x0668, 0x06a0, 0x06a4, 0x06c0, 0x06c4, 0x06c8, 0x06cc, 0x0764, 0x076c, 0x0778,
	0x0790, 0x07c0,
};
/* BB: CCK/OFDM enable (0x800), RF interface (0x820-0x870), AGC / RX path (0x948, 0x950), CCK (0xa00-0xa2c), OFDM RX
 * (0xc04-0xc1c), IGI (0xc50), AGC core (0xc58-0xc78), IQK results (0xc14, 0xc80-0xca0), 0xd00-0xd2c (OFDM/ATC) */
static const unsigned short rxd_bb[] = {
	0x0800, 0x0804, 0x0808, 0x080c, 0x0818, 0x0820, 0x0824, 0x082c, 0x0838, 0x0840, 0x0858, 0x085c,
	0x0860, 0x0864, 0x0870, 0x088c, 0x0900, 0x0948, 0x094c, 0x0950, 0x0954, 0x0a00, 0x0a04, 0x0a08,
	0x0a0c, 0x0a10, 0x0a14, 0x0a20, 0x0a24, 0x0a28, 0x0a2c, 0x0a70, 0x0a74, 0x0c04, 0x0c08, 0x0c0c,
	0x0c10, 0x0c14, 0x0c1c, 0x0c30, 0x0c50, 0x0c58, 0x0c5c, 0x0c60, 0x0c64, 0x0c68, 0x0c6c, 0x0c70,
	0x0c74, 0x0c78, 0x0c80, 0x0c84, 0x0c88, 0x0c90, 0x0c94, 0x0ca0, 0x0d00, 0x0d04, 0x0d08, 0x0d0c,
	0x0d10, 0x0d2c, 0x0d40, 0x0d44,
};
/* RF path A: mode (0x00), 0x01, 0x05, channel (0x18), RX gain (0x1a-0x1f), 0x52/0x56, LNA tables (0x30-0x33, 0x3e/0x3f),
 * 0x63-0x66, 0xb0, 0xdf, 0xef */
#ifndef RXD_NO_RF
static const unsigned char rxd_rf[] = {
	0x00, 0x01, 0x05, 0x08, 0x18, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x30, 0x31, 0x32, 0x33, 0x3e,
	0x3f, 0x52, 0x56, 0x63, 0x64, 0x65, 0x66, 0xb0, 0xdf, 0xef,
};
#endif

static void rxregs_dump(const char *tag)
{
	unsigned i;
	for (i = 0; i < sizeof(rxd_mac) / sizeof(rxd_mac[0]); i += 6)
		RXD_LOG("[rxregs %s] mac %04x=%08x %04x=%08x %04x=%08x %04x=%08x %04x=%08x %04x=%08x\n", tag,
			rxd_mac[i], RXD_R32(rxd_mac[i]),
			rxd_mac[i + 1 < sizeof(rxd_mac) / 2 ? i + 1 : i], RXD_R32(rxd_mac[i + 1 < sizeof(rxd_mac) / 2 ? i + 1 : i]),
			rxd_mac[i + 2 < sizeof(rxd_mac) / 2 ? i + 2 : i], RXD_R32(rxd_mac[i + 2 < sizeof(rxd_mac) / 2 ? i + 2 : i]),
			rxd_mac[i + 3 < sizeof(rxd_mac) / 2 ? i + 3 : i], RXD_R32(rxd_mac[i + 3 < sizeof(rxd_mac) / 2 ? i + 3 : i]),
			rxd_mac[i + 4 < sizeof(rxd_mac) / 2 ? i + 4 : i], RXD_R32(rxd_mac[i + 4 < sizeof(rxd_mac) / 2 ? i + 4 : i]),
			rxd_mac[i + 5 < sizeof(rxd_mac) / 2 ? i + 5 : i], RXD_R32(rxd_mac[i + 5 < sizeof(rxd_mac) / 2 ? i + 5 : i]));
	for (i = 0; i < sizeof(rxd_bb) / sizeof(rxd_bb[0]); i += 6)
		RXD_LOG("[rxregs %s] bb %04x=%08x %04x=%08x %04x=%08x %04x=%08x %04x=%08x %04x=%08x\n", tag,
			rxd_bb[i], RXD_R32(rxd_bb[i]),
			rxd_bb[i + 1 < sizeof(rxd_bb) / 2 ? i + 1 : i], RXD_R32(rxd_bb[i + 1 < sizeof(rxd_bb) / 2 ? i + 1 : i]),
			rxd_bb[i + 2 < sizeof(rxd_bb) / 2 ? i + 2 : i], RXD_R32(rxd_bb[i + 2 < sizeof(rxd_bb) / 2 ? i + 2 : i]),
			rxd_bb[i + 3 < sizeof(rxd_bb) / 2 ? i + 3 : i], RXD_R32(rxd_bb[i + 3 < sizeof(rxd_bb) / 2 ? i + 3 : i]),
			rxd_bb[i + 4 < sizeof(rxd_bb) / 2 ? i + 4 : i], RXD_R32(rxd_bb[i + 4 < sizeof(rxd_bb) / 2 ? i + 4 : i]),
			rxd_bb[i + 5 < sizeof(rxd_bb) / 2 ? i + 5 : i], RXD_R32(rxd_bb[i + 5 < sizeof(rxd_bb) / 2 ? i + 5 : i]));
#ifndef RXD_NO_RF	/* RXD_NO_RF: MAC + BB only (the 8723cs scan-end dump) */
	for (i = 0; i < sizeof(rxd_rf); i += 6)
		RXD_LOG("[rxregs %s] rf %02x=%05x %02x=%05x %02x=%05x %02x=%05x %02x=%05x %02x=%05x\n", tag,
			rxd_rf[i], RXD_RF(rxd_rf[i]),
			rxd_rf[i + 1 < sizeof(rxd_rf) ? i + 1 : i], RXD_RF(rxd_rf[i + 1 < sizeof(rxd_rf) ? i + 1 : i]),
			rxd_rf[i + 2 < sizeof(rxd_rf) ? i + 2 : i], RXD_RF(rxd_rf[i + 2 < sizeof(rxd_rf) ? i + 2 : i]),
			rxd_rf[i + 3 < sizeof(rxd_rf) ? i + 3 : i], RXD_RF(rxd_rf[i + 3 < sizeof(rxd_rf) ? i + 3 : i]),
			rxd_rf[i + 4 < sizeof(rxd_rf) ? i + 4 : i], RXD_RF(rxd_rf[i + 4 < sizeof(rxd_rf) ? i + 4 : i]),
			rxd_rf[i + 5 < sizeof(rxd_rf) ? i + 5 : i], RXD_RF(rxd_rf[i + 5 < sizeof(rxd_rf) ? i + 5 : i]));
#endif
}
#ifdef RXD_RF_WIDE
/* every RF path A register 0x00..0xFF, 8 per line (PHE_RFWIDE): the RF-side diff between the two drivers */
static void rxregs_rf_wide(const char *tag)
{
	unsigned a;
	for (a = 0; a < 0x100; a += 8)
		RXD_LOG("[rfwide %s] %02x: %05x %05x %05x %05x %05x %05x %05x %05x\n", tag, a, RXD_RF(a), RXD_RF(a + 1), RXD_RF(a + 2),
			RXD_RF(a + 3), RXD_RF(a + 4), RXD_RF(a + 5), RXD_RF(a + 6), RXD_RF(a + 7));
}
#endif
#endif
