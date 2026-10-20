/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* shim_mac80211.c -- OURS: "mac80211-lite", the stack side of the rtw88 driver interface (shim/include/net/mac80211.h).
 * The real mac80211 owns the interfaces, stations, keys, scanning and the frame queues; here, in the chip-core stage,
 * there is one hw, no vif and no sta: the iterators visit nothing, the station lookups find nothing, the queues are
 * never stopped, and every received frame goes to the beacon table (the "beacons listed" demo) -- the W3 rung replaces
 * that sink with the WHA front under the real wlanldd. */
#include <net/mac80211.h>
#include "a64wlan_api.h"

/* ---- the one hw ---- */
struct ieee80211_hw *ieee80211_alloc_hw(size_t priv_data_len, const struct ieee80211_ops *ops)
{
	struct ieee80211_hw *hw;
	struct wiphy *wiphy;
	(void)ops;
	wiphy = (struct wiphy *)shim_zalloc(sizeof(*wiphy) + sizeof(*hw) + 8);
	if (!wiphy)
		return (struct ieee80211_hw *)0;
	hw = (struct ieee80211_hw *)(((unsigned long)wiphy->priv + 7) & ~7ul);
	hw->wiphy = wiphy;
	hw->priv = shim_zalloc(priv_data_len);
	if (!hw->priv) {
		shim_free(wiphy);
		return (struct ieee80211_hw *)0;
	}
	mutex_init(&wiphy->mtx);
	return hw;
}
void ieee80211_free_hw(struct ieee80211_hw *hw) { shim_free(hw->priv); shim_free(hw->wiphy); }
int ieee80211_register_hw(struct ieee80211_hw *hw) { (void)hw; return 0; }
void ieee80211_unregister_hw(struct ieee80211_hw *hw) { (void)hw; }
void ieee80211_restart_hw(struct ieee80211_hw *hw) { (void)hw; shim_log("rtw88: mac80211-lite: restart_hw requested (ignored)\n"); }

/* ---- queues, work ---- */
void ieee80211_stop_queues(struct ieee80211_hw *hw) { (void)hw; }
void ieee80211_wake_queues(struct ieee80211_hw *hw) { (void)hw; }
void ieee80211_stop_queue(struct ieee80211_hw *hw, int queue) { (void)hw; (void)queue; }
void ieee80211_wake_queue(struct ieee80211_hw *hw, int queue) { (void)hw; (void)queue; }
void ieee80211_queue_work(struct ieee80211_hw *hw, struct work_struct *work) { (void)hw; shim_queue_work(work); }
void ieee80211_queue_delayed_work(struct ieee80211_hw *hw, struct delayed_work *dwork, unsigned long delay) { (void)hw; shim_queue_delayed_work(dwork, delay); }

/* ---- tx status: nothing owns the frames yet ---- */
/* W3b: the WHA PDD's transmit completion -- a64wlan_tx() registers each skb with its cookie, the driver's status
 * report (rtw_sdio_indicate_tx_status: "ACK" once the packet is written to the chip, the real report only with
 * IEEE80211_TX_CTL_REQ_TX_STATUS) hands the cookie back through the sink */
struct shim_tx_track { struct sk_buff *skb; void *cookie; };
static struct shim_tx_track shim_tx_table[32];
static a64wlan_tx_sink_fn shim_tx_sink;
static void *shim_tx_sink_ctx;
static unsigned shim_tx_done, shim_tx_untracked;
void a64wlan_set_tx_sink(a64wlan_tx_sink_fn fn, void *ctx) { shim_tx_sink_ctx = ctx; shim_tx_sink = fn; }
unsigned a64wlan_tx_done_count(void) { return shim_tx_done; }
int shim_mac80211_tx_track(struct sk_buff *skb, void *cookie)
{
	unsigned i;
	for (i = 0; i < 32; ++i)
		if (!shim_tx_table[i].skb) { shim_tx_table[i].skb = skb; shim_tx_table[i].cookie = cookie; return 0; }
	/* 32 frames the driver took but never reported: an skb freed behind our back (a tx report purge) leaves its
	 * slot stale -- say so, the sender sees -ENOSPC */
	shim_log("[wlan] tx track table full (%u done, %u untracked)\n", shim_tx_done, shim_tx_untracked);
	return -ENOSPC;
}
void ieee80211_tx_status_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	unsigned i;
	(void)hw;
	++shim_tx_done;
	for (i = 0; i < 32; ++i) {
		if (shim_tx_table[i].skb == skb) {
			void *cookie = shim_tx_table[i].cookie;
			shim_tx_table[i].skb = (struct sk_buff *)0;
			if (shim_tx_sink)
				shim_tx_sink(shim_tx_sink_ctx, cookie, (info->flags & (IEEE80211_TX_STAT_ACK | IEEE80211_TX_STAT_NOACK_TRANSMITTED)) ? 1 : 0);
			shim_free_skb(skb);
			return;
		}
	}
	++shim_tx_untracked;
	shim_free_skb(skb);
}
/* a frame the driver drops (rtw_ops_tx while not running, a TX error) is a failed transmission for its sender */
void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	unsigned i;
	(void)hw;
	for (i = 0; i < 32; ++i) {
		if (shim_tx_table[i].skb == skb) {
			void *cookie = shim_tx_table[i].cookie;
			shim_tx_table[i].skb = (struct sk_buff *)0;
			if (shim_tx_sink)
				shim_tx_sink(shim_tx_sink_ctx, cookie, 0);
			break;
		}
	}
	shim_free_skb(skb);
}
void ieee80211_purge_tx_queue(struct ieee80211_hw *hw, struct sk_buff_head *skbs) { (void)hw; skb_queue_purge(skbs); }
struct sk_buff *ieee80211_tx_dequeue(struct ieee80211_hw *hw, struct ieee80211_txq *txq) { (void)hw; (void)txq; return (struct sk_buff *)0; }
void ieee80211_txq_get_depth(struct ieee80211_txq *txq, unsigned long *frame_cnt, unsigned long *byte_cnt) { (void)txq; if (frame_cnt) *frame_cnt = 0; if (byte_cnt) *byte_cnt = 0; }

/* ---- the one interface and the one station (the AP), registered by the glue as mac80211's lists hold them;
 * keys: none yet (W5) ---- */
static struct ieee80211_vif *shim_vif;
static struct ieee80211_sta *shim_sta;
void shim_mac80211_set_vif(struct ieee80211_vif *vif) { shim_vif = vif; }
void shim_mac80211_set_sta(struct ieee80211_sta *sta) { shim_sta = sta; }
void ieee80211_iterate_active_interfaces_atomic(struct ieee80211_hw *hw, u32 iter_flags, void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif), void *data) { (void)hw; (void)iter_flags; if (shim_vif) iterator(data, shim_vif->addr, shim_vif); }
void ieee80211_iterate_active_interfaces(struct ieee80211_hw *hw, u32 iter_flags, void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif), void *data) { (void)hw; (void)iter_flags; if (shim_vif) iterator(data, shim_vif->addr, shim_vif); }
void ieee80211_iterate_stations_atomic(struct ieee80211_hw *hw, void (*iterator)(void *data, struct ieee80211_sta *sta), void *data) { (void)hw; if (shim_sta) iterator(data, shim_sta); }
void ieee80211_iterate_stations(struct ieee80211_hw *hw, void (*iterator)(void *data, struct ieee80211_sta *sta), void *data) { (void)hw; if (shim_sta) iterator(data, shim_sta); }
void ieee80211_iter_keys(struct ieee80211_hw *hw, struct ieee80211_vif *vif, void (*iter)(struct ieee80211_hw *, struct ieee80211_vif *, struct ieee80211_sta *, struct ieee80211_key_conf *, void *), void *iter_data) { (void)hw; (void)vif; (void)iter; (void)iter_data; }
void ieee80211_iter_keys_rcu(struct ieee80211_hw *hw, struct ieee80211_vif *vif, void (*iter)(struct ieee80211_hw *, struct ieee80211_vif *, struct ieee80211_sta *, struct ieee80211_key_conf *, void *), void *iter_data) { (void)hw; (void)vif; (void)iter; (void)iter_data; }
struct ieee80211_sta *ieee80211_find_sta(struct ieee80211_vif *vif, const u8 *addr)
{
	if (shim_sta && vif == shim_vif && addr && ether_addr_equal(shim_sta->addr, addr))
		return shim_sta;
	return (struct ieee80211_sta *)0;
}
struct ieee80211_sta *ieee80211_find_sta_by_ifaddr(struct ieee80211_hw *hw, const u8 *addr, const u8 *localaddr)
{
	(void)hw;
	if (!shim_sta || !shim_vif || !addr || !ether_addr_equal(shim_sta->addr, addr))
		return (struct ieee80211_sta *)0;
	if (localaddr && !ether_addr_equal(shim_vif->addr, localaddr))
		return (struct ieee80211_sta *)0;
	return shim_sta;
}
/* chanctx: rtw88 uses mac80211's emulation (= config(CHANGE_CHANNEL)); the glue drives config itself */
int ieee80211_emulate_add_chanctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx) { (void)hw; (void)ctx; return 0; }
void ieee80211_emulate_remove_chanctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx) { (void)hw; (void)ctx; }
void ieee80211_emulate_change_chanctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx, u32 changed) { (void)hw; (void)ctx; (void)changed; }
int ieee80211_emulate_switch_vif_chanctx(struct ieee80211_hw *hw, struct ieee80211_vif_chanctx_switch *vifs, int n_vifs, enum ieee80211_chanctx_switch_mode mode) { (void)hw; (void)vifs; (void)n_vifs; (void)mode; return 0; }
void ieee80211_stop_tx_ba_cb_irqsafe(struct ieee80211_vif *vif, const u8 *ra, u16 tid) { (void)vif; (void)ra; (void)tid; }
void ieee80211_connection_loss(struct ieee80211_vif *vif) { (void)vif; }
void ieee80211_beacon_loss(struct ieee80211_vif *vif) { (void)vif; }
void ieee80211_cqm_rssi_notify(struct ieee80211_vif *vif, enum nl80211_cqm_rssi_threshold_event rssi_event, s32 rssi_level, gfp_t gfp) { (void)vif; (void)rssi_event; (void)rssi_level; (void)gfp; }
void ieee80211_request_smps(struct ieee80211_vif *vif, unsigned int link_id, enum ieee80211_smps_mode smps_mode) { (void)vif; (void)link_id; (void)smps_mode; }
int ieee80211_start_tx_ba_session(struct ieee80211_sta *sta, u16 tid, u16 timeout) { (void)sta; (void)tid; (void)timeout; return -EINVAL; }
int ieee80211_stop_tx_ba_session(struct ieee80211_sta *sta, u16 tid) { (void)sta; (void)tid; return -EINVAL; }
void ieee80211_scan_completed(struct ieee80211_hw *hw, struct cfg80211_scan_info *info) { (void)hw; (void)info; }
void ieee80211_sched_scan_results(struct ieee80211_hw *hw) { (void)hw; }
void ieee80211_sched_scan_stopped(struct ieee80211_hw *hw) { (void)hw; }
void ieee80211_report_wowlan_wakeup(struct ieee80211_vif *vif, struct cfg80211_wowlan_wakeup *wakeup, gfp_t gfp) { (void)vif; (void)wakeup; (void)gfp; }
void ieee80211_get_key_rx_seq(struct ieee80211_key_conf *keyconf, int tid, struct ieee80211_key_seq *seq) { (void)keyconf; (void)tid; memset(seq, 0, sizeof(*seq)); }

/* ---- frame templates: the station's own (mac80211 tx.c ieee80211_pspoll_get / ieee80211_nullfunc_get: the
 * reserved-page download at association wants them); no beacon (not an AP) ---- */
struct sk_buff *ieee80211_beacon_get_tim(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u16 *tim_offset, u16 *tim_length, unsigned int link_id) { (void)hw; (void)vif; (void)tim_offset; (void)tim_length; (void)link_id; return (struct sk_buff *)0; }
struct sk_buff *ieee80211_nullfunc_get(struct ieee80211_hw *hw, struct ieee80211_vif *vif, int link_id, bool qos_ok)
{
	struct ieee80211_hdr_3addr *nullfunc;
	struct sk_buff *skb;
	bool qos = false;
	(void)link_id;
	if (!vif || vif->type != NL80211_IFTYPE_STATION || !vif->bss_conf.bssid)
		return (struct sk_buff *)0;
	if (qos_ok && shim_sta && shim_sta->wme)
		qos = true;
	skb = shim_alloc_skb(hw->extra_tx_headroom + sizeof(*nullfunc) + 2);
	if (!skb)
		return (struct sk_buff *)0;
	skb_reserve(skb, hw->extra_tx_headroom);
	nullfunc = (struct ieee80211_hdr_3addr *)skb_put_zero(skb, sizeof(*nullfunc));
	nullfunc->frame_control = cpu_to_le16(IEEE80211_FTYPE_DATA | (qos ? IEEE80211_STYPE_QOS_NULLFUNC : IEEE80211_STYPE_NULLFUNC) | IEEE80211_FCTL_TODS);
	ether_addr_copy(nullfunc->addr1, vif->bss_conf.bssid);
	ether_addr_copy(nullfunc->addr2, vif->addr);
	ether_addr_copy(nullfunc->addr3, vif->bss_conf.bssid);
	if (qos) {
		u8 *qc = (u8 *)skb_put(skb, 2);
		qc[0] = 7; qc[1] = 0;		/* mac80211: qos_ctrl = 7 (TID 7, VO) */
	}
	return skb;
}
struct sk_buff *ieee80211_pspoll_get(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct ieee80211_pspoll *pspoll;
	struct sk_buff *skb;
	if (!vif || vif->type != NL80211_IFTYPE_STATION || !vif->bss_conf.bssid)
		return (struct sk_buff *)0;
	skb = shim_alloc_skb(hw->extra_tx_headroom + sizeof(*pspoll));
	if (!skb)
		return (struct sk_buff *)0;
	skb_reserve(skb, hw->extra_tx_headroom);
	pspoll = (struct ieee80211_pspoll *)skb_put_zero(skb, sizeof(*pspoll));
	pspoll->frame_control = cpu_to_le16(IEEE80211_FTYPE_CTL | IEEE80211_STYPE_PSPOLL);
	pspoll->aid = cpu_to_le16(vif->cfg.aid | 0xc000);	/* aid in the duration field, the two MSBs set */
	ether_addr_copy(pspoll->bssid, vif->bss_conf.bssid);
	ether_addr_copy(pspoll->ta, vif->addr);
	return skb;
}
struct sk_buff *ieee80211_proberesp_get(struct ieee80211_hw *hw, struct ieee80211_vif *vif) { (void)hw; (void)vif; return (struct sk_buff *)0; }
struct sk_buff *ieee80211_probereq_get(struct ieee80211_hw *hw, const u8 *src_addr, const u8 *ssid, size_t ssid_len, size_t tailroom)
{
	struct sk_buff *skb = shim_alloc_skb(hw->extra_tx_headroom + 24 + 2 + ssid_len + 16 + tailroom);
	struct ieee80211_hdr_3addr *hdr;
	u8 *p;
	if (!skb)
		return (struct sk_buff *)0;
	skb_reserve(skb, hw->extra_tx_headroom);
	hdr = (struct ieee80211_hdr_3addr *)skb_put_zero(skb, sizeof(*hdr));
	hdr->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_PROBE_REQ);
	eth_broadcast_addr(hdr->addr1);
	ether_addr_copy(hdr->addr2, src_addr);
	eth_broadcast_addr(hdr->addr3);
	p = (u8 *)skb_put(skb, 2 + ssid_len);
	p[0] = WLAN_EID_SSID; p[1] = (u8)ssid_len;
	if (ssid_len)
		memcpy(p + 2, ssid, ssid_len);
	return skb;
}

/* ---- cfg80211 helpers ---- */
int ieee80211_channel_to_frequency(int chan, enum nl80211_band band)
{
	if (band == NL80211_BAND_2GHZ) {
		if (chan == 14) return 2484;
		if (chan >= 1 && chan < 14) return 2407 + chan * 5;
		return 0;
	}
	if (band == NL80211_BAND_5GHZ) {
		if (chan >= 182 && chan <= 196) return 4000 + chan * 5;
		if (chan > 0) return 5000 + chan * 5;
	}
	return 0;
}
int ieee80211_frequency_to_channel(int freq)
{
	if (freq == 2484) return 14;
	if (freq < 2484) return (freq - 2407) / 5;
	if (freq >= 4910 && freq <= 4980) return (freq - 4000) / 5;
	if (freq < 5925) return (freq - 5000) / 5;
	return 0;
}
struct ieee80211_channel *ieee80211_get_channel(struct wiphy *wiphy, int freq)
{
	int b, i;
	for (b = 0; b < NUM_NL80211_BANDS; ++b) {
		struct ieee80211_supported_band *sb = wiphy->bands[b];
		if (!sb) continue;
		for (i = 0; i < sb->n_channels; ++i)
			if ((int)sb->channels[i].center_freq == freq)
				return &sb->channels[i];
	}
	return (struct ieee80211_channel *)0;
}
const struct element *cfg80211_find_elem(u8 eid, const u8 *ies, int len)
{
	const struct element *e;
	for_each_element_id(e, eid, ies, len)
		return e;
	return (const struct element *)0;
}
const u8 *cfg80211_find_vendor_ie(unsigned int oui, int oui_type, const u8 *ies, unsigned int len)
{
	const struct element *e;
	for_each_element_id(e, WLAN_EID_VENDOR_SPECIFIC, ies, (int)len) {
		if (e->datalen < 4) continue;
		if (((unsigned)e->data[0] << 16 | (unsigned)e->data[1] << 8 | e->data[2]) == oui && (oui_type < 0 || e->data[3] == oui_type))
			return (const u8 *)e;
	}
	return (const u8 *)0;
}
int cfg80211_get_ies_channel_number(const u8 *ie, size_t ielen, enum nl80211_band band)
{
	const struct element *e;
	if (band == NL80211_BAND_2GHZ || band == NL80211_BAND_5GHZ) {
		e = cfg80211_find_elem(WLAN_EID_DS_PARAMS, ie, (int)ielen);
		if (e && e->datalen == 1)
			return e->data[0];
		e = cfg80211_find_elem(WLAN_EID_HT_OPERATION, ie, (int)ielen);
		if (e && e->datalen >= sizeof(struct ieee80211_ht_operation))
			return ((const struct ieee80211_ht_operation *)e->data)->primary_chan;
	}
	return -1;
}
u32 cfg80211_calculate_bitrate(struct rate_info *rate)
{
	static const u16 ht20[8] = { 65, 130, 195, 260, 390, 520, 585, 650 };	/* MCS0-7 at 20 MHz, long GI, x 100 kbps */
	if (rate->flags & RATE_INFO_FLAGS_MCS) {
		u32 r = ht20[rate->mcs & 7] * ((rate->mcs >> 3) + 1);
		if (rate->bw == RATE_INFO_BW_40) r = r * 2080 / 1000;
		if (rate->flags & RATE_INFO_FLAGS_SHORT_GI) r = r * 10 / 9;
		return r;
	}
	return rate->legacy;
}

/* ---- the receive sink: the beacon table (W2's demo) ----
 * every management frame with an SSID + DS-params lands here; the first sighting of each BSSID is logged with its
 * SSID, channel and RSSI; the count is printed every 64 frames so a quiet log still shows life */
struct shim_bss { u8 bssid[6]; u8 chan; s8 rssi; u16 seen; u8 ssid_len; char ssid[33]; };
static struct shim_bss shim_bss_table[32];
static unsigned shim_bss_count, shim_rx_frames, shim_rx_beacons;
unsigned a64wlan_rx_frames(void) { return shim_rx_frames; }
unsigned shim_mac80211_bss_count(void) { return shim_bss_count; }
/* the WHA PDD's receive path (a64wlan_api.h): every frame, before the beacon table looks at it */
static a64wlan_rx_sink_fn shim_rx_sink;
static void *shim_rx_sink_ctx;
void a64wlan_set_rx_sink(a64wlan_rx_sink_fn fn, void *ctx) { shim_rx_sink_ctx = ctx; shim_rx_sink = fn; }

void ieee80211_rx_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	struct ieee80211_rx_status *rx = IEEE80211_SKB_RXCB(skb);
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
	const struct element *e;
	unsigned i, hdr_len, ielen;
	const u8 *ies;
	int chan = -1;
	(void)hw;
	shim_rx_frames++;
	if ((shim_rx_frames & 63) == 1)
		shim_log("[wlan] rx frames=%u beacons=%u bss=%u\n", shim_rx_frames, shim_rx_beacons, shim_bss_count);
	if (skb->len >= 10 && (rx->flag & RX_FLAG_FAILED_FCS_CRC)) {
		/* W4 (boot 252): the association response never shows -- is it arriving with a bad FCS/ICV? */
		static unsigned fcs_failed, fcs_failed_to_us;
		unsigned char mac[6];
		fcs_failed++;
		a64wlan_mac_addr(mac);
		if (memcmp(skb->data + 4, mac, 6) == 0 && ++fcs_failed_to_us <= 12)
			shim_log("[wlan] rx FCS/ICV-failed frame TO US: fc=%02x%02x len=%u from %pM (%u failed so far)\n",
				 skb->data[0], skb->data[1], skb->len, skb->data + 10, fcs_failed);
		goto out;
	}
	if (skb->len < 10)
		goto out;
	/* control frames (ACK = 10 bytes, RCR ACF + RXFLTMAP1) go to the sink only: the PDD counts / logs the ones
	 * addressed to us (W4 bring-up: is our unicast frame acknowledged by the AP at all?) */
	if (shim_rx_sink) {
		/* security (a64wlan_api.h): a protected data frame is DECRYPTED (rtw88 RX_FLAG_DECRYPTED, IV + trailer still in
		 * it: RCR APP_ICV / APP_MIC) with the cipher of the key it was matched against, or SWDEC */
		unsigned fl = rx->encoding == RX_ENC_HT ? A64WLAN_RX_HT : 0u;
		if (skb->len >= 24 && ((skb->data[0] >> 2) & 3) == 2 && (skb->data[1] & 0x40))
			fl |= (rx->flag & RX_FLAG_DECRYPTED) ? A64WLAN_RX_DECRYPTED | A64WLAN_RX_SECTYPE_SET(a64wlan_rx_keyalgo(skb->data, skb->len))
							     : A64WLAN_RX_SWDEC;
		shim_rx_sink(shim_rx_sink_ctx, skb->data, skb->len, rx->signal, rx->rate_idx, rx->freq, fl);
	}
	if (skb->len < 24)
		goto out;
	if (ieee80211_is_beacon(mgmt->frame_control)) { ies = mgmt->u.beacon.variable; hdr_len = offsetof(struct ieee80211_mgmt, u.beacon.variable); shim_rx_beacons++; }
	else if (ieee80211_is_probe_resp(mgmt->frame_control)) { ies = mgmt->u.probe_resp.variable; hdr_len = offsetof(struct ieee80211_mgmt, u.probe_resp.variable); }
	else goto out;
	if (skb->len <= hdr_len)
		goto out;
	ielen = skb->len - hdr_len;
	for (i = 0; i < shim_bss_count; ++i)
		if (ether_addr_equal(shim_bss_table[i].bssid, mgmt->bssid)) {
			shim_bss_table[i].seen++;
			shim_bss_table[i].rssi = rx->signal;
			goto out;
		}
	if (shim_bss_count < ARRAY_SIZE(shim_bss_table)) {
		struct shim_bss *b = &shim_bss_table[shim_bss_count++];
		memset(b, 0, sizeof(*b));
		ether_addr_copy(b->bssid, mgmt->bssid);
		b->rssi = rx->signal;
		b->seen = 1;
		chan = cfg80211_get_ies_channel_number(ies, ielen, NL80211_BAND_2GHZ);
		b->chan = chan > 0 ? (u8)chan : (u8)ieee80211_frequency_to_channel(rx->freq);
		e = cfg80211_find_elem(WLAN_EID_SSID, ies, (int)ielen);
		if (e) {
			b->ssid_len = e->datalen > 32 ? 32 : e->datalen;
			for (i = 0; i < b->ssid_len; ++i)
				b->ssid[i] = (e->data[i] >= 32 && e->data[i] < 127) ? (char)e->data[i] : '.';
		}
		shim_log("[wlan] AP %u: bssid=%pM ch=%u rssi=%d ssid=\"%s\"%s\n", shim_bss_count, b->bssid, b->chan, (int)b->rssi,
			 b->ssid, b->ssid_len ? "" : " (hidden)");
	}
out:
	shim_free_skb(skb);
}
void ieee80211_rx_napi(struct ieee80211_hw *hw, struct ieee80211_sta *sta, struct sk_buff *skb, void *napi) { (void)sta; (void)napi; ieee80211_rx_irqsafe(hw, skb); }
