/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

/* rtw88_stubs.c -- OURS: the rtw88 modules we do not vendor yet, as no-op stubs with the real signatures, plus the
 * debug sink and the (unused) mac80211 ops table. Each stub is a known hole:
 *   coex.c  -- the BT/WiFi coexistence engine (the 8703B shares its antenna with the Bluetooth half our BT PDD drives;
 *              without coex the PTA runs the chip's power-on defaults). Vendor coex.c when WiFi and BT must share.
 *   regd.c  -- regulatory domains: we pin the world-wide domain (RTW_REGD_WW) for the tx-power tables.
 *   bf.c    -- beamforming (not on an 11n 1x1 chip).
 *   sar.c   -- specific-absorption-rate limits: unlimited (the regulatory table still caps the power).
 *   led.c   -- no LED.  debug.c -- rtw_dbg goes to the kernel log, gated by rtw_debug_mask. */
#include "../rtw88/main.h"
#include "../rtw88/coex.h"
#include "../rtw88/regd.h"
#include "../rtw88/bf.h"
#include "../rtw88/sar.h"
#include "../rtw88/debug.h"

unsigned int rtw_debug_mask = RTW_DBG_FW | RTW_DBG_EFUSE | RTW_DBG_SDIO | RTW_DBG_UNEXP | RTW_DBG_STATE | RTW_DBG_RX | RTW_DBG_RFK;	/* RFK: the IQK result (W4 boot 255: OFDM TX dead) */

void rtw_dbg(struct rtw_dev *rtwdev, enum rtw_debug_mask mask, const char *fmt, ...)
{
	char buf[200];
	va_list ap;
	if (!(rtw_debug_mask & mask))
		return;
	va_start(ap, fmt);
	shim_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	shim_log("rtw88: %s", buf);
}

/* rtw_ops = rtw88's own mac80211.c (vendored unmodified 2026-10-10); a64wlan_glue.c calls its ops in mac80211's order

/* ---- regd ---- */
static const struct rtw_regulatory shim_regd_ww = { .alpha2 = "00", .txpwr_regd_2g = RTW_REGD_WW, .txpwr_regd_5g = RTW_REGD_WW };
int rtw_regd_init(struct rtw_dev *rtwdev) { rtwdev->regd.state = RTW_REGD_STATE_WORLDWIDE; rtwdev->regd.regulatory = &shim_regd_ww; rtwdev->regd.dfs_region = NL80211_DFS_UNSET; return 0; }
int rtw_regd_hint(struct rtw_dev *rtwdev) { (void)rtwdev; return 0; }
u8 rtw_regd_get(struct rtw_dev *rtwdev) { (void)rtwdev; return RTW_REGD_WW; }
bool rtw_regd_has_alt(u8 regd, u8 *regd_alt) { (void)regd; (void)regd_alt; return false; }
bool rtw_regd_srrc(struct rtw_dev *rtwdev) { (void)rtwdev; return false; }

/* ---- sar ---- */
const struct cfg80211_sar_capa rtw_sar_capa = { .type = NL80211_SAR_TYPE_POWER, .num_freq_ranges = 0, .freq_ranges = 0 };
s8 rtw_query_sar(struct rtw_dev *rtwdev, const struct rtw_sar_arg *arg) { (void)rtwdev; (void)arg; return (s8)0x7f; }
int rtw_set_sar_specs(struct rtw_dev *rtwdev, const struct cfg80211_sar_specs *sar) { (void)rtwdev; (void)sar; return -EOPNOTSUPP; }

/* ---- bf ---- */
void rtw_bf_disassoc(struct rtw_dev *rtwdev, struct ieee80211_vif *vif, struct ieee80211_bss_conf *bss_conf) { (void)rtwdev; (void)vif; (void)bss_conf; }
void rtw_bf_assoc(struct rtw_dev *rtwdev, struct ieee80211_vif *vif, struct ieee80211_bss_conf *bss_conf) { (void)rtwdev; (void)vif; (void)bss_conf; }	/* VHT beamformee only (rtw_bf_assoc returns early without VHT caps) */

/* ---- coex: vendored (rtw88/coex.c) since W4, 2026-10-10 -- the arbiter grant was the missing TX ---- */
