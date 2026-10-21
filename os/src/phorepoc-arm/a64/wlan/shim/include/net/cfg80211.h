/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

/* shim net/cfg80211.h -- OURS: the configuration-layer types rtw88 uses (bands, channels, chandef, bit rates, SSIDs,
 * scan requests, the wiphy and its flags). Linux names, our own freestanding definitions. See shim_core.h. */
#ifndef SHIM_NET_CFG80211_H
#define SHIM_NET_CFG80211_H
#include "../linux/ieee80211.h"

enum nl80211_band { NL80211_BAND_2GHZ, NL80211_BAND_5GHZ, NL80211_BAND_60GHZ, NL80211_BAND_6GHZ, NL80211_BAND_S1GHZ, NL80211_BAND_LC, NUM_NL80211_BANDS };
enum nl80211_iftype { NL80211_IFTYPE_UNSPECIFIED, NL80211_IFTYPE_ADHOC, NL80211_IFTYPE_STATION, NL80211_IFTYPE_AP, NL80211_IFTYPE_AP_VLAN, NL80211_IFTYPE_WDS,
	NL80211_IFTYPE_MONITOR, NL80211_IFTYPE_MESH_POINT, NL80211_IFTYPE_P2P_CLIENT, NL80211_IFTYPE_P2P_GO, NL80211_IFTYPE_P2P_DEVICE, NL80211_IFTYPE_OCB, NL80211_IFTYPE_NAN, NUM_NL80211_IFTYPES };
enum nl80211_chan_width { NL80211_CHAN_WIDTH_20_NOHT, NL80211_CHAN_WIDTH_20, NL80211_CHAN_WIDTH_40, NL80211_CHAN_WIDTH_80, NL80211_CHAN_WIDTH_80P80, NL80211_CHAN_WIDTH_160,
	NL80211_CHAN_WIDTH_5, NL80211_CHAN_WIDTH_10, NL80211_CHAN_WIDTH_1, NL80211_CHAN_WIDTH_2, NL80211_CHAN_WIDTH_4, NL80211_CHAN_WIDTH_8, NL80211_CHAN_WIDTH_16, NL80211_CHAN_WIDTH_320 };
enum nl80211_dfs_regions { NL80211_DFS_UNSET = 0, NL80211_DFS_FCC = 1, NL80211_DFS_ETSI = 2, NL80211_DFS_JP = 3 };
enum nl80211_cqm_rssi_threshold_event { NL80211_CQM_RSSI_THRESHOLD_EVENT_LOW, NL80211_CQM_RSSI_THRESHOLD_EVENT_HIGH, NL80211_CQM_RSSI_BEACON_LOSS_EVENT };
enum nl80211_feature_flags { NL80211_FEATURE_SK_TX_STATUS = 1 << 0, NL80211_FEATURE_SCAN_RANDOM_MAC_ADDR = 1 << 29, NL80211_FEATURE_SCHED_SCAN_RANDOM_MAC_ADDR = 1 << 30 };
enum nl80211_ext_feature_index { NL80211_EXT_FEATURE_VHT_IBSS, NL80211_EXT_FEATURE_RRM, NL80211_EXT_FEATURE_SET_SCAN_DWELL, NL80211_EXT_FEATURE_SCAN_RANDOM_SN,
	NL80211_EXT_FEATURE_CAN_REPLACE_PTK0, NL80211_EXT_FEATURE_BEACON_PROTECTION, NL80211_EXT_FEATURE_SCAN_MIN_PREQ_CONTENT, NUM_NL80211_EXT_FEATURES };
enum nl80211_scan_flags { NL80211_SCAN_FLAG_LOW_PRIORITY = 1 << 0, NL80211_SCAN_FLAG_FLUSH = 1 << 1, NL80211_SCAN_FLAG_AP = 1 << 2, NL80211_SCAN_FLAG_RANDOM_ADDR = 1 << 3,
	NL80211_SCAN_FLAG_RANDOM_SN = 1 << 10, NL80211_SCAN_FLAG_MIN_PREQ_CONTENT = 1 << 11 };
enum ieee80211_channel_flags { IEEE80211_CHAN_DISABLED = 1 << 0, IEEE80211_CHAN_NO_IR = 1 << 1, IEEE80211_CHAN_PSD = 1 << 2, IEEE80211_CHAN_RADAR = 1 << 3,
	IEEE80211_CHAN_NO_HT40PLUS = 1 << 4, IEEE80211_CHAN_NO_HT40MINUS = 1 << 5, IEEE80211_CHAN_NO_OFDM = 1 << 6, IEEE80211_CHAN_NO_80MHZ = 1 << 7, IEEE80211_CHAN_NO_160MHZ = 1 << 8 };
#define IEEE80211_CHAN_NO_HT40 (IEEE80211_CHAN_NO_HT40PLUS | IEEE80211_CHAN_NO_HT40MINUS)
enum ieee80211_rate_flags { IEEE80211_RATE_SHORT_PREAMBLE = 1 << 0, IEEE80211_RATE_MANDATORY_A = 1 << 1, IEEE80211_RATE_MANDATORY_B = 1 << 2, IEEE80211_RATE_MANDATORY_G = 1 << 3,
	IEEE80211_RATE_ERP_G = 1 << 4, IEEE80211_RATE_SUPPORTS_5MHZ = 1 << 5, IEEE80211_RATE_SUPPORTS_10MHZ = 1 << 6 };
enum rate_info_flags { RATE_INFO_FLAGS_MCS = BIT(0), RATE_INFO_FLAGS_VHT_MCS = BIT(1), RATE_INFO_FLAGS_SHORT_GI = BIT(2), RATE_INFO_FLAGS_DMG = BIT(3), RATE_INFO_FLAGS_HE_MCS = BIT(4) };
enum rate_info_bw { RATE_INFO_BW_20 = 0, RATE_INFO_BW_5, RATE_INFO_BW_10, RATE_INFO_BW_40, RATE_INFO_BW_80, RATE_INFO_BW_160, RATE_INFO_BW_HE_RU, RATE_INFO_BW_320 };
enum wiphy_flags { WIPHY_FLAG_SUPPORTS_MLO = BIT(0), WIPHY_FLAG_NETNS_OK = BIT(3), WIPHY_FLAG_PS_ON_BY_DEFAULT = BIT(4), WIPHY_FLAG_4ADDR_AP = BIT(5), WIPHY_FLAG_4ADDR_STATION = BIT(6),
	WIPHY_FLAG_CONTROL_PORT_PROTOCOL = BIT(7), WIPHY_FLAG_IBSS_RSN = BIT(8), WIPHY_FLAG_MESH_AUTH = BIT(10), WIPHY_FLAG_SUPPORTS_FW_ROAM = BIT(13), WIPHY_FLAG_AP_UAPSD = BIT(14),
	WIPHY_FLAG_SUPPORTS_TDLS = BIT(15), WIPHY_FLAG_TDLS_EXTERNAL_SETUP = BIT(16), WIPHY_FLAG_HAVE_AP_SME = BIT(17), WIPHY_FLAG_REPORTS_OBSS = BIT(18),
	WIPHY_FLAG_AP_PROBE_RESP_OFFLOAD = BIT(19), WIPHY_FLAG_OFFCHAN_TX = BIT(20), WIPHY_FLAG_HAS_REMAIN_ON_CHANNEL = BIT(21), WIPHY_FLAG_SUPPORTS_5_10_MHZ = BIT(22),
	WIPHY_FLAG_HAS_CHANNEL_SWITCH = BIT(23), WIPHY_FLAG_SUPPORTS_EXT_KEK_KCK = BIT(25) };
enum regulatory_flags { REGULATORY_CUSTOM_REG = BIT(0), REGULATORY_STRICT_REG = BIT(1), REGULATORY_DISABLE_BEACON_HINTS = BIT(2), REGULATORY_COUNTRY_IE_FOLLOW_POWER = BIT(3),
	REGULATORY_COUNTRY_IE_IGNORE = BIT(4), REGULATORY_ENABLE_RELAX_NO_IR = BIT(5), REGULATORY_WIPHY_SELF_MANAGED = BIT(7) };
enum nl80211_reg_initiator { NL80211_REGDOM_SET_BY_CORE, NL80211_REGDOM_SET_BY_USER, NL80211_REGDOM_SET_BY_DRIVER, NL80211_REGDOM_SET_BY_COUNTRY_IE };
enum nl80211_dfs_state { NL80211_DFS_USABLE, NL80211_DFS_UNAVAILABLE, NL80211_DFS_AVAILABLE };
enum survey_info_flags { SURVEY_INFO_NOISE_DBM = BIT(0), SURVEY_INFO_IN_USE = BIT(1), SURVEY_INFO_TIME = BIT(2), SURVEY_INFO_TIME_BUSY = BIT(3), SURVEY_INFO_TIME_EXT_BUSY = BIT(4),
	SURVEY_INFO_TIME_RX = BIT(5), SURVEY_INFO_TIME_TX = BIT(6), SURVEY_INFO_TIME_SCAN = BIT(7), SURVEY_INFO_TIME_BSS_RX = BIT(8) };
enum ieee80211_edmg_bw_config { IEEE80211_EDMG_BW_CONFIG_4 = 4 };

struct ieee80211_channel { enum nl80211_band band; u32 center_freq; u16 freq_offset; u16 hw_value; u32 flags; int max_antenna_gain; int max_power; int max_reg_power;
	bool beacon_found; u32 orig_flags; int orig_mag, orig_mpwr; enum nl80211_dfs_state dfs_state; unsigned long dfs_state_entered; unsigned int dfs_cac_ms; s8 psd; };
struct ieee80211_rate { u32 flags; u16 bitrate; u16 hw_value, hw_value_short; };
struct ieee80211_sta_ht_cap { u16 cap; bool ht_supported; u8 ampdu_factor; u8 ampdu_density; struct ieee80211_mcs_info mcs; };
struct ieee80211_sta_vht_cap { bool vht_supported; u32 cap; struct ieee80211_vht_mcs_info vht_mcs; };
struct ieee80211_supported_band { struct ieee80211_channel *channels; struct ieee80211_rate *bitrates; enum nl80211_band band; int n_channels; int n_bitrates;
	struct ieee80211_sta_ht_cap ht_cap; struct ieee80211_sta_vht_cap vht_cap; };
struct ieee80211_edmg { u8 channels; enum ieee80211_edmg_bw_config bw_config; };
struct cfg80211_chan_def { struct ieee80211_channel *chan; enum nl80211_chan_width width; u32 center_freq1; u32 center_freq2; struct ieee80211_edmg edmg; u16 freq1_offset; u16 punctured; };
struct cfg80211_ssid { u8 ssid[IEEE80211_MAX_SSID_LEN]; u8 ssid_len; };
struct cfg80211_match_set { struct cfg80211_ssid ssid; u8 bssid[ETH_ALEN]; s32 rssi_thold; };
struct cfg80211_sched_scan_plan { u32 interval; u32 iterations; };
struct cfg80211_scan_info { u64 scan_start_tsf; u8 tsf_bssid[ETH_ALEN] __aligned(2); bool aborted; };
struct cfg80211_scan_request { struct cfg80211_ssid *ssids; int n_ssids; u32 n_channels; const u8 *ie; size_t ie_len; u16 duration; bool duration_mandatory; u32 flags;
	u32 rates[NUM_NL80211_BANDS]; void *wdev; u8 mac_addr[ETH_ALEN] __aligned(2); u8 mac_addr_mask[ETH_ALEN] __aligned(2); u8 bssid[ETH_ALEN] __aligned(2);
	struct wiphy *wiphy; unsigned long scan_start; struct cfg80211_scan_info info; bool notified; bool no_cck; bool scan_6ghz; u32 n_6ghz_params; void *scan_6ghz_params; s8 tsf_report_link_id;
	struct ieee80211_channel *channels[]; };
struct cfg80211_sched_scan_request { u64 reqid; struct cfg80211_ssid *ssids; int n_ssids; u32 n_channels; const u8 *ie; size_t ie_len; u32 flags; struct cfg80211_match_set *match_sets; int n_match_sets;
	s32 min_rssi_thold; u32 delay; struct cfg80211_sched_scan_plan *scan_plans; int n_scan_plans; u8 mac_addr[ETH_ALEN] __aligned(2); u8 mac_addr_mask[ETH_ALEN] __aligned(2);
	struct ieee80211_channel *channels[]; };
struct cfg80211_bitrate_mask { struct { u32 legacy; u8 ht_mcs[IEEE80211_HT_MCS_MASK_LEN]; u16 vht_mcs[8]; u16 he_mcs[8]; enum nl80211_txrate_gi { NL80211_TXRATE_DEFAULT_GI, NL80211_TXRATE_FORCE_SGI, NL80211_TXRATE_FORCE_LGI } gi; u8 he_gi; u8 he_ltf; } control[NUM_NL80211_BANDS]; };
struct cfg80211_wowlan_nd_match;
struct cfg80211_wowlan_nd_info { int n_matches; struct cfg80211_wowlan_nd_match *matches[]; };
struct cfg80211_wowlan_wakeup { bool disconnect, magic_pkt, gtk_rekey_failure, eap_identity_req, four_way_handshake, rfkill_release, packet_80211, tcp_match, tcp_connlost, tcp_nomoretokens;
	s32 pattern_idx; u32 packet_present_len, packet_len; const void *packet; struct cfg80211_wowlan_nd_info *net_detect; };
struct cfg80211_pkt_pattern { const u8 *mask, *pattern; int pattern_len; int pkt_offset; };
struct cfg80211_wowlan { bool any, disconnect, magic_pkt, gtk_rekey_failure, eap_identity_req, four_way_handshake, rfkill_release; struct cfg80211_pkt_pattern *patterns; void *tcp;
	int n_patterns; struct cfg80211_sched_scan_request *nd_config; };
struct wiphy_wowlan_support { u32 flags; int n_patterns; int pattern_max_len; int pattern_min_len; int max_pkt_offset; int max_nd_match_sets; void *tcp; };
#define WIPHY_WOWLAN_ANY BIT(0)
#define WIPHY_WOWLAN_MAGIC_PKT BIT(1)
#define WIPHY_WOWLAN_DISCONNECT BIT(2)
#define WIPHY_WOWLAN_SUPPORTS_GTK_REKEY BIT(3)
#define WIPHY_WOWLAN_GTK_REKEY_FAILURE BIT(4)
#define WIPHY_WOWLAN_EAP_IDENTITY_REQ BIT(5)
#define WIPHY_WOWLAN_4WAY_HANDSHAKE BIT(6)
#define WIPHY_WOWLAN_RFKILL_RELEASE BIT(7)
#define WIPHY_WOWLAN_NET_DETECT BIT(8)
struct rate_info { u8 flags; u16 legacy; u8 mcs; u8 nss; u8 bw; u8 he_gi; u8 he_dcm; u8 he_ru_alloc; u8 n_bonded_ch; u8 eht_gi; u8 eht_ru_alloc; };
enum nl80211_sar_type { NL80211_SAR_TYPE_POWER, NUM_NL80211_SAR_TYPE };
struct cfg80211_sar_freq_ranges { u32 start_freq; u32 end_freq; };
struct cfg80211_sar_capa { enum nl80211_sar_type type; u32 num_freq_ranges; const struct cfg80211_sar_freq_ranges *freq_ranges; };
struct cfg80211_sar_sub_specs { s32 power; u32 freq_range_index; };
struct cfg80211_sar_specs { enum nl80211_sar_type type; u32 num_sub_specs; struct cfg80211_sar_sub_specs sub_specs[]; };
struct ieee80211_iface_limit { u16 max; u16 types; };
struct ieee80211_iface_combination { const struct ieee80211_iface_limit *limits; u32 num_different_channels; u16 max_interfaces; u8 n_limits; bool beacon_int_infra_match;
	u8 radar_detect_widths; u8 radar_detect_regions; u32 beacon_int_min_gcd; };
struct ieee80211_regdomain;
struct ieee80211_reg_rule;
struct regulatory_request { int wiphy_idx; enum nl80211_reg_initiator initiator; char alpha2[3]; enum nl80211_dfs_regions dfs_region; bool intersect; bool processed; };
struct wiphy {
	u8 perm_addr[ETH_ALEN]; u8 addr_mask[ETH_ALEN]; u16 interface_modes; u32 flags, regulatory_flags, features; u8 ext_features[DIV_ROUND_UP(NUM_NL80211_EXT_FEATURES, 8)];
	u32 ap_sme_capa; u32 max_scan_ssids, max_sched_scan_reqs, max_sched_scan_ssids, max_match_sets, max_scan_ie_len, max_sched_scan_ie_len, max_sched_scan_plans, max_sched_scan_plan_interval, max_sched_scan_plan_iterations;
	int n_cipher_suites; const u32 *cipher_suites; u8 retry_short, retry_long; u32 frag_threshold, rts_threshold; u8 coverage_class; char fw_version[32]; u32 hw_version;
	const struct wiphy_wowlan_support *wowlan; const struct cfg80211_sar_capa *sar_capa; struct cfg80211_wowlan *wowlan_config; u16 max_remain_on_channel_duration; u8 max_num_pmkids; u32 available_antennas_tx, available_antennas_rx;
	u32 probe_resp_offload; const u8 *extended_capabilities, *extended_capabilities_mask; u8 extended_capabilities_len; const struct ieee80211_iface_combination *iface_combinations; int n_iface_combinations;
	const struct ieee80211_regdomain *regd; struct ieee80211_supported_band *bands[NUM_NL80211_BANDS]; void (*reg_notifier)(struct wiphy *wiphy, struct regulatory_request *request);
	struct device dev; u16 max_ap_assoc_sta; u8 max_num_csa_counters; u32 bss_select_support; u8 nan_supported_bands; u32 txq_limit, txq_memory_limit, txq_quantum; u8 support_mbssid, support_only_he_mbssid;
	struct mutex mtx; char priv[] __aligned(8);
};
static inline void *wiphy_priv(struct wiphy *w) { return w->priv; }
static inline struct device *wiphy_dev(struct wiphy *w) { return &w->dev; }
static inline void wiphy_ext_feature_set(struct wiphy *w, enum nl80211_ext_feature_index f) { w->ext_features[f / 8] |= (u8)BIT(f % 8); }
static inline bool wiphy_ext_feature_isset(struct wiphy *w, enum nl80211_ext_feature_index f) { return (w->ext_features[f / 8] & BIT(f % 8)) != 0; }
#define wiphy_lock(w) mutex_lock(&(w)->mtx)
#define wiphy_unlock(w) mutex_unlock(&(w)->mtx)
#define wiphy_dbg(w, fmt, ...) ((void)0)
#define wiphy_info(w, fmt, ...) shim_log("rtw88: " fmt, ##__VA_ARGS__)
#define wiphy_err(w, fmt, ...) shim_log("rtw88: " fmt, ##__VA_ARGS__)
#define wiphy_warn(w, fmt, ...) shim_log("rtw88: " fmt, ##__VA_ARGS__)
enum nl80211_sta_info { NL80211_STA_INFO_TX_BITRATE = 8 };
struct station_info { u64 filled; struct rate_info txrate; };
struct survey_info { struct ieee80211_channel *channel; u64 time, time_busy, time_ext_busy, time_rx, time_tx, time_scan, time_bss_rx; u32 filled; s8 noise; };
struct wireless_dev { struct wiphy *wiphy; enum nl80211_iftype iftype; void *netdev; };

/* the helpers rtw88 calls, implemented in shim_mac80211.c */
int ieee80211_channel_to_frequency(int chan, enum nl80211_band band);
int ieee80211_frequency_to_channel(int freq);
struct ieee80211_channel *ieee80211_get_channel(struct wiphy *wiphy, int freq);
int cfg80211_get_ies_channel_number(const u8 *ie, size_t ielen, enum nl80211_band band);
u32 cfg80211_calculate_bitrate(struct rate_info *rate);
const struct element *cfg80211_find_elem(u8 eid, const u8 *ies, int len);
static inline const u8 *cfg80211_find_ie(u8 eid, const u8 *ies, int len) { return (const u8 *)cfg80211_find_elem(eid, ies, len); }
const u8 *cfg80211_find_vendor_ie(unsigned int oui, int oui_type, const u8 *ies, unsigned int len);
static inline bool cfg80211_ssid_eq(struct cfg80211_ssid *a, struct cfg80211_ssid *b) { return a->ssid_len == b->ssid_len && shim_memcmp(a->ssid, b->ssid, a->ssid_len) == 0; }
static inline int cfg80211_chandef_dfs_required(struct wiphy *w, const struct cfg80211_chan_def *c, enum nl80211_iftype t) { (void)w; (void)c; (void)t; return 0; }
static inline bool cfg80211_chandef_valid(const struct cfg80211_chan_def *c) { return c->chan != 0; }
static inline void cfg80211_chandef_create(struct cfg80211_chan_def *c, struct ieee80211_channel *ch, int type) { c->chan = ch; c->width = NL80211_CHAN_WIDTH_20_NOHT; c->center_freq1 = ch->center_freq; c->center_freq2 = 0; (void)type; }
#endif
