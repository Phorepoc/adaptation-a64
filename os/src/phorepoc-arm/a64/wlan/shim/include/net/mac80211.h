/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* shim net/mac80211.h -- OURS: the mac80211 driver-facing interface as rtw88 uses it (the hw, vif, sta, key, txq
 * objects, the tx-info / rx-status control blocks, the flag enums and the callbacks into the stack). Linux names and
 * semantics, our freestanding definitions; the callbacks are implemented by our mac80211-lite (shim_mac80211.c), which
 * in the end is the bridge to the Symbian WHA front. See shim_core.h. */
#ifndef SHIM_NET_MAC80211_H
#define SHIM_NET_MAC80211_H
#include "cfg80211.h"

#define IEEE80211_MAX_QUEUES 16
#define IEEE80211_INVAL_HW_QUEUE 0xff
#define IEEE80211_LINK_UNSPECIFIED 0xff
#define IEEE80211_MLD_MAX_NUM_LINKS 15
#define IEEE80211_TX_MAX_RATES 4
#define IEEE80211_TX_RATE_TABLE_SIZE 4
#define IEEE80211_MAX_AMPDU_BUF_HE 256

enum ieee80211_hw_flags {
	IEEE80211_HW_HAS_RATE_CONTROL, IEEE80211_HW_RX_INCLUDES_FCS, IEEE80211_HW_HOST_BROADCAST_PS_BUFFERING, IEEE80211_HW_SIGNAL_UNSPEC, IEEE80211_HW_SIGNAL_DBM,
	IEEE80211_HW_NEED_DTIM_BEFORE_ASSOC, IEEE80211_HW_SPECTRUM_MGMT, IEEE80211_HW_AMPDU_AGGREGATION, IEEE80211_HW_SUPPORTS_PS, IEEE80211_HW_PS_NULLFUNC_STACK,
	IEEE80211_HW_SUPPORTS_DYNAMIC_PS, IEEE80211_HW_MFP_CAPABLE, IEEE80211_HW_WANT_MONITOR_VIF, IEEE80211_HW_NO_VIRTUAL_MONITOR, IEEE80211_HW_NO_AUTO_VIF, IEEE80211_HW_SW_CRYPTO_CONTROL,
	IEEE80211_HW_SUPPORT_FAST_XMIT, IEEE80211_HW_REPORTS_TX_ACK_STATUS, IEEE80211_HW_CONNECTION_MONITOR, IEEE80211_HW_QUEUE_CONTROL, IEEE80211_HW_SUPPORTS_PER_STA_GTK,
	IEEE80211_HW_AP_LINK_PS, IEEE80211_HW_TX_AMPDU_SETUP_IN_HW, IEEE80211_HW_SUPPORTS_RC_TABLE, IEEE80211_HW_P2P_DEV_ADDR_FOR_INTF, IEEE80211_HW_TIMING_BEACON_ONLY,
	IEEE80211_HW_SUPPORTS_HT_CCK_RATES, IEEE80211_HW_CHANCTX_STA_CSA, IEEE80211_HW_SUPPORTS_CLONED_SKBS, IEEE80211_HW_SINGLE_SCAN_ON_ALL_BANDS, IEEE80211_HW_TDLS_WIDER_BW,
	IEEE80211_HW_SUPPORTS_AMSDU_IN_AMPDU, IEEE80211_HW_BEACON_TX_STATUS, IEEE80211_HW_NEEDS_UNIQUE_STA_ADDR, IEEE80211_HW_SUPPORTS_REORDERING_BUFFER, IEEE80211_HW_USES_RSS,
	IEEE80211_HW_TX_AMSDU, IEEE80211_HW_TX_FRAG_LIST, IEEE80211_HW_REPORTS_LOW_ACK, IEEE80211_HW_SUPPORTS_TX_FRAG, IEEE80211_HW_SUPPORTS_TDLS_BUFFER_STA, IEEE80211_HW_DEAUTH_NEED_MGD_TX_PREP,
	IEEE80211_HW_DOESNT_SUPPORT_QOS_NDP, IEEE80211_HW_BUFF_MMPDU_TXQ, IEEE80211_HW_SUPPORTS_VHT_EXT_NSS_BW, IEEE80211_HW_STA_MMPDU_TXQ, IEEE80211_HW_TX_STATUS_NO_AMPDU_LEN,
	IEEE80211_HW_SUPPORTS_MULTI_BSSID, IEEE80211_HW_SUPPORTS_ONLY_HE_MULTI_BSSID, IEEE80211_HW_AMPDU_KEYBORDER_SUPPORT, IEEE80211_HW_SUPPORTS_TX_ENCAP_OFFLOAD,
	IEEE80211_HW_SUPPORTS_RX_DECAP_OFFLOAD, IEEE80211_HW_SUPPORTS_CONC_MON_RX_DECAP, IEEE80211_HW_DETECTS_COLOR_COLLISION, IEEE80211_HW_MLO_MCAST_MULTI_LINK_TX,
	IEEE80211_HW_DISALLOW_PUNCTURING, IEEE80211_HW_DISALLOW_PUNCTURING_5GHZ, IEEE80211_HW_HANDLES_QUIET_CSA, IEEE80211_HW_STRICT, NUM_IEEE80211_HW_FLAGS
};
enum ieee80211_conf_flags { IEEE80211_CONF_MONITOR = (1 << 0), IEEE80211_CONF_PS = (1 << 1), IEEE80211_CONF_IDLE = (1 << 2), IEEE80211_CONF_OFFCHANNEL = (1 << 3) };
enum ieee80211_conf_changed { IEEE80211_CONF_CHANGE_SMPS = BIT(1), IEEE80211_CONF_CHANGE_LISTEN_INTERVAL = BIT(2), IEEE80211_CONF_CHANGE_MONITOR = BIT(3), IEEE80211_CONF_CHANGE_PS = BIT(4),
	IEEE80211_CONF_CHANGE_POWER = BIT(5), IEEE80211_CONF_CHANGE_CHANNEL = BIT(6), IEEE80211_CONF_CHANGE_RETRY_LIMITS = BIT(7), IEEE80211_CONF_CHANGE_IDLE = BIT(8) };
enum ieee80211_bss_change { BSS_CHANGED_ASSOC = 1 << 0, BSS_CHANGED_ERP_CTS_PROT = 1 << 1, BSS_CHANGED_ERP_PREAMBLE = 1 << 2, BSS_CHANGED_ERP_SLOT = 1 << 3, BSS_CHANGED_HT = 1 << 4,
	BSS_CHANGED_BASIC_RATES = 1 << 5, BSS_CHANGED_BEACON_INT = 1 << 6, BSS_CHANGED_BSSID = 1 << 7, BSS_CHANGED_BEACON = 1 << 8, BSS_CHANGED_BEACON_ENABLED = 1 << 9, BSS_CHANGED_CQM = 1 << 10,
	BSS_CHANGED_IBSS = 1 << 11, BSS_CHANGED_ARP_FILTER = 1 << 12, BSS_CHANGED_QOS = 1 << 13, BSS_CHANGED_IDLE = 1 << 14, BSS_CHANGED_SSID = 1 << 15, BSS_CHANGED_AP_PROBE_RESP = 1 << 16,
	BSS_CHANGED_PS = 1 << 17, BSS_CHANGED_TXPOWER = 1 << 18, BSS_CHANGED_P2P_PS = 1 << 19, BSS_CHANGED_BEACON_INFO = 1 << 20, BSS_CHANGED_BANDWIDTH = 1 << 21, BSS_CHANGED_OCB = 1 << 22,
	BSS_CHANGED_MU_GROUPS = 1 << 23, BSS_CHANGED_KEEP_ALIVE = 1 << 24, BSS_CHANGED_MCAST_RATE = 1 << 25, BSS_CHANGED_FTM_RESPONDER = 1 << 26, BSS_CHANGED_TWT = 1 << 27,
	BSS_CHANGED_HE_OBSS_PD = 1 << 28, BSS_CHANGED_HE_BSS_COLOR = 1 << 29, BSS_CHANGED_FILS_DISCOVERY = 1 << 30, BSS_CHANGED_UNSOL_BCAST_PROBE_RESP = 1 << 31 };
enum ieee80211_filter_flags { FIF_ALLMULTI = 1 << 1, FIF_FCSFAIL = 1 << 2, FIF_PLCPFAIL = 1 << 3, FIF_BCN_PRBRESP_PROMISC = 1 << 4, FIF_CONTROL = 1 << 5, FIF_OTHER_BSS = 1 << 6,
	FIF_PSPOLL = 1 << 7, FIF_PROBE_REQ = 1 << 8, FIF_MCAST_ACTION = 1 << 9 };
enum ieee80211_ampdu_mlme_action { IEEE80211_AMPDU_RX_START, IEEE80211_AMPDU_RX_STOP, IEEE80211_AMPDU_TX_START, IEEE80211_AMPDU_TX_STOP_CONT, IEEE80211_AMPDU_TX_STOP_FLUSH,
	IEEE80211_AMPDU_TX_STOP_FLUSH_CONT, IEEE80211_AMPDU_TX_OPERATIONAL };
#define IEEE80211_AMPDU_TX_START_IMMEDIATE 1
#define IEEE80211_AMPDU_TX_START_DELAY_ADDBA 2
enum ieee80211_sta_state { IEEE80211_STA_NOTEXIST, IEEE80211_STA_NONE, IEEE80211_STA_AUTH, IEEE80211_STA_ASSOC, IEEE80211_STA_AUTHORIZED };
enum ieee80211_sta_rx_bandwidth { IEEE80211_STA_RX_BW_20 = 0, IEEE80211_STA_RX_BW_40, IEEE80211_STA_RX_BW_80, IEEE80211_STA_RX_BW_160, IEEE80211_STA_RX_BW_320 };
enum ieee80211_iface_iter { IEEE80211_IFACE_ITER_NORMAL = 0, IEEE80211_IFACE_ITER_RESUME_ALL = BIT(0), IEEE80211_IFACE_SKIP_SDATA_NOT_IN_DRIVER = BIT(1), IEEE80211_IFACE_ITER_ACTIVE = BIT(2) };
enum ieee80211_rate_control_changed { IEEE80211_RC_BW_CHANGED = BIT(0), IEEE80211_RC_SMPS_CHANGED = BIT(1), IEEE80211_RC_SUPP_RATES_CHANGED = BIT(2), IEEE80211_RC_NSS_CHANGED = BIT(3) };
enum sta_notify_cmd { STA_NOTIFY_SLEEP, STA_NOTIFY_AWAKE };
enum ieee80211_key_flags { IEEE80211_KEY_FLAG_GENERATE_IV_MGMT = BIT(0), IEEE80211_KEY_FLAG_GENERATE_IV = BIT(1), IEEE80211_KEY_FLAG_GENERATE_MMIC = BIT(2), IEEE80211_KEY_FLAG_PAIRWISE = BIT(3),
	IEEE80211_KEY_FLAG_SW_MGMT_TX = BIT(4), IEEE80211_KEY_FLAG_PUT_IV_SPACE = BIT(5), IEEE80211_KEY_FLAG_RX_MGMT = BIT(6), IEEE80211_KEY_FLAG_RESERVE_TAILROOM = BIT(7), IEEE80211_KEY_FLAG_PUT_MIC_SPACE = BIT(8),
	IEEE80211_KEY_FLAG_NO_AUTO_TX = BIT(9), IEEE80211_KEY_FLAG_GENERATE_MMIE = BIT(10), IEEE80211_KEY_FLAG_SPP_AMSDU = BIT(11) };
enum set_key_cmd { SET_KEY, DISABLE_KEY };
enum ieee80211_tx_info_flags { IEEE80211_TX_CTL_REQ_TX_STATUS = BIT(0), IEEE80211_TX_CTL_ASSIGN_SEQ = BIT(1), IEEE80211_TX_CTL_NO_ACK = BIT(2), IEEE80211_TX_CTL_CLEAR_PS_FILT = BIT(3),
	IEEE80211_TX_CTL_FIRST_FRAGMENT = BIT(4), IEEE80211_TX_CTL_SEND_AFTER_DTIM = BIT(5), IEEE80211_TX_CTL_AMPDU = BIT(6), IEEE80211_TX_CTL_INJECTED = BIT(7), IEEE80211_TX_STAT_TX_FILTERED = BIT(8),
	IEEE80211_TX_STAT_ACK = BIT(9), IEEE80211_TX_STAT_AMPDU = BIT(10), IEEE80211_TX_STAT_AMPDU_NO_BACK = BIT(11), IEEE80211_TX_CTL_RATE_CTRL_PROBE = BIT(12), IEEE80211_TX_INTFL_OFFCHAN_TX_OK = BIT(13),
	IEEE80211_TX_CTL_HW_80211_ENCAP = BIT(14), IEEE80211_TX_INTFL_RETRIED = BIT(15), IEEE80211_TX_INTFL_DONT_ENCRYPT = BIT(16), IEEE80211_TX_CTL_NO_PS_BUFFER = BIT(17), IEEE80211_TX_CTL_MORE_FRAMES = BIT(18),
	IEEE80211_TX_INTFL_RETRANSMISSION = BIT(19), IEEE80211_TX_INTFL_MLME_CONN_TX = BIT(20), IEEE80211_TX_INTFL_NL80211_FRAME_TX = BIT(21), IEEE80211_TX_CTL_LDPC = BIT(22), IEEE80211_TX_CTL_STBC = BIT(23) | BIT(24),
	IEEE80211_TX_CTL_TX_OFFCHAN = BIT(25), IEEE80211_TX_INTFL_TKIP_MIC_FAILURE = BIT(26), IEEE80211_TX_CTL_NO_CCK_RATE = BIT(27), IEEE80211_TX_STATUS_EOSP = BIT(28), IEEE80211_TX_CTL_USE_MINRATE = BIT(29),
	IEEE80211_TX_CTL_DONTFRAG = BIT(30), IEEE80211_TX_STAT_NOACK_TRANSMITTED = BIT(31) };
enum mac80211_rx_flags { RX_FLAG_MMIC_ERROR = BIT(0), RX_FLAG_DECRYPTED = BIT(1), RX_FLAG_ONLY_MONITOR = BIT(2), RX_FLAG_MMIC_STRIPPED = BIT(3), RX_FLAG_IV_STRIPPED = BIT(4),
	RX_FLAG_FAILED_FCS_CRC = BIT(5), RX_FLAG_FAILED_PLCP_CRC = BIT(6), RX_FLAG_MACTIME = BIT(7) | BIT(8), RX_FLAG_MACTIME_PLCP_START = 1 << 7, RX_FLAG_MACTIME_START = 2 << 7, RX_FLAG_MACTIME_END = 3 << 7,
	RX_FLAG_NO_SIGNAL_VAL = BIT(9), RX_FLAG_AMPDU_DETAILS = BIT(10), RX_FLAG_PN_VALIDATED = BIT(11), RX_FLAG_DUP_VALIDATED = BIT(12), RX_FLAG_AMPDU_LAST_KNOWN = BIT(13), RX_FLAG_AMPDU_IS_LAST = BIT(14),
	RX_FLAG_AMPDU_DELIM_CRC_ERROR = BIT(15), RX_FLAG_MIC_STRIPPED = BIT(17), RX_FLAG_ALLOW_SAME_PN = BIT(18), RX_FLAG_ICV_STRIPPED = BIT(19), RX_FLAG_AMSDU_MORE = BIT(20),
	RX_FLAG_RADIOTAP_TLV_AT_END = BIT(21), RX_FLAG_MIC_STRIPPED_RESERVED = BIT(22), RX_FLAG_SKIP_MONITOR = BIT(23), RX_FLAG_AMSDU_MORE_RESERVED = BIT(24), RX_FLAG_RADIOTAP_HE = BIT(25),
	RX_FLAG_RADIOTAP_HE_MU = BIT(26), RX_FLAG_RADIOTAP_LSIG = BIT(27), RX_FLAG_NO_PSDU = BIT(28), RX_FLAG_8023 = BIT(29) };
enum mac80211_rx_encoding_flags { RX_ENC_FLAG_SHORTPRE = BIT(0), RX_ENC_FLAG_SHORT_GI = BIT(2), RX_ENC_FLAG_HT_GF = BIT(3), RX_ENC_FLAG_STBC_MASK = BIT(4) | BIT(5), RX_ENC_FLAG_LDPC = BIT(6), RX_ENC_FLAG_BF = BIT(7) };
#define RX_ENC_FLAG_STBC_SHIFT 4
enum mac80211_rx_encoding { RX_ENC_LEGACY = 0, RX_ENC_HT, RX_ENC_VHT, RX_ENC_HE, RX_ENC_EHT };
enum ieee80211_roc_type { IEEE80211_ROC_TYPE_NORMAL = 0, IEEE80211_ROC_TYPE_MGMT_TX };
enum ieee80211_reconfig_type { IEEE80211_RECONFIG_TYPE_RESTART, IEEE80211_RECONFIG_TYPE_SUSPEND };
enum ieee80211_vif_flags { IEEE80211_VIF_BEACON_FILTER = BIT(0), IEEE80211_VIF_SUPPORTS_CQM_RSSI = BIT(1) };
enum ieee80211_chanctx_switch_mode { CHANCTX_SWMODE_REASSIGN_VIF, CHANCTX_SWMODE_SWAP_CONTEXTS };
struct ieee80211_vif_chanctx_switch;
enum ieee80211_vif_cfg_change { IEEE80211_VIF_CFG_ASSOC = BIT(0) };
enum ieee80211_frame_release_type { IEEE80211_FRAME_RELEASE_PSPOLL, IEEE80211_FRAME_RELEASE_UAPSD };

struct ieee80211_hw;
struct ieee80211_vif;
struct ieee80211_sta;
struct ieee80211_txq;
struct ieee80211_tx_rate { s8 idx; u16 count : 5, flags : 11; } __packed;
struct ieee80211_key_conf { atomic_t tx_pn; u32 cipher; u8 icv_len; u8 iv_len; u8 hw_key_idx; s8 keyidx; u16 flags; s8 link_id; u8 keylen; u8 key[]; };
struct ieee80211_tx_queue_params { u16 txop; u16 cw_min; u16 cw_max; u8 aifs; bool acm; bool uapsd; bool mu_edca; };
struct ieee80211_tx_control { struct ieee80211_sta *sta; };
struct ieee80211_scan_ies { const u8 *ies[NUM_NL80211_BANDS]; size_t len[NUM_NL80211_BANDS]; const u8 *common_ies; size_t common_ie_len; };
struct ieee80211_scan_request { struct ieee80211_scan_ies ies; struct cfg80211_scan_request req; };
struct ieee80211_conf { u32 flags; int power_level, dynamic_ps_timeout; u16 listen_interval; u8 ps_dtim_period; u8 long_frame_max_tx_count, short_frame_max_tx_count;
	struct cfg80211_chan_def chandef; bool radar_enabled; enum ieee80211_smps_mode smps_mode; };
struct ieee80211_hw {
	struct ieee80211_conf conf; struct wiphy *wiphy; const char *rate_control_algorithm; void *priv; unsigned long flags[BITS_TO_LONGS(NUM_IEEE80211_HW_FLAGS)];
	unsigned int extra_tx_headroom, extra_beacon_tailroom; int vif_data_size, sta_data_size, chanctx_data_size, txq_data_size; u16 queues; u16 max_listen_interval; s8 max_signal;
	u8 max_rates, max_report_rates, max_rate_tries, max_rx_aggregation_subframes, max_tx_aggregation_subframes, max_tx_fragments, offchannel_tx_hw_queue, radiotap_mcs_details;
	u16 radiotap_vht_details; u32 netdev_features; u8 uapsd_queues, uapsd_max_sp_len, max_nan_de_entries, tx_sk_pacing_shift, weight_multiplier; u32 max_mtu;
	const s8 *tx_power_levels; u8 max_txpwr_levels_idx;
};
static inline bool _ieee80211_hw_check(struct ieee80211_hw *hw, enum ieee80211_hw_flags f) { return test_bit(f, hw->flags) != 0; }
static inline void _ieee80211_hw_set(struct ieee80211_hw *hw, enum ieee80211_hw_flags f) { set_bit(f, hw->flags); }
#define ieee80211_hw_check(hw, flg) _ieee80211_hw_check(hw, IEEE80211_HW_##flg)
#define ieee80211_hw_set(hw, flg) _ieee80211_hw_set(hw, IEEE80211_HW_##flg)
struct ieee80211_chanctx_conf { struct cfg80211_chan_def def, min_def, ap; u8 rx_chains_static, rx_chains_dynamic; bool radar_enabled; u8 drv_priv[] __aligned(8); };
struct ieee80211_bss_conf {
	struct ieee80211_vif *vif; struct ieee80211_chanctx_conf *chanctx_conf; const u8 *bssid; unsigned int link_id; u8 addr[ETH_ALEN] __aligned(2); u8 htc_trig_based_pkt_ext;
	bool uora_exists; u8 uora_ocw_range; u16 frame_time_rts_th; bool he_support, twt_requester, twt_responder, twt_protected, twt_broadcast; bool use_cts_prot, use_short_preamble, use_short_slot;
	bool enable_beacon; u8 dtim_period; u16 beacon_int, assoc_capability; u64 sync_tsf; u32 sync_device_ts; u8 sync_dtim_count; u32 basic_rates; u8 mcast_rate[NUM_NL80211_BANDS];
	u16 ht_operation_mode; s32 cqm_rssi_thold; u32 cqm_rssi_hyst; s32 cqm_rssi_low, cqm_rssi_high; struct cfg80211_chan_def chanreq_oper; struct cfg80211_chan_def chandef;
	bool qos, hidden_ssid; int txpower; bool nontransmitted; u8 transmitter_bssid[ETH_ALEN]; u8 bssid_index, bssid_indicator; bool ema_ap; u8 profile_periodicity;
	bool csa_active; bool color_change_active; u8 color_change_color; bool ht_ldpc, vht_ldpc, he_ldpc, vht_su_beamformer, vht_su_beamformee, vht_mu_beamformer, vht_mu_beamformee;
	bool he_su_beamformer, he_su_beamformee, he_mu_beamformer, he_full_ul_mumimo, eht_su_beamformer, eht_su_beamformee, eht_mu_beamformer, eht_80mhz_full_bw_ul_mumimo, eht_disable_mcs15;
	u8 bss_param_ch_cnt, bss_param_ch_cnt_link_id;
};
struct ieee80211_vif_cfg { bool assoc, ibss_joined, ibss_creator, ps; u16 aid, eml_cap, eml_med_sync_delay, mld_capa_op; __be32 arp_addr_list[4]; int arp_addr_cnt; u8 ssid[IEEE80211_MAX_SSID_LEN];
	size_t ssid_len; bool s1g; bool idle; u8 ap_addr[ETH_ALEN] __aligned(2); };
struct ieee80211_vif {
	enum nl80211_iftype type; struct ieee80211_vif_cfg cfg; struct ieee80211_bss_conf bss_conf; struct ieee80211_bss_conf *link_conf[IEEE80211_MLD_MAX_NUM_LINKS]; u16 valid_links, active_links, dormant_links, suspended_links;
	u8 addr[ETH_ALEN] __aligned(2); bool p2p; u8 cab_queue; u8 hw_queue[IEEE80211_NUM_ACS]; struct ieee80211_txq *txq; u32 driver_flags, offload_flags; bool probe_req_reg, rx_mcast_action_reg;
	struct ieee80211_vif *mbssid_tx_vif; u8 drv_priv[] __aligned(8);
};
struct ieee80211_sta_aggregates { u16 max_amsdu_len; u16 max_rc_amsdu_len; u16 max_tid_amsdu_len[IEEE80211_NUM_TIDS]; };
struct ieee80211_link_sta { struct ieee80211_sta *sta; u8 addr[ETH_ALEN]; u8 link_id; enum ieee80211_smps_mode smps_mode; u32 supp_rates[NUM_NL80211_BANDS]; struct ieee80211_sta_ht_cap ht_cap;
	struct ieee80211_sta_vht_cap vht_cap; struct ieee80211_sta_aggregates agg; u8 rx_nss; enum ieee80211_sta_rx_bandwidth bandwidth; struct ieee80211_sta_txpwr { s16 power; u8 type; } txpwr; };
struct ieee80211_sta {
	u8 addr[ETH_ALEN] __aligned(2); u16 aid; u16 max_rx_aggregation_subframes; bool wme; u8 uapsd_queues, max_sp; struct ieee80211_sta_rates *rates; bool tdls, tdls_initiator, mfp, mlo, spp_amsdu;
	u8 max_amsdu_subframes; struct ieee80211_sta_aggregates *cur; bool support_p2p_ps; struct ieee80211_txq *txq[IEEE80211_NUM_TIDS + 1]; u16 valid_links;
	struct ieee80211_link_sta deflink; struct ieee80211_link_sta *link[IEEE80211_MLD_MAX_NUM_LINKS]; u8 drv_priv[] __aligned(8);
};
struct ieee80211_sta_rates { struct { s8 idx; u8 count, count_cts, count_rts; u16 flags; } rate[IEEE80211_TX_RATE_TABLE_SIZE]; };
struct ieee80211_txq { struct ieee80211_vif *vif; struct ieee80211_sta *sta; u8 tid; u8 ac; u8 drv_priv[] __aligned(8); };
struct ieee80211_tx_info {
	u32 flags; u32 band : 3, status_data_idr : 1, status_data : 13, hw_queue : 4, tx_time_est : 10; u16 ack_frame_id;
	union {
		struct { union { struct { struct ieee80211_tx_rate rates[IEEE80211_TX_MAX_RATES]; s8 rts_cts_rate_idx; u8 use_rts : 1, use_cts_prot : 1, short_preamble : 1, skip_table : 1;
				u8 antennas : 2; }; unsigned long enqueue_jiffies; }; union { struct ieee80211_vif *vif; void *tx_time_est_vif; }; struct ieee80211_key_conf *hw_key; u32 flags; } control;
		struct { u64 cookie; } ack;
		struct { struct ieee80211_tx_rate rates[IEEE80211_TX_MAX_RATES]; s32 ack_signal; u8 ampdu_ack_len, ampdu_len, antenna; u8 pad; u16 tx_time; u8 flags; u8 pad2; void *status_driver_data[16 / sizeof(void *)]; } status;
		struct { struct ieee80211_tx_rate driver_rates[IEEE80211_TX_MAX_RATES]; u8 pad[4]; void *rate_driver_data[24 / sizeof(void *)]; };
		void *driver_data[40 / sizeof(void *)];
	};
};
static inline struct ieee80211_tx_info *IEEE80211_SKB_CB(struct sk_buff *skb) { return (struct ieee80211_tx_info *)skb->cb; }
static inline void ieee80211_tx_info_clear_status(struct ieee80211_tx_info *info) { int i; for (i = 0; i < IEEE80211_TX_MAX_RATES; i++) info->status.rates[i].count = 0; memset(&info->status.ampdu_ack_len, 0, sizeof(*info) - offsetof(struct ieee80211_tx_info, status.ampdu_ack_len)); }
struct ieee80211_rx_status {
	u64 mactime; u64 boottime_ns; u32 device_timestamp; u32 ampdu_reference; u32 flag; u16 freq : 13, freq_offset : 1; u8 enc_flags; u8 encoding : 3, bw : 4; union { struct { u8 he_ru : 3, he_gi : 2, he_dcm : 1; }; struct { u8 ru : 4, gi : 2; } eht; };
	u8 rate_idx; u8 nss; u8 rx_flags; u8 band; u8 antenna; s8 signal; u8 chains; s8 chain_signal[IEEE80211_MAX_CHAINS]; u8 zero_length_psdu_type; u8 link_valid : 1, link_id : 4;
};
static inline struct ieee80211_rx_status *IEEE80211_SKB_RXCB(struct sk_buff *skb) { return (struct ieee80211_rx_status *)skb->cb; }
struct ieee80211_channel_switch { u64 timestamp; u32 device_timestamp; bool block_tx; struct cfg80211_chan_def chandef; u8 count; u8 link_id; u32 delay; };
struct ieee80211_ampdu_params { enum ieee80211_ampdu_mlme_action action; struct ieee80211_sta *sta; u16 tid; u16 ssn; u16 buf_size; bool amsdu; u16 timeout; };
struct ieee80211_low_level_stats { unsigned int dot11ACKFailureCount, dot11RTSFailureCount, dot11FCSErrorCount, dot11RTSSuccessCount; };
struct ieee80211_prep_tx_info { u16 duration; u16 subtype; u8 success : 1, was_assoc : 1; int link_id; };
struct ieee80211_tx_status { struct ieee80211_sta *sta; struct ieee80211_tx_info *info; struct sk_buff *skb; struct rate_info *rate; struct list_head *free_list; };
struct ieee80211_key_seq { union { struct { u32 iv32; u16 iv16; } tkip; struct { u8 pn[6]; } ccmp; struct { u8 pn[6]; } aes_cmac; struct { u8 pn[6]; } gcmp; struct { u8 seq[16]; u8 seq_len; } hw; }; };

struct ieee80211_ops {
	void (*tx)(struct ieee80211_hw *hw, struct ieee80211_tx_control *control, struct sk_buff *skb);
	int (*start)(struct ieee80211_hw *hw);
	void (*stop)(struct ieee80211_hw *hw, bool suspend);
	int (*suspend)(struct ieee80211_hw *hw, struct cfg80211_wowlan *wowlan);
	int (*resume)(struct ieee80211_hw *hw);
	void (*set_wakeup)(struct ieee80211_hw *hw, bool enabled);
	int (*add_interface)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	int (*change_interface)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, enum nl80211_iftype new_type, bool p2p);
	void (*remove_interface)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	int (*config)(struct ieee80211_hw *hw, int radio_idx, u32 changed);
	void (*vif_cfg_changed)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u64 changed);
	void (*link_info_changed)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_bss_conf *info, u64 changed);
	int (*start_ap)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_bss_conf *link_conf);
	void (*stop_ap)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_bss_conf *link_conf);
	void (*configure_filter)(struct ieee80211_hw *hw, unsigned int changed_flags, unsigned int *total_flags, u64 multicast);
	int (*set_key)(struct ieee80211_hw *hw, enum set_key_cmd cmd, struct ieee80211_vif *vif, struct ieee80211_sta *sta, struct ieee80211_key_conf *key);
	int (*hw_scan)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_scan_request *req);
	void (*cancel_hw_scan)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	void (*sw_scan_start)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, const u8 *mac_addr);
	void (*sw_scan_complete)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	int (*sta_add)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_sta *sta);
	int (*sta_remove)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_sta *sta);
	int (*sta_state)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_sta *sta, enum ieee80211_sta_state old_state, enum ieee80211_sta_state new_state);
	void (*sta_rc_update)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_sta *sta, u32 changed);
	void (*link_sta_rc_update)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_link_sta *link_sta, u32 changed);
	int (*conf_tx)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, unsigned int link_id, u16 ac, const struct ieee80211_tx_queue_params *params);
	u64 (*get_tsf)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	void (*set_tsf)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u64 tsf);
	int (*ampdu_action)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_ampdu_params *params);
	void (*flush)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u32 queues, bool drop);
	int (*set_bitrate_mask)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, const struct cfg80211_bitrate_mask *mask);
	int (*set_antenna)(struct ieee80211_hw *hw, int radio_idx, u32 tx_ant, u32 rx_ant);
	int (*get_antenna)(struct ieee80211_hw *hw, int radio_idx, u32 *tx_ant, u32 *rx_ant);
	void (*reconfig_complete)(struct ieee80211_hw *hw, enum ieee80211_reconfig_type reconfig_type);
	void (*mgd_prepare_tx)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_prep_tx_info *info);
	void (*wake_tx_queue)(struct ieee80211_hw *hw, struct ieee80211_txq *txq);
	int (*set_sar_specs)(struct ieee80211_hw *hw, const struct cfg80211_sar_specs *sar);
	/* the members rtw88's own mac80211.c fills (vendored 2026-10-10 so the glue drives the real rtw_ops in mac80211's
	 * order) */
	int (*add_chanctx)(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx);
	void (*remove_chanctx)(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx);
	void (*change_chanctx)(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx, u32 changed);
	int (*switch_vif_chanctx)(struct ieee80211_hw *hw, struct ieee80211_vif_chanctx_switch *vifs, int n_vifs, enum ieee80211_chanctx_switch_mode mode);
	void (*bss_info_changed)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_bss_conf *info, u64 changed);
	int (*set_tim)(struct ieee80211_hw *hw, struct ieee80211_sta *sta, bool set);
	bool (*can_aggregate_in_amsdu)(struct ieee80211_hw *hw, struct sk_buff *head, struct sk_buff *skb);
	void (*sta_statistics)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_sta *sta, struct station_info *sinfo);
	int (*set_rts_threshold)(struct ieee80211_hw *hw, int radio_idx, u32 value);
	void (*rfkill_poll)(struct ieee80211_hw *hw);
	void (*channel_switch)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_channel_switch *ch_switch);
	int (*remain_on_channel)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_channel *chan, int duration, enum ieee80211_roc_type type);
	int (*cancel_remain_on_channel)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	int (*get_survey)(struct ieee80211_hw *hw, int idx, struct survey_info *survey);
};

/* implemented by shim_mac80211.c */
int ieee80211_emulate_add_chanctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx);
void ieee80211_emulate_remove_chanctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx);
void ieee80211_emulate_change_chanctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx, u32 changed);
int ieee80211_emulate_switch_vif_chanctx(struct ieee80211_hw *hw, struct ieee80211_vif_chanctx_switch *vifs, int n_vifs, enum ieee80211_chanctx_switch_mode mode);
void ieee80211_stop_tx_ba_cb_irqsafe(struct ieee80211_vif *vif, const u8 *ra, u16 tid);
static inline enum nl80211_iftype ieee80211_vif_type_p2p(struct ieee80211_vif *vif) { return vif->type; }
/* the one interface and the one station (the AP) the glue registers, as mac80211's lists would hold them: the
 * iterators and the station lookups visit them (NULL = none) */
void shim_mac80211_set_vif(struct ieee80211_vif *vif);
void shim_mac80211_set_sta(struct ieee80211_sta *sta);
unsigned shim_mac80211_bss_count(void);
int shim_mac80211_tx_track(struct sk_buff *skb, void *cookie);	/* a64wlan_glue.c: the cookie of an skb handed to rtw88 */
unsigned a64wlan_rx_keyalgo(const u8 *frame, unsigned len);	/* a64wlan_glue.c: A64WLAN_KEYALGO_* of the key a protected frame used */
struct ieee80211_hw *ieee80211_alloc_hw(size_t priv_data_len, const struct ieee80211_ops *ops);
void ieee80211_free_hw(struct ieee80211_hw *hw);
int ieee80211_register_hw(struct ieee80211_hw *hw);
void ieee80211_unregister_hw(struct ieee80211_hw *hw);
void ieee80211_restart_hw(struct ieee80211_hw *hw);
static inline void SET_IEEE80211_PERM_ADDR(struct ieee80211_hw *hw, const u8 *addr) { memcpy(hw->wiphy->perm_addr, addr, ETH_ALEN); }
void ieee80211_rx_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb);
void ieee80211_rx_napi(struct ieee80211_hw *hw, struct ieee80211_sta *sta, struct sk_buff *skb, void *napi);
static inline void ieee80211_rx(struct ieee80211_hw *hw, struct sk_buff *skb) { ieee80211_rx_irqsafe(hw, skb); }
void ieee80211_tx_status_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb);
static inline void ieee80211_tx_status_skb(struct ieee80211_hw *hw, struct sk_buff *skb) { ieee80211_tx_status_irqsafe(hw, skb); }
void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb);
void ieee80211_purge_tx_queue(struct ieee80211_hw *hw, struct sk_buff_head *skbs);
void ieee80211_stop_queues(struct ieee80211_hw *hw);
void ieee80211_wake_queues(struct ieee80211_hw *hw);
void ieee80211_stop_queue(struct ieee80211_hw *hw, int queue);
void ieee80211_wake_queue(struct ieee80211_hw *hw, int queue);
void ieee80211_queue_work(struct ieee80211_hw *hw, struct work_struct *work);
void ieee80211_queue_delayed_work(struct ieee80211_hw *hw, struct delayed_work *dwork, unsigned long delay);
void ieee80211_scan_completed(struct ieee80211_hw *hw, struct cfg80211_scan_info *info);
void ieee80211_sched_scan_results(struct ieee80211_hw *hw);
void ieee80211_sched_scan_stopped(struct ieee80211_hw *hw);
void ieee80211_connection_loss(struct ieee80211_vif *vif);
void ieee80211_beacon_loss(struct ieee80211_vif *vif);
void ieee80211_cqm_rssi_notify(struct ieee80211_vif *vif, enum nl80211_cqm_rssi_threshold_event rssi_event, s32 rssi_level, gfp_t gfp);
void ieee80211_request_smps(struct ieee80211_vif *vif, unsigned int link_id, enum ieee80211_smps_mode smps_mode);
int ieee80211_start_tx_ba_session(struct ieee80211_sta *sta, u16 tid, u16 timeout);
int ieee80211_stop_tx_ba_session(struct ieee80211_sta *sta, u16 tid);
void ieee80211_iterate_active_interfaces_atomic(struct ieee80211_hw *hw, u32 iter_flags, void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif), void *data);
void ieee80211_iterate_active_interfaces(struct ieee80211_hw *hw, u32 iter_flags, void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif), void *data);
void ieee80211_iterate_stations_atomic(struct ieee80211_hw *hw, void (*iterator)(void *data, struct ieee80211_sta *sta), void *data);
void ieee80211_iterate_stations(struct ieee80211_hw *hw, void (*iterator)(void *data, struct ieee80211_sta *sta), void *data);
void ieee80211_iter_keys(struct ieee80211_hw *hw, struct ieee80211_vif *vif, void (*iter)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_sta *sta, struct ieee80211_key_conf *key, void *data), void *iter_data);
void ieee80211_iter_keys_rcu(struct ieee80211_hw *hw, struct ieee80211_vif *vif, void (*iter)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_sta *sta, struct ieee80211_key_conf *key, void *data), void *iter_data);
struct ieee80211_sta *ieee80211_find_sta(struct ieee80211_vif *vif, const u8 *addr);
struct ieee80211_sta *ieee80211_find_sta_by_ifaddr(struct ieee80211_hw *hw, const u8 *addr, const u8 *localaddr);
struct sk_buff *ieee80211_beacon_get_tim(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u16 *tim_offset, u16 *tim_length, unsigned int link_id);
static inline struct sk_buff *ieee80211_beacon_get(struct ieee80211_hw *hw, struct ieee80211_vif *vif, unsigned int link_id) { return ieee80211_beacon_get_tim(hw, vif, 0, 0, link_id); }
struct sk_buff *ieee80211_nullfunc_get(struct ieee80211_hw *hw, struct ieee80211_vif *vif, int link_id, bool qos_ok);
struct sk_buff *ieee80211_pspoll_get(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
struct sk_buff *ieee80211_probereq_get(struct ieee80211_hw *hw, const u8 *src_addr, const u8 *ssid, size_t ssid_len, size_t tailroom);
struct sk_buff *ieee80211_proberesp_get(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
struct sk_buff *ieee80211_tx_dequeue(struct ieee80211_hw *hw, struct ieee80211_txq *txq);
void ieee80211_txq_get_depth(struct ieee80211_txq *txq, unsigned long *frame_cnt, unsigned long *byte_cnt);
void ieee80211_txq_schedule_start(struct ieee80211_hw *hw, u8 ac);
void ieee80211_txq_schedule_end(struct ieee80211_hw *hw, u8 ac);
struct ieee80211_txq *ieee80211_next_txq(struct ieee80211_hw *hw, u8 ac);
void ieee80211_return_txq(struct ieee80211_hw *hw, struct ieee80211_txq *txq, bool force);
void ieee80211_handle_wake_tx_queue(struct ieee80211_hw *hw, struct ieee80211_txq *txq);
void ieee80211_report_wowlan_wakeup(struct ieee80211_vif *vif, struct cfg80211_wowlan_wakeup *wakeup, gfp_t gfp);
void ieee80211_get_key_rx_seq(struct ieee80211_key_conf *keyconf, int tid, struct ieee80211_key_seq *seq);
static inline struct ieee80211_rate *ieee80211_get_tx_rate(const struct ieee80211_hw *hw, const struct ieee80211_tx_info *c) { if (c->control.rates[0].idx < 0) return (struct ieee80211_rate *)0; return &hw->wiphy->bands[c->band]->bitrates[c->control.rates[0].idx]; }
static inline int ieee80211_get_tx_power(void) { return 0; }
#endif
