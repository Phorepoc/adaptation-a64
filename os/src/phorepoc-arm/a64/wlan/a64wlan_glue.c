/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

/* a64wlan_glue.c -- OURS: the C half of a64wlan.cpp, the part that sees the rtw88 headers: the Linux sdio_func /
 * mmc_host objects the driver expects, the probe, and the station sequencer -- the role mac80211 plays for rtw88:
 * one station interface (vif) and one peer (the AP), driven through rtw88's OWN ops table (rtw88/mac80211.c,
 * vendored unmodified) in the order mac80211 calls them. Every step below cites the mac80211 source it mirrors;
 * the derivation and the diff against the earlier glue.
 * Called from the WLAN DFC thread (a64wlan.cpp / a64wlanpdd.cpp RunCmd), except a64wlan_tx (any thread: it only
 * queues). */
#include "rtw88/main.h"
#include "rtw88/sdio.h"
#include "rtw88/reg.h"
#include "rtw88/rtw8703b.h"
#include "rtw88/debug.h"
#include "rtw88/fw.h"
#include "rtw88/coex.h"
#include "rtw88/tx.h"
#include "rtw88/phy.h"
#include "rtw88/ps.h"
#include "rtw88/sec.h"
#include "a64wlan_api.h"
static struct rtw_dev *a64_rtwdev;
#define RXD_R32(a) rtw_read32(a64_rtwdev, (a))
#define RXD_RF(a) rtw_read_rf(a64_rtwdev, RF_PATH_A, (a), RFREG_MASK)
#define RXD_LOG shim_log
#ifdef PHE_RFWIDE
#define RXD_RF_WIDE
#endif
#if defined(PHE_RXREGS) || defined(PHE_RFWIDE)	/* opt-in (A64DEFS=-DPHE_RXREGS / -DPHE_RFWIDE) */
#include "rxregs_dump.h"
#endif

/* the host as the sunxi SMHC1 behaves under a64sdio.cpp: PIO, 512-byte blocks, one request up to 64 KB */
static struct mmc_host a64_host = { .max_req_size = 65536, .max_blk_size = 512, .max_blk_count = 511, .caps = 0 };
static struct mmc_card a64_card = { .host = &a64_host };
static struct sdio_func a64_func = { .card = &a64_card, .num = 1, .cur_blksize = 512, .max_blksize = 512, .dev = { .name = "mmc1:0001:1" } };
static const struct sdio_device_id a64_id = { .class = 0, .vendor = 0x024c, .device = 0xb703, .driver_data = (unsigned long)&rtw8703b_hw_spec };
static struct rtw_dev *a64_rtwdev;

/* mac80211's per-interface / per-station state, as far as rtw88 reads it */
static union {
	struct ieee80211_vif vif;
	unsigned char raw[sizeof(struct ieee80211_vif) + sizeof(struct rtw_vif) + 16];
} a64_vif_mem __attribute__((aligned(8)));
static union {
	struct ieee80211_sta sta;
	unsigned char raw[sizeof(struct ieee80211_sta) + sizeof(struct rtw_sta_info) + 16];
} a64_sta_mem __attribute__((aligned(8)));
static struct ieee80211_vif *a64_vif;		/* non-NULL = the interface is up (ieee80211_do_open done) */
static struct ieee80211_sta *a64_ap_sta;	/* non-NULL = the AP is an inserted station (NONE..AUTHORIZED) */
static u8 a64_bssid[ETH_ALEN];			/* link->u.mgd.bssid: bss_conf.bssid points here, zero = none */
static int a64_scanning;			/* SCAN_SW_SCANNING */
static int a64_chanctx;				/* a channel context is in use (from the Join to the leave) */
static int a64_op_channel;			/* its channel */
static unsigned a64_filter_flags;		/* local->filter_flags */
static int a64_short_slot;			/* the UMAC's dot11SlotTime (9 us) -> bss_conf.use_short_slot */
static struct ieee80211_tx_queue_params a64_umac_ac[IEEE80211_NUM_ACS];	/* the UMAC's ConfigureAC, mac80211 AC order */
static int a64_umac_ac_valid;

/* ---------------------------------------------------------------- firmware (a64wlan.cpp, from the ROM) */
void shim_firmware_provide(const char *name, const unsigned char *data, size_t size);	/* shim_rt.cpp */
void a64wlan_set_firmware(const unsigned char *data, unsigned size) { shim_firmware_provide("rtw8703b_fw.bin", data, size); }
void a64wlan_set_wow_firmware(const unsigned char *data, unsigned size) { shim_firmware_provide("rtw8703b_wow_fw.bin", data, size); }

/* ---------------------------------------------------------------- probe */
int a64wlan_probe(void)
{
	struct ieee80211_hw *hw;
	int r = rtw_sdio_probe(&a64_func, &a64_id);
	if (r)
		return r;
	hw = sdio_get_drvdata(&a64_func);
	a64_rtwdev = hw->priv;
	/* the PinePhone's efuse holds no MAC (normal: Linux takes local-mac-address, which U-Boot derives from the SoC
	 * SID); rtw88 made a random locally-administered one, some APs refuse those (W4 boot 240): Realtek's OUI + the
	 * random tail (a stable SID-derived tail is a later rung). mac80211 copies perm_addr to the interface. */
	a64_rtwdev->efuse.addr[0] = 0x00; a64_rtwdev->efuse.addr[1] = 0xe0; a64_rtwdev->efuse.addr[2] = 0x4c;
	SET_IEEE80211_PERM_ADDR(hw, a64_rtwdev->efuse.addr);
	/* the antenna grant (boots 257-271): our MAC's ACK exchange worked only while WiFi held the grant in software
	 * (GNT_WL high: boot 257, before coex's first run -- the AP's ACK was seen, the association completed); under the
	 * hardware arbiter (PTA) every boot since misses ACKs both ways, BT off (265, 267) or BT on (270: coex still reads
	 * bt_disabled from the scoreboard, so no BT cooperates). Boot 261's wifi_only got no answer at all, but so did
	 * 268/269 under PTA (the AP's silence is not the grant). wifi_only again, with the real ACK reports now. */
	/* (efuse.btcoex as the efuse says -- Linux; boots 273-291 forced wifi_only, no difference) */
	shim_log("[wlan] rtw88 (VENDOR_DIFF: MGMT on HIQ, HWSEQ 0xff, no ext-XTAL bits, WiFi-only coex table)\n");
	shim_log("[wlan] probed: chip %s, mac=%pM (efuse had none) btcoex %u plan=0x%02x rf paths=%u fw \"%s\"\n",
		 a64_rtwdev->chip->fw_name ? "rtw8703b" : "?", a64_rtwdev->efuse.addr, (unsigned)a64_rtwdev->efuse.btcoex,
		 a64_rtwdev->efuse.channel_plan, a64_rtwdev->hal.rf_path_num, a64_rtwdev->chip->fw_name);
	return 0;
}

/* the carrier offset of the frame being received, kHz (rtw8703b query_phy_status_ofdm: path_cfotail * 2.5, OFDM
 * frames only), read from the rx sink while that frame is delivered */
int a64wlan_last_cfo_khz(void) { return a64_rtwdev ? a64_rtwdev->dm_info.cfo_tail[RF_PATH_A] : 0; }
/* CFO tracking (PHE difference 4, rtw88-vs-8723cs.md Summary 3: rtw88 has no 8703B cfo_track hook; the vendor's phydm
 * runs one). The vendor algorithm as our rtl8703b/r8703b_dm.c cfo_track() writes it down (rtl8723cs-fresh-driver.md
 * FD:301-309): every OFDM frame's offset is summed; a 2 s tick, only while linked with BT off, averages them, starts
 * adjusting beyond +-20 kHz and stops inside +-10 kHz, moving the crystal cap one step toward zero offset; unlinked (or
 * BT on) the cap walks back to the efuse value one step per tick; BB 0xD2C bit11 (ATC) is on while |avg| >= 80 kHz.
 * The previous experiment (boots 283-286: magnitude-led steps, always on) is gone. */
static int a64_powered(void);
static int a64_cfo_sum, a64_cfo_cnt, a64_cfo_adjust = 1;
static unsigned a64_cfo_pkts, a64_cfo_pkts_pre;
static unsigned long a64_cfo_next;
static u8 a64_xtal_def = 0xff;
void a64wlan_cfo_feed(int khz)
{
	a64_cfo_pkts++;
	a64_cfo_sum += khz;
	a64_cfo_cnt++;
}
static void a64_set_crystal(struct rtw_dev *d, u8 cap)
{
	d->efuse.crystal_cap = cap;
	rtw_write32_mask(d, REG_AFE_CTRL3, BIT_MASK_XTAL, (u32)(cap | (cap << 6)));
}
static void a64_cfo_track(void)
{
	struct rtw_dev *d = a64_rtwdev;
	u8 cap, def;
	int avg;
	if (!d || !a64_powered() || !a64_vif)
		return;
	if (a64_xtal_def == 0xff)
		a64_xtal_def = d->efuse.crystal_cap & 0x3f;
	def = a64_xtal_def;
	cap = d->efuse.crystal_cap & 0x3f;
	if (!a64_vif->cfg.assoc || !d->coex.stat.bt_disabled) {
		a64_cfo_adjust = 1;
		if (cap != def)
			a64_set_crystal(d, (u8)(cap < def ? cap + 1 : cap - 1));
		rtw_write32_mask(d, 0xd2c, BIT(11), 1);
		a64_cfo_sum = a64_cfo_cnt = 0;
		return;
	}
	if (a64_cfo_pkts == a64_cfo_pkts_pre || !a64_cfo_cnt)
		return;
	a64_cfo_pkts_pre = a64_cfo_pkts;
	avg = a64_cfo_sum / a64_cfo_cnt;
	a64_cfo_sum = a64_cfo_cnt = 0;
	if (!a64_cfo_adjust && (avg > 20 || avg < -20))
		a64_cfo_adjust = 1;
	else if (a64_cfo_adjust && avg <= 10 && avg >= -10)
		a64_cfo_adjust = 0;
	if (a64_cfo_adjust) {
		int c = cap + (avg > 10 ? 1 : avg < -10 ? -1 : 0);
		if (c < 0) c = 0;
		if (c > 0x3f) c = 0x3f;
		if (c != cap) {
			a64_set_crystal(d, (u8)c);
			shim_log("[wlan] cfo: mean %d kHz -> crystal cap 0x%02x\n", avg, c);
		}
	}
	rtw_write32_mask(d, 0xd2c, BIT(11), (avg < 80 && avg > -80) ? 0 : 1);
}
int a64wlan_is_probed(void) { return a64_rtwdev != 0; }
int a64wlan_is_up(void) { return a64_vif != 0; }
void a64wlan_mac_addr(unsigned char out[6]) { if (a64_rtwdev) memcpy(out, a64_rtwdev->efuse.addr, 6); else memset(out, 0, 6); }
int a64wlan_channel(void) { return a64_rtwdev ? a64_rtwdev->hal.current_channel : 0; }
static int a64_powered(void) { return a64_rtwdev && test_bit(RTW_FLAG_POWERON, a64_rtwdev->flags); }

/* the SDIO "interrupt", polled from a64wlan.cpp: the handler rtw_sdio_request_irq claimed. Only while the MAC is
 * powered (rtw_core_stop masks HIMR before an idle power-down; the chip serves no CMD53 while its MAC is off) */
#ifdef PHE_MACCNT
static unsigned a64_maccnt_n;
static void a64_maccnt(const char *when);
#endif
void a64wlan_irq_poll(void)
{
	if (a64_powered() && test_bit(RTW_FLAG_RUNNING, a64_rtwdev->flags) && a64_func.irq_handler)
		a64_func.irq_handler(&a64_func);
	if (a64_powered() && (long)(jiffies - a64_cfo_next) >= 0) {	/* the vendor's 2 s watchdog, for the CFO tracker */
		a64_cfo_next = jiffies + msecs_to_jiffies(2000);
		a64_cfo_track();
	}
#ifdef PHE_MACCNT
	if (a64_powered() && a64_chanctx && !a64_scanning && a64_maccnt_n < 30 && (long)(jiffies - a64_cfo_next) >= -1000 && (long)(jiffies - a64_cfo_next) < -980) {
		a64_maccnt_n++;
		a64_maccnt("joined");	/* ~every 2 s while joined, 30 at most */
	}
#endif
}

static struct ieee80211_channel *a64wlan_chan_of(int number)
{
	struct ieee80211_supported_band *sb = a64_rtwdev->hw->wiphy->bands[NL80211_BAND_2GHZ];
	int i;
	if (!sb)
		return (struct ieee80211_channel *)0;
	for (i = 0; i < sb->n_channels; ++i)
		if (sb->channels[i].hw_value == number)
			return &sb->channels[i];
	return (struct ieee80211_channel *)0;
}

static int a64_set_chandef(int number)
{
	struct ieee80211_hw *hw = a64_rtwdev->hw;
	struct ieee80211_channel *ch = a64wlan_chan_of(number);
	if (!ch)
		return -EINVAL;
	hw->conf.chandef.chan = ch;
	hw->conf.chandef.width = NL80211_CHAN_WIDTH_20_NOHT;
	hw->conf.chandef.center_freq1 = ch->center_freq;
	return 0;
}

/* the registers that decide whether our MAC answers (logged at the Join and the association) */
static void a64_log_state(const char *when)
{
	struct rtw_dev *d = a64_rtwdev;
	if (!a64_powered()) {
		shim_log("[wlan] state %s: powered off (IPS)\n", when);
		return;
	}
	shim_log("[wlan] state %s: RRSR=0x%08x coex 6c0=0x%08x 6c4=0x%08x LTE_COEX_CTRL=0x%08x stop_dm %d ant %u | RCR=0x%08x "
		 "RXFLTMAP0/1/2=%04x/%04x/%04x BCN_CTRL=0x%02x CR=0x%08x AID(6a8)=0x%04x MACID=%02x:%02x:%02x:%02x:%02x:%02x "
		 "BSSID=%02x:%02x:%02x:%02x:%02x:%02x ch %u\n", when,
		 rtw_read32(d, REG_RRSR), rtw_read32(d, REG_BT_COEX_TABLE0), rtw_read32(d, REG_BT_COEX_TABLE1),
		 rtw_coex_read_indirect_reg(d, LTE_COEX_CTRL), (int)d->coex.stop_dm, (unsigned)d->coex.dm.cur_ant_pos_type,
		 rtw_read32(d, REG_RCR), rtw_read16(d, REG_RXFLTMAP0), rtw_read16(d, REG_RXFLTMAP1), rtw_read16(d, REG_RXFLTMAP2),
		 rtw_read8(d, REG_BCN_CTRL), rtw_read32(d, REG_CR), rtw_read16(d, 0x06a8) & 0x7ff,
		 rtw_read8(d, 0x0610), rtw_read8(d, 0x0611), rtw_read8(d, 0x0612), rtw_read8(d, 0x0613), rtw_read8(d, 0x0614), rtw_read8(d, 0x0615),
		 rtw_read8(d, 0x0618), rtw_read8(d, 0x0619), rtw_read8(d, 0x061a), rtw_read8(d, 0x061b), rtw_read8(d, 0x061c), rtw_read8(d, 0x061d),
		 d->hal.current_channel);
	/* did rtw8703b_mac_tbl land? (byte reads; the table writes RRSR 440-442 = 5d 01 00, SPEC_SIFS 428/429 = 0a 10,
	 * 638-640 = 28 | resp SIFS cck 0a 0a ofdm 0c 0c | ACKTO 40; rtw8703b_mac_init: SIFS 514-517 = 0a 10 0a 10) */
	shim_log("[wlan] state %s: table readback 440-442=%02x %02x %02x 428-429=%02x %02x 514-517=%02x %02x %02x %02x "
		 "638-640=%02x %02x %02x %02x %02x %02x %02x %02x %02x\n", when,
		 rtw_read8(d, 0x440), rtw_read8(d, 0x441), rtw_read8(d, 0x442), rtw_read8(d, 0x428), rtw_read8(d, 0x429),
		 rtw_read8(d, 0x514), rtw_read8(d, 0x515), rtw_read8(d, 0x516), rtw_read8(d, 0x517),
		 rtw_read8(d, 0x638), rtw_read8(d, 0x639), rtw_read8(d, 0x63a), rtw_read8(d, 0x63b), rtw_read8(d, 0x63c),
		 rtw_read8(d, 0x63d), rtw_read8(d, 0x63e), rtw_read8(d, 0x63f), rtw_read8(d, 0x640));
}

/* rtw8703b_phy_pwrtrack's inputs and outputs (every 2 s from the watchdog): the thermal reading against the efuse
 * base, the power-index delta it derives, the OFDM swing (0xc80[31:22]), the CCK swing filter (0xa22-0xa29) and the
 * crystal cap it shifts (0x2c[23:12]) -- the TX power and the carrier frequency */
static void a64_pwrtrk_log(const char *when)
{
	struct rtw_dev *d = a64_rtwdev;
	struct rtw_dm_info *dm = &d->dm_info;
	if (!a64_powered())
		return;
	shim_log("[wlan] pwrtrk %s: thermal now 0x%02x avg 0x%02x efuse 0x%02x | delta_pwr %d remnant cck %d ofdm %d "
		 "default ofdm %u cck %u | xtal 0x%03x (efuse 0x%02x) c80=%08x a22=%02x a24=%08x a28=%04x\n", when,
		 rtw_read_rf(d, RF_PATH_A, RF_T_METER, 0xfc00), dm->thermal_avg[RF_PATH_A], d->efuse.thermal_meter[0],
		 dm->delta_power_index[RF_PATH_A], dm->txagc_remnant_cck, dm->txagc_remnant_ofdm[RF_PATH_A],
		 dm->default_ofdm_index, dm->default_cck_index, (rtw_read32(d, REG_AFE_CTRL3) & BIT_MASK_XTAL) >> 12,
		 d->efuse.crystal_cap, rtw_read32(d, 0xc80), rtw_read8(d, 0xa22), rtw_read32(d, 0xa24), rtw_read16(d, 0xa28));
	/* DIG (the watchdog's first run follows scan 1 -- after it the APs stopped answering, boots 286/287): the IGI
	 * (0xc50[6:0]) sets the RX gain and how readily the medium reads busy; it moves on the false-alarm counts */
	shim_log("[wlan] dig %s: IGI 0x%02x (history %02x %02x %02x) | FA cck %u ofdm %u total %u | CCA cck %u ofdm %u | "
		 "ok cck %u ofdm %u ht %u | sta_cnt %u dig_disable %d\n", when, rtw_read8(d, 0xc50) & 0x7f,
		 dm->igi_history[0], dm->igi_history[1], dm->igi_history[2], dm->cck_fa_cnt, dm->ofdm_fa_cnt,
		 dm->total_fa_cnt, dm->cck_cca_cnt, dm->ofdm_cca_cnt, dm->cck_ok_cnt, dm->ofdm_ok_cnt, dm->ht_ok_cnt,
		 d->sta_cnt, test_bit(RTW_FLAG_DIG_DISABLE, d->flags) ? 1 : 0);
}

/* ---------------------------------------------------------------- the chip vs rtw88's own tables
 * Every register rtw8703b's MAC / BB / AGC / RF tables program (the tables' own parse, so the conditional branches
 * resolve as at load time; the last write to an address wins), read back and compared: the source says what the chip
 * should hold after rtw_power_on; a mismatch is either a later rtw88 write (channel, power, IQK, DIG -- expected) or
 * something outside rtw88 (firmware, our layer). Plus the MAC clock: the TSF counts microseconds by USTIME_TSF (0x55C)
 * MAC clocks; its rate against the ARM timer says whether the SIFS / ACK timers run at the right speed. */
#define A64_DIFF_MAX 640
static struct { u32 addr, data; u8 kind; } a64_diff[A64_DIFF_MAX];
static unsigned a64_diff_n;
static u8 a64_diff_kind;		/* 0 mac (8-bit), 1 bb/agc (32-bit), 2 rf (20-bit) */
static void a64_diff_cfg(struct rtw_dev *d, const struct rtw_table *tbl, u32 addr, u32 data)
{
	unsigned i;
	(void)d; (void)tbl;
	if (a64_diff_kind == 1 && (addr >= 0xf9 && addr <= 0xfe))
		return;
	if (a64_diff_kind == 2 && (addr == 0xffe || addr == 0xfe))
		return;
	for (i = 0; i < a64_diff_n; i++)
		if (a64_diff[i].addr == addr && a64_diff[i].kind == a64_diff_kind) {
			a64_diff[i].data = data;
			return;
		}
	if (a64_diff_n < A64_DIFF_MAX) {
		a64_diff[a64_diff_n].addr = addr;
		a64_diff[a64_diff_n].data = data;
		a64_diff[a64_diff_n].kind = a64_diff_kind;
		a64_diff_n++;
	}
}
static void a64_diff_collect(const struct rtw_table *tbl, u8 kind)
{
	struct rtw_table t = { .data = tbl->data, .size = tbl->size, .parse = tbl->parse, .do_cfg = a64_diff_cfg,
			       .rf_path = tbl->rf_path };
	a64_diff_kind = kind;
	t.parse(a64_rtwdev, &t);
}
static void a64_table_diff(const char *when)
{
	struct rtw_dev *d = a64_rtwdev;
	const struct rtw_chip_info *chip = d->chip;
	char line[200];
	unsigned i, len = 0, mism = 0, checked = 0;
	ktime_t t0, t1;
	u32 tsf0, tsf1;
	if (!a64_powered())
		return;
	a64_diff_n = 0;
	a64_diff_collect(chip->mac_tbl, 0);
	a64_diff_collect(chip->bb_tbl, 1);
	a64_diff_collect(chip->agc_tbl, 1);
	a64_diff_collect(chip->rf_tbl[RF_PATH_A], 2);
	for (i = 0; i < a64_diff_n; i++) {
		u32 want = a64_diff[i].data, got;
		if (a64_diff[i].kind == 0) {
			want &= 0xff;
			got = rtw_read8(d, a64_diff[i].addr);
		} else if (a64_diff[i].kind == 1) {
			if (a64_diff[i].addr == 0xc78)		/* the AGC table's write port */
				continue;
			got = rtw_read32(d, a64_diff[i].addr);
		} else {
			want &= RFREG_MASK;
			got = rtw_read_rf(d, RF_PATH_A, a64_diff[i].addr, RFREG_MASK);
		}
		checked++;
		if (got == want)
			continue;
		mism++;
		len += (unsigned)snprintf(line + len, sizeof(line) - len, " %c%03x:%x>%x", "MBR"[a64_diff[i].kind],
					  a64_diff[i].addr, want, got);
		if (len > sizeof(line) - 40) {
			shim_log("[wlan] diff %s:%s\n", when, line);
			len = 0;
		}
	}
	if (len)
		shim_log("[wlan] diff %s:%s\n", when, line);
	shim_log("[wlan] diff %s: %u table registers checked, %u differ (M = MAC 8-bit, B = BB 32-bit, R = RF path A; "
		 "table>chip)\n", when, checked, mism);
	/* the MAC clock: TSF microseconds per ARM microsecond over 200 ms (boot 271: 100127 in 200257 = half speed, with
	 * USTIME_TSF reading 0x50 after the window although 0x28 at the table check): as found, then with USTIME_TSF set
	 * to the table's 0x28. 1.0 with 0x28 = a 40 MHz MAC clock (only the TSF divider was off); 0.5 = a 20 MHz MAC
	 * clock, every EDCA / SIFS / ACK timer (USTIME_EDCA 0x28) twice as long */
	{
		int pass;
		for (pass = 0; pass < 2; pass++) {
			u8 us0, us1;
			if (pass == 1)
				rtw_write8(d, REG_USTIME_TSF, 0x28);
			us0 = rtw_read8(d, REG_USTIME_TSF);
			t0 = ktime_get();
			tsf0 = rtw_read32(d, 0x0560);
			msleep(200);
			tsf1 = rtw_read32(d, 0x0560);
			t1 = ktime_get();
			us1 = rtw_read8(d, REG_USTIME_TSF);
			shim_log("[wlan] diff %s: pass %d TSF advanced %u us in %lld us of ARM time (USTIME_TSF 0x%02x -> 0x%02x, "
				 "USTIME_EDCA 0x%02x)\n", when, pass, tsf1 - tsf0, (long long)ktime_us_delta(t1, t0), us0, us1,
				 rtw_read8(d, REG_USTIME_EDCA));
		}
	}
	/* TX power: every management frame (probe, authentication, association) and every ACK to a CCK frame goes out
	 * at 1 Mbps CCK; the CCK indexes live in 0xe08[15:8] (1M) and 0x86c[31:8] (2/5.5/11M), written by
	 * rtw8723x_set_tx_power_index from hal->tx_pwr_tbl (efuse bases + by-rate + limits), not by any table */
	{
		const struct rtw_txpwr_idx *p = &d->efuse.txpwr_idx_table[0];
		const u8 *t = (const u8 *)d->hal.tx_pwr_tbl[RF_PATH_A];
		shim_log("[wlan] diff %s: txpwr regs e08=%08x 86c=%08x e00=%08x e04=%08x e14=%08x | tbl 1M %u 2M %u 11M %u 6M %u "
			 "54M %u MCS7 %u\n", when, rtw_read32(d, 0xe08), rtw_read32(d, 0x86c), rtw_read32(d, 0xe00),
			 rtw_read32(d, 0xe04), rtw_read32(d, 0xe14), t[DESC_RATE1M], t[DESC_RATE2M], t[DESC_RATE11M],
			 t[DESC_RATE6M], t[DESC_RATE54M], t[DESC_RATEMCS7]);
		shim_log("[wlan] diff %s: efuse cck_base %u %u %u %u %u %u bw40_base %u %u %u %u %u | xtal 0x%02x thermal 0x%02x "
			 "board 0x%02x bt 0x%02x pa 0x%02x regd %u plan 0x%02x\n", when,
			 p->pwr_idx_2g.cck_base[0], p->pwr_idx_2g.cck_base[1], p->pwr_idx_2g.cck_base[2],
			 p->pwr_idx_2g.cck_base[3], p->pwr_idx_2g.cck_base[4], p->pwr_idx_2g.cck_base[5],
			 p->pwr_idx_2g.bw40_base[0], p->pwr_idx_2g.bw40_base[1], p->pwr_idx_2g.bw40_base[2],
			 p->pwr_idx_2g.bw40_base[3], p->pwr_idx_2g.bw40_base[4], d->efuse.crystal_cap, d->efuse.thermal_meter[0],
			 d->efuse.rf_board_option, d->efuse.bt_setting, d->efuse.pa_type_2g, d->efuse.regd, d->efuse.channel_plan);
	}
	shim_log("[wlan] diff %s: clocks SYS_ISO 0x%04x SYS_FUNC_EN 0x%04x SYS_CLKR 0x%04x AFE 0x10=%08x 0x14=%08x "
		 "0x24=%08x 0x28=%08x 0x2c=%08x 0x78=%08x 0x7c=%08x SYS_CFG 0xf0=%08x\n", when,
		 rtw_read16(d, 0x0000), rtw_read16(d, 0x0002), rtw_read16(d, 0x0008), rtw_read32(d, 0x0010),
		 rtw_read32(d, 0x0014), rtw_read32(d, 0x0024), rtw_read32(d, 0x0028), rtw_read32(d, 0x002c),
		 rtw_read32(d, 0x0078), rtw_read32(d, 0x007c), rtw_read32(d, 0x00f0));
}

/* What the working driver on this phone (Mobian loads Realtek's rtl8723cs vendor driver, not rtw88) programs in
 * halbtc8703b1ant_init_hw_config and rtw88's 8703b never does: enable_gnt_to_gpio(FALSE) = 0x73[3], 0x4e[6], 0x67[0]
 * cleared; in the concurrent (BT-present) branch BB 0x953[1] set and RF 0x01[0] cleared. */
static void a64_vendor_coex_hw(void)
{
	struct rtw_dev *d = a64_rtwdev;
	if (!a64_powered())
		return;
	/* (0x73[3], 0x4e[6], 0x67[0] are NOT written any more: the vendor's halbtc8703b1ant_enable_gnt_to_gpio is compiled
	 * out -- BT_8703B_1ANT_ENABLE_GNTBT_TO_GPIO14 = 0, halbtc8703b1ant.h:23 -- and rtl8703b/ never writes them) */
	rtw_write8_set(d, 0x953, BIT(1));
	rtw_write_rf(d, RF_PATH_A, 0x1, BIT(0), 0);
}


#ifdef PHE_MACCNT
/* PHE diagnostic MACCNT: the MAC's own RX counters (REG_RXERR_RPT 0x664, type select in bits 31:28, count in 15:0 --
 * the vendor's rtw_dump_mac_rx_counters, hal_com.c: 3 CCK ok, 0 OFDM ok, 6 HT ok, 4/1/7 the CRC failures, 5/2/9 false
 * alarms) + our address registers vs the interface: does OFDM reach the MAC and fail its CRC, or never arrive; is the
 * station address the hardware answers for the one we transmit from */
static u16 a64_rxerr(u8 type)
{
	rtw_write32_mask(a64_rtwdev, 0x664, 0xf0000000, type);
	return (u16)rtw_read32(a64_rtwdev, 0x664);
}
static void a64_maccnt(const char *when)
{
	struct rtw_dev *d = a64_rtwdev;
	u8 mac[6], bss[6];
	int i;
	if (!a64_powered())
		return;
	for (i = 0; i < 6; i++) {
		mac[i] = rtw_read8(d, 0x610 + i);
		bss[i] = rtw_read8(d, 0x618 + i);
	}
	shim_log("[maccnt %s] ok cck %u ofdm %u ht %u | crc cck %u ofdm %u ht %u | fa cck %u ofdm %u ht %u | 0x610 %pM vif %pM 0x618 %pM bssid %pM\n",
		 when, a64_rxerr(3), a64_rxerr(0), a64_rxerr(6), a64_rxerr(4), a64_rxerr(1), a64_rxerr(7), a64_rxerr(5), a64_rxerr(2),
		 a64_rxerr(9), mac, a64_vif ? a64_vif->addr : mac, bss, a64_bssid);
}
#endif


/* ---------------------------------------------------------------- mac80211's helpers */
/* main.c ieee80211_configure_filter: a station's only filter flag is FIF_BCN_PRBRESP_PROMISC while it scans */
static void a64_configure_filter(void)
{
	/* (no FIF_CONTROL: boots 262/264 with RXFLTMAP1 = 0xffff heard no probe response and no "Test" at all) */
	unsigned new_flags = a64_scanning ? FIF_BCN_PRBRESP_PROMISC : 0u;
	unsigned changed = a64_filter_flags ^ new_flags;
	unsigned total = new_flags | (1u << 31);
	rtw_ops.configure_filter(a64_rtwdev->hw, changed, &total, 0);
	a64_filter_flags = total & ~(1u << 31);
}

/* iface.c __ieee80211_recalc_idle: idle = no channel context in use and not scanning; rtw_ops_config then leaves
 * or enters IPS (rtw88 ps.c: idle = the MAC powered off) */
static void a64_recalc_idle(void)
{
	struct ieee80211_hw *hw = a64_rtwdev->hw;
	/* no IPS once powered (boot 276): boot 260 never power-cycled the chip and heard 45+ probe responses from many
	 * APs; every boot with Linux's IPS (a fresh rtw_power_on -- tables, LCK -- before each scan and Join) is heard
	 * only by the nearest devices and by "Test" intermittently. The RTL8723BS rtw88 series (2026) keeps that chip
	 * family powered between scan and connect for the same SDIO reason (and drops the LCK at power-on). */
	const int idle = !a64_chanctx && !a64_scanning;
	const int was_idle = (hw->conf.flags & IEEE80211_CONF_IDLE) != 0;
	if (idle == was_idle)
		return;
	if (idle)
		hw->conf.flags |= IEEE80211_CONF_IDLE;
	else
		hw->conf.flags &= ~IEEE80211_CONF_IDLE;
	rtw_ops.config(hw, -1, IEEE80211_CONF_CHANGE_IDLE);
}

/* util.c ieee80211_set_wmm_default(link, bss_notify = true, enable_qos = false): 802.11-2007 table 7-37 for every
 * AC (aCWmin 15 -- not 11b-only --, aCWmax 1023, AIFSN 2, no TXOP), then BSS_CHANGED_QOS */
static void a64_set_wmm_default(void)
{
	struct ieee80211_tx_queue_params q;
	int ac;
	memset(&q, 0, sizeof(q));
	q.cw_min = 15;
	q.cw_max = 1023;
	q.aifs = 2;
	q.txop = 0;
	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++)
		rtw_ops.conf_tx(a64_rtwdev->hw, a64_vif, 0, (u16)ac, &q);
	a64_vif->bss_conf.qos = false;
	rtw_ops.bss_info_changed(a64_rtwdev->hw, a64_vif, &a64_vif->bss_conf, BSS_CHANGED_QOS);
}

/* ---------------------------------------------------------------- interface up */
/* iface.c ieee80211_do_open for a station + main.c ieee80211_hw_conf_init. Ends powered off (IPS), as Linux does:
 * the first scan or the Join powers the chip on again. Idempotent. */
int a64wlan_core_up(void)
{
	struct ieee80211_hw *hw;
	struct ieee80211_vif *vif;
	int r;
	if (!a64_rtwdev)
		return -ENODEV;
	if (a64_vif)
		return 0;
	hw = a64_rtwdev->hw;
	if (a64_set_chandef(1)) {
		shim_log("[wlan] no 2.4 GHz band registered\n");
		return -ENODEV;
	}
	hw->conf.flags = IEEE80211_CONF_IDLE;		/* mac80211 main.c: the initial state */
	/* drv_start */
	r = rtw_ops.start(hw);
	if (r)
		return r;
	/* drv_add_interface: a station interface with the permanent address */
	memset(&a64_vif_mem, 0, sizeof(a64_vif_mem));
	vif = &a64_vif_mem.vif;
	vif->type = NL80211_IFTYPE_STATION;
	memcpy(vif->addr, a64_rtwdev->efuse.addr, ETH_ALEN);
	memset(a64_bssid, 0, sizeof(a64_bssid));
	vif->bss_conf.vif = vif;
	vif->bss_conf.bssid = a64_bssid;
	vif->bss_conf.beacon_int = 100;
	/* probes at 6 Mbps OFDM (rtw_get_mgmt_rate: the lowest basic rate): their OFDM answers carry the PHY status CFO
	 * the crystal tracking needs (CCK frames have none; boot 284 got no OFDM frame at all with 1 Mbps probes) */
	vif->bss_conf.basic_rates = 0xf;
	vif->cfg.ps = false;				/* no LPS yet: the UMAC's SetPsMode is echoed */
	a64_vif = vif;
	shim_mac80211_set_vif(vif);
	r = rtw_ops.add_interface(hw, vif);
	if (r) {
		shim_mac80211_set_vif((struct ieee80211_vif *)0);
		a64_vif = (struct ieee80211_vif *)0;
		return r;
	}
	/* ieee80211_reset_erp_info -> link_info_change_notify, then the WMM defaults */
	rtw_ops.bss_info_changed(hw, vif, &vif->bss_conf, BSS_CHANGED_ERP_CTS_PROT | BSS_CHANGED_ERP_PREAMBLE | BSS_CHANGED_ERP_SLOT);
	a64_set_wmm_default();
	a64_configure_filter();
	/* ieee80211_hw_conf_init: every config bit (channel included: emulated chanctx) with conf.flags = IDLE ->
	 * rtw_set_channel, then rtw_enter_ips */
	rtw_ops.config(hw, -1, ~(IEEE80211_CONF_CHANGE_POWER | IEEE80211_CONF_CHANGE_SMPS));
	shim_log("[wlan] up: fw %u.%u.%u h2c %u, vif %pM mac_id %u port %u, %s\n",
		 a64_rtwdev->fw.version, a64_rtwdev->fw.sub_version, a64_rtwdev->fw.sub_index, a64_rtwdev->fw.h2c_version,
		 vif->addr, ((struct rtw_vif *)vif->drv_priv)->mac_id, ((struct rtw_vif *)vif->drv_priv)->port,
		 a64_powered() ? "powered" : "idle (IPS, powered off)");
	return 0;
}

/* a channel for a scan step (scan.c: config(CHANGE_CHANNEL) per channel) or, outside a scan / a join (the W2
 * demo), with the idle state left first */
int a64wlan_set_channel(int number)
{
	if (!a64_vif)
		return -ENODEV;
	if (a64_set_chandef(number))
		return -EINVAL;
	if (!a64_scanning && !a64_chanctx) {
		a64_chanctx = 1;			/* the demo: a channel in use, as a monitor would hold it */
		a64_op_channel = number;
		a64_recalc_idle();
	}
	rtw_ops.config(a64_rtwdev->hw, -1, IEEE80211_CONF_CHANGE_CHANNEL);
	return 0;
}

/* ---------------------------------------------------------------- software scan */
/* scan.c ieee80211_start_sw_scan / __ieee80211_scan_completed */
void a64wlan_scan_notify(int start)
{
	struct ieee80211_hw *hw;
	if (!a64_vif)
		return;
	hw = a64_rtwdev->hw;
	if (start) {
		a64_scanning = 1;
		/* (probes at the 11b basic rate, as Linux) */
		a64_recalc_idle();			/* leave IPS: a fresh power-on, the port registers restored */
		/* (boot 286 swept the crystal cap 0x00..0x3f one value per scan: all 8 probe responses came in scan 1 --
		 * cap 0x00 -- and none in any later scan, cap 0x00 again included: the crystal is not the variable; the
		 * break follows the first Join) */
		rtw_ops.sw_scan_start(hw, a64_vif, a64_vif->addr);
		a64_vendor_coex_hw();
		/* boot 289 test: IGI held at DIG's non-linked ceiling (DIG_CVRG_MAX 0x2a) instead of the scan's max coverage
		 * 0x1c -- boot 288: OFDM false alarms ~2000/s, CCA busy, and DIG never adapts (the UMAC scans back to back,
		 * the watchdog lands in scans). Linux's rare scans let DIG raise it before the authentication. */
		/* (boot 289 held the IGI at 0x2a: no change -- removed) */
		a64_configure_filter();			/* FIF_BCN_PRBRESP_PROMISC: every BSS's beacons */
	} else {
		a64_scanning = 0;
		if (a64_chanctx && !a64_set_chandef(a64_op_channel))	/* back to the operating channel */
			rtw_ops.config(hw, -1, IEEE80211_CONF_CHANGE_CHANNEL);
		a64_configure_filter();			/* RCR |= CBSSID_BCN */
		rtw_ops.sw_scan_complete(hw, a64_vif);
		a64_recalc_idle();			/* idle again unless joined: IPS */
	}
}

/* ---------------------------------------------------------------- join / association / leave */
static void a64_leave(void);

/* mlme.c ieee80211_prep_connection (auth) + ieee80211_auth's drv_mgd_prepare_tx, for the WHA Join:
 * chan.c ieee80211_add_chanctx (idle off first, then the channel), BSS_CHANGED_BSSID|BASIC_RATES|BEACON_INT, the AP
 * inserted as a station (sta_state NOTEXIST->NONE = sta_add), mgd_prepare_tx before the authentication frame (the
 * UMAC sends it right after the Join completes; done here on the WLAN thread: it runs the IQK) */
int a64wlan_join(int channel, const unsigned char *bssid, unsigned beacon_int)
{
	struct ieee80211_hw *hw;
	struct ieee80211_sta *sta;
	struct ieee80211_prep_tx_info info;
	int r;
	if (!a64_vif)
		return -ENODEV;
	hw = a64_rtwdev->hw;
	if (a64_ap_sta || a64_chanctx)
		a64_leave();				/* a new Join without a Reset: the old connection goes first */
	if (a64_set_chandef(channel))
		return -EINVAL;
	a64_chanctx = 1;
	a64_op_channel = channel;
	a64_recalc_idle();				/* idle off before the channel ("some drivers need that") */
	rtw_ops.config(hw, -1, IEEE80211_CONF_CHANGE_CHANNEL);
	memcpy(a64_bssid, bssid, ETH_ALEN);
	a64_vif->bss_conf.beacon_int = beacon_int ? (u16)beacon_int : 100;
	/* rtw_get_mgmt_rate: management frames go at the lowest BASIC rate (__ffs(basic_rates) + DESC_RATE1M). Boot 279
	 * test: 6 Mbps OFDM (bit 4) instead of the 11b set's 1 Mbps CCK -- every frame and ACK that failed so far was
	 * CCK; is the OFDM transmit path heard by the AP? */
	/* boot 291 test: the authentication / association at 6 Mbps OFDM -- boot 289: the AP's ACKs to our 1 Mbps CCK
	 * authentication arrive as 6 Mbps OFDM frames (rate idx 4, in pairs) and our MAC reports NOT acked: is a CCK
	 * transmission waiting for a CCK ACK? (279 tried this while "Test" answered nothing: no reading) */
	a64_vif->bss_conf.basic_rates = 0xf;
	rtw_ops.bss_info_changed(hw, a64_vif, &a64_vif->bss_conf, BSS_CHANGED_BSSID | BSS_CHANGED_BASIC_RATES | BSS_CHANGED_BEACON_INT);
	/* sta_info_insert: the AP, legacy rates only (no HT IE parsing yet: supp_rates = all twelve 2.4 GHz rates) */
	memset(&a64_sta_mem, 0, sizeof(a64_sta_mem));
	sta = &a64_sta_mem.sta;
	memcpy(sta->addr, bssid, ETH_ALEN);
	memcpy(sta->deflink.addr, bssid, ETH_ALEN);
	sta->deflink.sta = sta;
	sta->deflink.supp_rates[NL80211_BAND_2GHZ] = 0xfffu;
	sta->deflink.bandwidth = IEEE80211_STA_RX_BW_20;
	sta->deflink.rx_nss = 1;
	a64_ap_sta = sta;
	shim_mac80211_set_sta(sta);
	r = rtw_ops.sta_add(hw, a64_vif, sta);
	if (r) {
		shim_log("[wlan] join: sta_add failed %d\n", r);
		shim_mac80211_set_sta((struct ieee80211_sta *)0);
		a64_ap_sta = (struct ieee80211_sta *)0;
		return r;
	}
	memset(&info, 0, sizeof(info));
	info.subtype = IEEE80211_STYPE_AUTH;
	/* (boots 268-291 tested, one variable per boot, and removed for boot 292: MSR linked at the Join, no join IQK,
	 * the table re-apply, the vendor per-join RRSR+2 / RESP_SIFS writes, ACK observation via RCR ACF + RXFLTMAP1, the
	 * BTG select, max TX power, 6 Mbps management -- see the journal 2026-10-10--22-20. None changed the outcome: the
	 * cause was our sequence numbers, a64wlan_tx.) */
	rtw_ops.mgd_prepare_tx(hw, a64_vif, &info);
	a64_vendor_coex_hw();
#ifdef PHE_MACCNT
	a64_maccnt("join");
#endif
	{
		struct rtw_sta_info *si = (struct rtw_sta_info *)sta->drv_priv;
		shim_log("[wlan] join: %pM ch %d, sta mac_id %u rate_id %u ra_mask 0x%llx\n", bssid, channel, si->mac_id,
			 si->rate_id, (unsigned long long)si->ra_mask);
	}
	a64_log_state("join");
#ifdef PHE_RXREGS
	rxregs_dump("join");			/* the same list as rtl8703b/ (WIFI_DRV=8723cs) dumps: diff a boot pair */
#endif
	a64_pwrtrk_log("join");
	if (0)
		a64_table_diff("join");		/* (the boot-271 readback of every table register vs the source; on demand) */
	return 0;
}

/* mlme.c ieee80211_assoc_success / ieee80211_set_associated, for the WHA SetBssParameters (the UMAC sends it once
 * the association response said success): the AP's EDCA parameters (ieee80211_mgd_set_link_qos_params ->
 * conf_tx), then one bss_info_changed with ASSOC and the link changes -> rtw88: net type MGD_LINKED, AID (0x06a8),
 * coex, the reserved pages (PS-Poll / null templates), the firmware's default port, ERP slot -> EDCA */
void a64wlan_assoc(unsigned aid, unsigned dtim)
{
	struct ieee80211_hw *hw;
	int ac;
	if (!a64_vif || !a64_ap_sta)
		return;
	hw = a64_rtwdev->hw;
	if (a64_vif->cfg.assoc) {
		/* already associated (at the association response, as mac80211): the UMAC's SetBssParameters after the
		 * first beacon brings the DTIM -- mac80211 learns it from the beacon too (BSS_CHANGED_BEACON_INFO) */
		a64_vif->bss_conf.dtim_period = (u8)(dtim ? dtim : 1);
		rtw_ops.bss_info_changed(hw, a64_vif, &a64_vif->bss_conf, BSS_CHANGED_BEACON_INFO);
		shim_log("[wlan] associated: dtim %u (aid %u, set at the association response)\n", dtim, a64_vif->cfg.aid);
		return;
	}
	if (a64_umac_ac_valid)
		for (ac = 0; ac < IEEE80211_NUM_ACS; ac++)
			rtw_ops.conf_tx(hw, a64_vif, 0, (u16)ac, &a64_umac_ac[ac]);
	a64_vif->cfg.assoc = true;
	a64_vif->cfg.aid = (u16)aid;
	a64_vif->bss_conf.dtim_period = (u8)(dtim ? dtim : 1);
	a64_vif->bss_conf.use_short_slot = a64_short_slot != 0;
	a64_vif->bss_conf.qos = a64_umac_ac_valid != 0;
	a64_ap_sta->aid = (u16)aid;
	rtw_ops.bss_info_changed(hw, a64_vif, &a64_vif->bss_conf,
				 BSS_CHANGED_ASSOC | BSS_CHANGED_ERP_SLOT | BSS_CHANGED_QOS | BSS_CHANGED_BEACON_INFO);
	shim_log("[wlan] associated: aid %u dtim %u short slot %d qos %d\n", aid, dtim, a64_short_slot, a64_umac_ac_valid);
	a64_log_state("assoc");
}

/* mlme.c ieee80211_set_disassoc: ASSOC off (NO_LINK, AID 0), the station removed, the WMM defaults, BSSID zero, the
 * channel context released -> idle -> IPS */
static void a64_leave(void)
{
	struct ieee80211_hw *hw = a64_rtwdev->hw;
	if (a64_vif->cfg.assoc) {
		a64_vif->cfg.assoc = false;
		a64_vif->cfg.aid = 0;
		rtw_ops.bss_info_changed(hw, a64_vif, &a64_vif->bss_conf, BSS_CHANGED_ASSOC);
	}
	{
		struct rtw_vif *rv = (struct rtw_vif *)a64_vif->drv_priv;
		if (rv->net_type != RTW_NET_NO_LINK && a64_powered()) {	/* the Join's early link, undone */
			rv->net_type = RTW_NET_NO_LINK;
			rtw_vif_port_config(a64_rtwdev, rv, PORT_SET_NET_TYPE);
		}
	}
	if (a64_ap_sta) {
		rtw_ops.sta_remove(hw, a64_vif, a64_ap_sta);
		shim_mac80211_set_sta((struct ieee80211_sta *)0);
		a64_ap_sta = (struct ieee80211_sta *)0;
	}
	if (a64_powered())
		a64_set_wmm_default();
	memset(a64_bssid, 0, sizeof(a64_bssid));
	rtw_ops.bss_info_changed(hw, a64_vif, &a64_vif->bss_conf, BSS_CHANGED_BSSID);
	a64_chanctx = 0;
	a64_recalc_idle();
}
static void a64_clear_keys_hw(void);
void a64wlan_leave(void)
{
	if (a64_vif && (a64_ap_sta || a64_chanctx)) {
		a64_clear_keys_hw();		/* mac80211 removes the keys before the station (sta_info_destroy_part1) */
		a64_leave();
		shim_log("[wlan] left the BSS (%s)\n", a64_powered() ? "powered" : "idle, powered off");
	}
}

/* the UMAC's ConfigureAC (WHA order BE, BK, VI, VO; TXOP in us) -> mac80211's ieee80211_tx_queue_params (order VO,
 * VI, BE, BK; TXOP in 32 us units); programmed at the association (or at once when already associated) */
void a64wlan_conf_tx(const unsigned short cwmin[4], const unsigned short cwmax[4], const unsigned char aifsn[4], const unsigned short txop_us[4])
{
	static const int kAcOfWha[4] = { IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_VI, IEEE80211_AC_VO };
	int q;
	for (q = 0; q < 4; q++) {
		struct ieee80211_tx_queue_params *p = &a64_umac_ac[kAcOfWha[q]];
		memset(p, 0, sizeof(*p));
		p->cw_min = cwmin[q];
		p->cw_max = cwmax[q];
		p->aifs = aifsn[q];
		p->txop = (u16)(txop_us[q] / 32);
	}
	a64_umac_ac_valid = 1;
	if (a64_vif && a64_vif->cfg.assoc && a64_powered())
		for (q = 0; q < IEEE80211_NUM_ACS; q++)
			rtw_ops.conf_tx(a64_rtwdev->hw, a64_vif, 0, (u16)q, &a64_umac_ac[q]);
}
void a64wlan_set_slot_time(unsigned us) { a64_short_slot = (us == 9); }

void a64wlan_demo_report(void)
{
	if (!a64_powered())
		return;
	shim_log("[wlan] report: rx frames=%u APs=%u ch=%u HISR=0x%08x RX0_REQ_LEN=%u RCR=0x%08x\n",
		 a64wlan_rx_frames(), shim_mac80211_bss_count(), a64_rtwdev->hal.current_channel,
		 rtw_read32(a64_rtwdev, REG_SDIO_HISR), rtw_read16(a64_rtwdev, REG_SDIO_RX0_REQ_LEN), rtw_read32(a64_rtwdev, REG_RCR));
}

/* ---------------------------------------------------------------- transmit */
/* one 802.11 frame (header + body, as the UMAC hands it to WHA::SendPacket, or the PDD's probe request) through
 * rtw88's own tx op, with what mac80211's tx path fills: info->control.vif = the interface, control->sta = the AP
 * when the frame is addressed to it (an inserted, uploaded station -- management frames too). The completion comes
 * back through shim_mac80211's tx sink with aCookie (a dropped frame = not acked). REQ_TX_STATUS as mac80211 sets
 * it (rtw88 has REPORTS_TX_ACK_STATUS): mlme.c ieee80211_auth / ieee80211_send_assoc mark the authentication and
 * (re)association requests IEEE80211_TX_CTL_REQ_TX_STATUS | IEEE80211_TX_INTFL_MLME_CONN_TX, so their completion
 * is the chip's real ACK report (boot 232's "no CCX report" predates upstream 5f936768: the tables overwrote the
 * report enable, now re-set in rtw8723x_mac_postinit). Everything else: rtw88 SDIO reports it acked. */
static struct ieee80211_key_conf *a64_tx_key(unsigned sec);
int a64wlan_tx(const unsigned char *frame, unsigned len, unsigned mgmt, void *cookie, unsigned sec)
{
	struct sk_buff *skb;
	struct ieee80211_tx_info *info;
	struct ieee80211_tx_control ctl;
	struct ieee80211_key_conf *key = (struct ieee80211_key_conf *)0;
	unsigned headroom, tail = 0;
	if (!a64_vif)
		return -ENODEV;
	if (!test_bit(RTW_FLAG_RUNNING, a64_rtwdev->flags))
		return -EAGAIN;			/* idle (IPS): rtw_ops_tx would drop it */
	if (sec == A64WLAN_SEC_FAIL)
		return -EINVAL;			/* protected, no key: never transmitted (the PDD completes it as not sent) */
	if (sec != A64WLAN_SEC_NONE) {
		key = a64_tx_key(sec);
		if (!key)
			return -EINVAL;
		/* the chip appends the ICV (WEP, TKIP: 4) / the CCMP MIC (8) in space the packet length already counts --
		 * the vendor's TX path (rtl8723cs-fresh-driver.md, r8703b_trx.c); Linux's mac80211 + rtw88 is not the
		 * reference here (PHE difference 5) */
		tail = sec == A64WLAN_SEC_AES ? 8 : 4;
	}
	headroom = a64_rtwdev->chip->tx_pkt_desc_sz + 16;
	skb = shim_alloc_skb(headroom + len + tail + 8);
	if (!skb)
		return -ENOMEM;
	skb_reserve(skb, headroom);
	skb_put_data(skb, frame, len);
	if (tail)
		memset(skb_put(skb, tail), 0, tail);
	/* sequence numbers -- mac80211 tx.c ieee80211_tx_h_sequence, which runs before every driver: no number for
	 * control frames, frames shorter than a 3-address header, QoS-null; QoS unicast data from the station's per-TID
	 * counter; everything else (management, non-QoS data, multicast) from the interface counter
	 * (sdata->sequence_number += 0x10), the fragment number kept. rtw88 relies on it (its data path copies
	 * hdr->seq_ctrl into the descriptor). The 2010 UMAC sends every frame with sequence number 0 -- in Symbian's WHA
	 * model the hardware numbered them -- so the AP took each later frame of ours for a duplicate of the first: the
	 * router's own log (wlceventd, 2026-10-10) shows exactly one authentication per boot from us and nothing after it
	 * -- no association request, no deauthentication -- until its 1-hour inactivity timeout. */
	{
		static u16 a64_seq_vif, a64_seq_tid[IEEE80211_NUM_TIDS];
		u8 *h = skb->data;
		const unsigned type = (h[0] >> 2) & 3, subtype = (h[0] >> 4) & 0xf;
		if (type != 1 && len >= 24 && !(type == 2 && subtype == 0xc)) {
			u16 sc;
			if (type == 2 && (subtype & 0x8) && len >= 26 && !(h[4] & 1)) {
				const unsigned tid = h[24] & 0xf;
				sc = a64_seq_tid[tid];
				a64_seq_tid[tid] += 0x10;
			} else {
				sc = a64_seq_vif;
				a64_seq_vif += 0x10;
			}
			sc |= (u16)(h[22] & 0xf);
			h[22] = (u8)sc;
			h[23] = (u8)(sc >> 8);
		}
	}
	info = IEEE80211_SKB_CB(skb);
	memset(info, 0, sizeof(*info));
	info->control.vif = a64_vif;
	info->control.hw_key = key;			/* rtw_tx_pkt_info_update_sec: the descriptor's SEC_TYPE */
	if (mgmt && len >= 2 && (frame[1] & 0x03) == 0 &&
	    ((frame[0] & 0xfc) == 0xb0 || (frame[0] & 0xfc) == 0x00 || (frame[0] & 0xfc) == 0x20))	/* auth, assoc / reassoc req */
		info->flags |= IEEE80211_TX_CTL_REQ_TX_STATUS | IEEE80211_TX_INTFL_MLME_CONN_TX;
	skb->priority = mgmt ? 7 : 0;			/* the TID mac80211 gives management / best-effort frames = the qsel */
	skb_set_queue_mapping(skb, IEEE80211_AC_BE);
	if (shim_mac80211_tx_track(skb, cookie)) {
		shim_free_skb(skb);
		return -ENOSPC;
	}
	memset(&ctl, 0, sizeof(ctl));
	ctl.sta = (a64_ap_sta && len >= 10 && ether_addr_equal(frame + 4, a64_ap_sta->addr)) ? a64_ap_sta : (struct ieee80211_sta *)0;
	rtw_ops.tx(a64_rtwdev->hw, &ctl, skb);
	return 0;
}

/* the chip's transmit state after a scan (bring-up): are the queues drained, is TX paused; the coex state */
void a64wlan_tx_report(void)
{
	const struct rtw_coex *c;
	if (!a64_powered())
		return;
#ifdef PHE_RXREGS
	{
		static int dumped;		/* once, at the first scan end (boots 903/905 never reached a Join) */
		if (!dumped++)
			rxregs_dump("scan");
	}
#endif
#ifdef PHE_RFWIDE
	{
		static int wide;	/* once: RF 0x00..0xFF */
		if (!wide++)
			rxregs_rf_wide("scan");
	}
#endif
	shim_log("[wlan] tx: TXPAUSE=0x%02x TXPKT_EMPTY=0x%04x FWHW_TXQ_CTRL=0x%08x TXDMA_STATUS=0x%08x CR=0x%04x MACID=%pM, tx reports pending %u\n",
		 rtw_read8(a64_rtwdev, REG_TXPAUSE), rtw_read16(a64_rtwdev, REG_TXPKT_EMPTY), rtw_read32(a64_rtwdev, REG_FWHW_TXQ_CTRL),
		 rtw_read32(a64_rtwdev, REG_TXDMA_STATUS), rtw_read16(a64_rtwdev, REG_CR), a64_rtwdev->efuse.addr,
		 (unsigned)skb_queue_len(&a64_rtwdev->tx_report.queue));
	a64_pwrtrk_log("scan");
	c = &a64_rtwdev->coex;
	shim_log("[wlan] coex: stop_dm %d bt_disabled %d wl_connected %d cur_table %u cur_ps_tdma %u ant_pos 0x%x LTE_COEX_CTRL=0x%08x\n",
		 (int)c->stop_dm, (int)c->stat.bt_disabled, (int)c->stat.wl_connected, c->dm.cur_table, c->dm.cur_ps_tdma,
		 c->dm.cur_ant_pos_type, rtw_coex_read_indirect_reg(a64_rtwdev, LTE_COEX_CTRL));
}

/* the WLAN thread's TX kick (a64wlan.cpp TxDfc): rtw88 queues in rtw_ops.tx itself (its tx work), nothing to do */
void a64wlan_tx_flush(void) { }

/* ---------------------------------------------------------------- keys (WHA AddKey / RemoveKey) */
/* rtw88's own set_key op, called as mac80211's key.c ieee80211_key_enable_hw_accel does: a pairwise key with the AP's
 * station (rtw88 takes a free CAM entry from 4 up), a group key without one (entry = key index, default-key search).
 * aCam (the WHA entry index) only names our slot; aMac is the AP's address either way (sta->addr / broadcast). */
static union {
	struct ieee80211_key_conf k;
	unsigned char raw[sizeof(struct ieee80211_key_conf) + 32];
} a64_keys[32] __attribute__((aligned(8)));
static u8 a64_key_used[32];
int a64wlan_set_key(unsigned cam, unsigned algo, unsigned keyid, const unsigned char *mac, const unsigned char *key,
		    unsigned keylen, int group)
{
	struct ieee80211_key_conf *k;
	u32 cipher;
	int r;
	(void)mac;
	if (!a64_vif || !a64_powered())
		return -ENODEV;
	if (cam >= 32 || keyid > 3 || !keylen || keylen > 16)
		return -EINVAL;
	if (algo == A64WLAN_KEY_WEP)
		cipher = keylen == 5 ? WLAN_CIPHER_SUITE_WEP40 : keylen == 13 ? WLAN_CIPHER_SUITE_WEP104 : 0;
	else if (algo == A64WLAN_KEY_TKIP && keylen == 16)
		cipher = WLAN_CIPHER_SUITE_TKIP;
	else if (algo == A64WLAN_KEY_CCMP && keylen == 16)
		cipher = WLAN_CIPHER_SUITE_CCMP;
	else
		cipher = 0;
	if (!cipher)
		return -EINVAL;
	if (!group && !a64_ap_sta)
		return -ENODEV;
	if (a64_key_used[cam])
		a64wlan_clear_key(cam);
	k = &a64_keys[cam].k;
	memset(&a64_keys[cam], 0, sizeof(a64_keys[cam]));
	k->cipher = cipher;
	k->keyidx = (s8)keyid;
	k->keylen = (u8)keylen;
	k->flags = group ? 0 : IEEE80211_KEY_FLAG_PAIRWISE;
	memcpy(k->key, key, keylen);
	r = rtw_ops.set_key(a64_rtwdev->hw, SET_KEY, a64_vif, group ? (struct ieee80211_sta *)0 : a64_ap_sta, k);
	if (!r)
		a64_key_used[cam] = 1;
	shim_log("[wlan] key %u: cipher 0x%08x id %u %s -> CAM %u r=%d\n", cam, cipher, keyid, group ? "group" : "pairwise",
		 (unsigned)k->hw_key_idx, r);
	return r;
}
int a64wlan_clear_key(unsigned cam)
{
	if (cam >= 32 || !a64_key_used[cam])
		return 0;
	a64_key_used[cam] = 0;
	if (a64_powered())
		rtw_sec_clear_cam(a64_rtwdev, &a64_rtwdev->sec, a64_keys[cam].k.hw_key_idx);
	return 0;
}
static void a64_clear_keys_hw(void)
{
	unsigned i;
	for (i = 0; i < 32; i++)
		a64wlan_clear_key(i);
}
void a64wlan_clear_keys(void) { a64_clear_keys_hw(); }

/* the key a DATA frame to the AP is encrypted with: the pairwise one, else (WEP, static keys) the first group key of
 * the matching cipher class */
static struct ieee80211_key_conf *a64_tx_key(unsigned sec)
{
	struct ieee80211_key_conf *grp = (struct ieee80211_key_conf *)0;
	unsigned i;
	for (i = 0; i < 32; i++) {
		struct ieee80211_key_conf *k = &a64_keys[i].k;
		const int aes = k->cipher == WLAN_CIPHER_SUITE_CCMP;
		if (!a64_key_used[i] || aes != (sec == A64WLAN_SEC_AES))
			continue;
		if (k->flags & IEEE80211_KEY_FLAG_PAIRWISE)
			return k;
		if (!grp)
			grp = k;
	}
	return grp;
}

/* shim_mac80211.c: the cipher (A64WLAN_KEYALGO_*) of a protected frame the chip decrypted -- the key it matched:
 * the group key of the frame's key id for a group-addressed frame, else the pairwise one */
unsigned a64wlan_rx_keyalgo(const u8 *f, unsigned len)
{
	unsigned hdr = 24, i, keyid;
	const int mcast = f[4] & 1;
	if ((f[1] & 3) == 3)
		hdr += 6;
	if ((f[0] & 0x80))		/* QoS data */
		hdr += 2;
	if (len < hdr + 4)
		return 0;
	keyid = f[hdr + 3] >> 6;
	for (i = 0; i < 32; i++) {
		struct ieee80211_key_conf *k = &a64_keys[i].k;
		if (!a64_key_used[i])
			continue;
		if (mcast ? (!(k->flags & IEEE80211_KEY_FLAG_PAIRWISE) && (unsigned)k->keyidx == keyid)
			  : (k->flags & IEEE80211_KEY_FLAG_PAIRWISE) || k->cipher == WLAN_CIPHER_SUITE_WEP40 ||
			    k->cipher == WLAN_CIPHER_SUITE_WEP104)
			switch (k->cipher) {
			case WLAN_CIPHER_SUITE_WEP40: return A64WLAN_KEYALGO_WEP40;
			case WLAN_CIPHER_SUITE_WEP104: return A64WLAN_KEYALGO_WEP104;
			case WLAN_CIPHER_SUITE_TKIP: return A64WLAN_KEYALGO_TKIP;
			case WLAN_CIPHER_SUITE_CCMP: return A64WLAN_KEYALGO_AES;
			}
	}
	return 0;
}
