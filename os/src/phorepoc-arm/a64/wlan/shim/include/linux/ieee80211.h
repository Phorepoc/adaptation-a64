/* shim linux/ieee80211.h -- OURS: the IEEE 802.11 wire-format definitions rtw88 uses (frame control, the MAC header,
 * management frames, HT/VHT capability fields, EIDs, cipher suites). Layouts follow the 802.11 standard; the names
 * follow Linux so the vendored driver compiles unchanged. See shim_core.h. */
#ifndef SHIM_LINUX_IEEE80211_H
#define SHIM_LINUX_IEEE80211_H
#include "../shim_core.h"

#define FCS_LEN 4
#define IEEE80211_FCTL_VERS 0x0003
#define IEEE80211_FCTL_FTYPE 0x000c
#define IEEE80211_FCTL_STYPE 0x00f0
#define IEEE80211_FCTL_TODS 0x0100
#define IEEE80211_FCTL_FROMDS 0x0200
#define IEEE80211_FCTL_MOREFRAGS 0x0400
#define IEEE80211_FCTL_RETRY 0x0800
#define IEEE80211_FCTL_PM 0x1000
#define IEEE80211_FCTL_MOREDATA 0x2000
#define IEEE80211_FCTL_PROTECTED 0x4000
#define IEEE80211_FCTL_ORDER 0x8000
#define IEEE80211_SCTL_FRAG 0x000F
#define IEEE80211_SCTL_SEQ 0xFFF0
#define IEEE80211_FTYPE_MGMT 0x0000
#define IEEE80211_FTYPE_CTL 0x0004
#define IEEE80211_FTYPE_DATA 0x0008
#define IEEE80211_FTYPE_EXT 0x000c
#define IEEE80211_STYPE_ASSOC_REQ 0x0000
#define IEEE80211_STYPE_ASSOC_RESP 0x0010
#define IEEE80211_STYPE_REASSOC_REQ 0x0020
#define IEEE80211_STYPE_REASSOC_RESP 0x0030
#define IEEE80211_STYPE_PROBE_REQ 0x0040
#define IEEE80211_STYPE_PROBE_RESP 0x0050
#define IEEE80211_STYPE_BEACON 0x0080
#define IEEE80211_STYPE_ATIM 0x0090
#define IEEE80211_STYPE_DISASSOC 0x00A0
#define IEEE80211_STYPE_AUTH 0x00B0
#define IEEE80211_STYPE_DEAUTH 0x00C0
#define IEEE80211_STYPE_ACTION 0x00D0
#define IEEE80211_STYPE_BACK_REQ 0x0080
#define IEEE80211_STYPE_BACK 0x0090
#define IEEE80211_STYPE_PSPOLL 0x00A0
#define IEEE80211_STYPE_RTS 0x00B0
#define IEEE80211_STYPE_CTS 0x00C0
#define IEEE80211_STYPE_ACK 0x00D0
#define IEEE80211_STYPE_CFEND 0x00E0
#define IEEE80211_STYPE_DATA 0x0000
#define IEEE80211_STYPE_DATA_CFACK 0x0010
#define IEEE80211_STYPE_DATA_CFPOLL 0x0020
#define IEEE80211_STYPE_DATA_CFACKPOLL 0x0030
#define IEEE80211_STYPE_NULLFUNC 0x0040
#define IEEE80211_STYPE_CFACK 0x0050
#define IEEE80211_STYPE_CFPOLL 0x0060
#define IEEE80211_STYPE_CFACKPOLL 0x0070
#define IEEE80211_STYPE_QOS_DATA 0x0080
#define IEEE80211_STYPE_QOS_NULLFUNC 0x00C0
#define IEEE80211_QOS_CTL_LEN 2
#define IEEE80211_QOS_CTL_TID_MASK 0x000f
#define IEEE80211_QOS_CTL_ACK_POLICY_NOACK 0x0020
#define IEEE80211_QOS_CTL_A_MSDU_PRESENT 0x0080
#define IEEE80211_HT_CTL_LEN 4
#define IEEE80211_MAX_SSID_LEN 32
#define IEEE80211_MAX_DATA_LEN 2304
#define IEEE80211_MAX_FRAME_LEN 2352
#define IEEE80211_MAX_MPDU_LEN_HT_3839 3839
#define IEEE80211_MAX_MPDU_LEN_HT_7935 7935
#define IEEE80211_MAX_MPDU_LEN_VHT_3895 3895
#define IEEE80211_MAX_MPDU_LEN_VHT_7991 7991
#define IEEE80211_MAX_MPDU_LEN_VHT_11454 11454
#define IEEE80211_MIN_ACTION_SIZE 25
#define IEEE80211_NUM_TIDS 16
#define IEEE80211_NUM_ACS 4
#define IEEE80211_MAX_AMPDU_BUF_HT 0x40
#define IEEE80211_MAX_AMPDU_BUF 0x100
#define IEEE80211_MAX_CHAINS 4
#define IEEE80211_CCMP_HDR_LEN 8
#define IEEE80211_CCMP_MIC_LEN 8
#define IEEE80211_TKIP_IV_LEN 8
#define IEEE80211_TKIP_ICV_LEN 4
#define IEEE80211_WEP_IV_LEN 4
#define IEEE80211_WEP_ICV_LEN 4
#define WLAN_SA_QUERY_TR_ID_LEN 2
#define WLAN_MEMBERSHIP_LEN 8
#define WLAN_USER_POSITION_LEN 16

enum ieee80211_ac_numbers { IEEE80211_AC_VO = 0, IEEE80211_AC_VI = 1, IEEE80211_AC_BE = 2, IEEE80211_AC_BK = 3 };
enum ieee80211_smps_mode { IEEE80211_SMPS_AUTOMATIC, IEEE80211_SMPS_OFF, IEEE80211_SMPS_STATIC, IEEE80211_SMPS_DYNAMIC, IEEE80211_SMPS_NUM_MODES };

struct ieee80211_hdr { __le16 frame_control; __le16 duration_id; u8 addr1[6]; u8 addr2[6]; u8 addr3[6]; __le16 seq_ctrl; u8 addr4[6]; } __packed __aligned(2);
struct ieee80211_hdr_3addr { __le16 frame_control; __le16 duration_id; u8 addr1[6]; u8 addr2[6]; u8 addr3[6]; __le16 seq_ctrl; } __packed __aligned(2);
struct ieee80211_qos_hdr { __le16 frame_control; __le16 duration_id; u8 addr1[6]; u8 addr2[6]; u8 addr3[6]; __le16 seq_ctrl; __le16 qos_ctrl; } __packed __aligned(2);
struct ieee80211_pspoll { __le16 frame_control; __le16 aid; u8 bssid[6]; u8 ta[6]; } __packed __aligned(2);
struct ieee80211_rts { __le16 frame_control; __le16 duration; u8 ra[6]; u8 ta[6]; } __packed __aligned(2);
struct ieee80211_cts { __le16 frame_control; __le16 duration; u8 ra[6]; } __packed __aligned(2);
struct ieee80211_bar { __le16 frame_control; __le16 duration; u8 ra[6]; u8 ta[6]; __le16 control; __le16 start_seq_num; } __packed;
#define IEEE80211_BAR_CTRL_ACK_POLICY_NORMAL 0x0000
#define IEEE80211_BAR_CTRL_MULTI_TID 0x0002
#define IEEE80211_BAR_CTRL_CBMTID_COMPRESSED_BA 0x0004
#define IEEE80211_BAR_CTRL_TID_INFO_MASK 0xf000
#define IEEE80211_BAR_CTRL_TID_INFO_SHIFT 12

struct ieee80211_mcs_info { u8 rx_mask[10]; __le16 rx_highest; u8 tx_params; u8 reserved[3]; } __packed;
#define IEEE80211_HT_MCS_MASK_LEN 10
#define IEEE80211_HT_MCS_RX_HIGHEST_MASK 0x3ff
#define IEEE80211_HT_MCS_TX_DEFINED 0x01
#define IEEE80211_HT_MCS_TX_RX_DIFF 0x02
#define IEEE80211_HT_MCS_TX_MAX_STREAMS_MASK 0x0C
#define IEEE80211_HT_MCS_TX_MAX_STREAMS_SHIFT 2
#define IEEE80211_HT_MCS_TX_MAX_STREAMS 4
#define IEEE80211_HT_MCS_TX_UNEQUAL_MODULATION 0x10
struct ieee80211_ht_cap { __le16 cap_info; u8 ampdu_params_info; struct ieee80211_mcs_info mcs; __le16 extended_ht_cap_info; __le32 tx_BF_cap_info; u8 antenna_selection_info; } __packed;
#define IEEE80211_HT_CAP_LDPC_CODING 0x0001
#define IEEE80211_HT_CAP_SUP_WIDTH_20_40 0x0002
#define IEEE80211_HT_CAP_SM_PS 0x000C
#define IEEE80211_HT_CAP_SM_PS_SHIFT 2
#define IEEE80211_HT_CAP_GRN_FLD 0x0010
#define IEEE80211_HT_CAP_SGI_20 0x0020
#define IEEE80211_HT_CAP_SGI_40 0x0040
#define IEEE80211_HT_CAP_TX_STBC 0x0080
#define IEEE80211_HT_CAP_RX_STBC 0x0300
#define IEEE80211_HT_CAP_RX_STBC_SHIFT 8
#define IEEE80211_HT_CAP_DELAY_BA 0x0400
#define IEEE80211_HT_CAP_MAX_AMSDU 0x0800
#define IEEE80211_HT_CAP_DSSSCCK40 0x1000
#define IEEE80211_HT_CAP_RESERVED 0x2000
#define IEEE80211_HT_CAP_40MHZ_INTOLERANT 0x4000
#define IEEE80211_HT_CAP_LSIG_TXOP_PROT 0x8000
#define IEEE80211_HT_AMPDU_PARM_FACTOR 0x03
#define IEEE80211_HT_AMPDU_PARM_DENSITY 0x1C
#define IEEE80211_HT_AMPDU_PARM_DENSITY_SHIFT 2
enum ieee80211_max_ampdu_length_exp { IEEE80211_HT_MAX_AMPDU_8K = 0, IEEE80211_HT_MAX_AMPDU_16K = 1, IEEE80211_HT_MAX_AMPDU_32K = 2, IEEE80211_HT_MAX_AMPDU_64K = 3 };
#define IEEE80211_HT_MAX_AMPDU_FACTOR 13
enum ieee80211_min_mpdu_spacing { IEEE80211_HT_MPDU_DENSITY_NONE = 0, IEEE80211_HT_MPDU_DENSITY_0_25 = 1, IEEE80211_HT_MPDU_DENSITY_0_5 = 2, IEEE80211_HT_MPDU_DENSITY_1 = 3, IEEE80211_HT_MPDU_DENSITY_2 = 4, IEEE80211_HT_MPDU_DENSITY_4 = 5, IEEE80211_HT_MPDU_DENSITY_8 = 6, IEEE80211_HT_MPDU_DENSITY_16 = 7 };
struct ieee80211_ht_operation { u8 primary_chan; u8 ht_param; __le16 operation_mode; __le16 stbc_param; u8 basic_set[16]; } __packed;
#define IEEE80211_HT_PARAM_CHA_SEC_OFFSET 0x03
#define IEEE80211_HT_PARAM_CHA_SEC_NONE 0x00
#define IEEE80211_HT_PARAM_CHA_SEC_ABOVE 0x01
#define IEEE80211_HT_PARAM_CHA_SEC_BELOW 0x03
#define IEEE80211_HT_PARAM_CHAN_WIDTH_ANY 0x04

struct ieee80211_vht_mcs_info { __le16 rx_mcs_map; __le16 rx_highest; __le16 tx_mcs_map; __le16 tx_highest; } __packed;
enum ieee80211_vht_mcs_support { IEEE80211_VHT_MCS_SUPPORT_0_7 = 0, IEEE80211_VHT_MCS_SUPPORT_0_8 = 1, IEEE80211_VHT_MCS_SUPPORT_0_9 = 2, IEEE80211_VHT_MCS_NOT_SUPPORTED = 3 };
struct ieee80211_vht_cap { __le32 vht_cap_info; struct ieee80211_vht_mcs_info supp_mcs; } __packed;
#define IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_3895 0x00000000
#define IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_7991 0x00000001
#define IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_11454 0x00000002
#define IEEE80211_VHT_CAP_MAX_MPDU_MASK 0x00000003
#define IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160MHZ 0x00000004
#define IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160_80PLUS80MHZ 0x00000008
#define IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_MASK 0x0000000C
#define IEEE80211_VHT_CAP_RXLDPC 0x00000010
#define IEEE80211_VHT_CAP_SHORT_GI_80 0x00000020
#define IEEE80211_VHT_CAP_SHORT_GI_160 0x00000040
#define IEEE80211_VHT_CAP_TXSTBC 0x00000080
#define IEEE80211_VHT_CAP_RXSTBC_1 0x00000100
#define IEEE80211_VHT_CAP_RXSTBC_2 0x00000200
#define IEEE80211_VHT_CAP_RXSTBC_3 0x00000300
#define IEEE80211_VHT_CAP_RXSTBC_4 0x00000400
#define IEEE80211_VHT_CAP_RXSTBC_MASK 0x00000700
#define IEEE80211_VHT_CAP_RXSTBC_SHIFT 8
#define IEEE80211_VHT_CAP_SU_BEAMFORMER_CAPABLE 0x00000800
#define IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE 0x00001000
#define IEEE80211_VHT_CAP_BEAMFORMEE_STS_SHIFT 13
#define IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK (7 << IEEE80211_VHT_CAP_BEAMFORMEE_STS_SHIFT)
#define IEEE80211_VHT_CAP_SOUNDING_DIMENSIONS_SHIFT 16
#define IEEE80211_VHT_CAP_SOUNDING_DIMENSIONS_MASK (7 << IEEE80211_VHT_CAP_SOUNDING_DIMENSIONS_SHIFT)
#define IEEE80211_VHT_CAP_MU_BEAMFORMER_CAPABLE 0x00080000
#define IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE 0x00100000
#define IEEE80211_VHT_CAP_VHT_TXOP_PS 0x00200000
#define IEEE80211_VHT_CAP_HTC_VHT 0x00400000
#define IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT 23
#define IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_MASK (7 << IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT)
#define IEEE80211_VHT_CAP_VHT_LINK_ADAPTATION_VHT_UNSOL_MFB 0x08000000
#define IEEE80211_VHT_CAP_VHT_LINK_ADAPTATION_VHT_MRQ_MFB 0x0c000000
#define IEEE80211_VHT_CAP_RX_ANTENNA_PATTERN 0x10000000
#define IEEE80211_VHT_CAP_TX_ANTENNA_PATTERN 0x20000000
struct ieee80211_vht_operation { u8 chan_width; u8 center_freq_seg0_idx; u8 center_freq_seg1_idx; __le16 basic_mcs_set; } __packed;

struct ieee80211_tim_ie { u8 dtim_count; u8 dtim_period; u8 bitmap_ctrl; u8 virtual_map[1]; } __packed;

struct ieee80211_mgmt {
	__le16 frame_control; __le16 duration; u8 da[6]; u8 sa[6]; u8 bssid[6]; __le16 seq_ctrl;
	union {
		struct { __le16 auth_alg; __le16 auth_transaction; __le16 status_code; u8 variable[]; } __packed auth;
		struct { __le16 reason_code; } __packed deauth;
		struct { __le16 capab_info; __le16 listen_interval; u8 variable[]; } __packed assoc_req;
		struct { __le16 capab_info; __le16 status_code; __le16 aid; u8 variable[]; } __packed assoc_resp, reassoc_resp;
		struct { __le16 capab_info; __le16 listen_interval; u8 current_ap[6]; u8 variable[]; } __packed reassoc_req;
		struct { __le16 reason_code; } __packed disassoc;
		struct { __le64 timestamp; __le16 beacon_int; __le16 capab_info; u8 variable[]; } __packed beacon;
		struct { u8 variable[0]; } __packed probe_req;
		struct { __le64 timestamp; __le16 beacon_int; __le16 capab_info; u8 variable[]; } __packed probe_resp;
		struct { u8 category; union { struct { u8 action_code; u8 dialog_token; u8 status_code; u8 variable[]; } __packed wme_action;
			struct { u8 action_code; u8 dialog_token; __le16 capab; __le16 timeout; __le16 start_seq_num; u8 variable[]; } __packed addba_req;
			struct { u8 action_code; u8 dialog_token; __le16 status; __le16 capab; __le16 timeout; } __packed addba_resp;
			struct { u8 action_code; __le16 params; __le16 reason_code; } __packed delba;
			struct { u8 action_code; u8 variable[]; } __packed self_prot;
			struct { u8 action_code; u8 smps_control; } __packed ht_smps;
			struct { u8 action_code; u8 variable[]; } __packed vht_opmode_notif; } u; } __packed action;
	} u;
} __packed __aligned(2);

#define WLAN_CAPABILITY_ESS (1 << 0)
#define WLAN_CAPABILITY_IBSS (1 << 1)
#define WLAN_CAPABILITY_PRIVACY (1 << 4)
#define WLAN_CAPABILITY_SHORT_PREAMBLE (1 << 5)
#define WLAN_CAPABILITY_SHORT_SLOT_TIME (1 << 10)
#define WLAN_ACTION_SPCT_CHL_SWITCH 4

enum ieee80211_eid {
	WLAN_EID_SSID = 0, WLAN_EID_SUPP_RATES = 1, WLAN_EID_DS_PARAMS = 3, WLAN_EID_CF_PARAMS = 4, WLAN_EID_TIM = 5,
	WLAN_EID_IBSS_PARAMS = 6, WLAN_EID_COUNTRY = 7, WLAN_EID_CHALLENGE = 16, WLAN_EID_PWR_CONSTRAINT = 32,
	WLAN_EID_CHANNEL_SWITCH = 37, WLAN_EID_ERP_INFO = 42, WLAN_EID_HT_CAPABILITY = 45, WLAN_EID_RSN = 48,
	WLAN_EID_EXT_SUPP_RATES = 50, WLAN_EID_HT_OPERATION = 61, WLAN_EID_SECONDARY_CHANNEL_OFFSET = 62,
	WLAN_EID_RRM_ENABLED_CAPABILITIES = 70, WLAN_EID_EXT_CHANSWITCH_ANN = 60, WLAN_EID_VHT_CAPABILITY = 191,
	WLAN_EID_VHT_OPERATION = 192, WLAN_EID_VENDOR_SPECIFIC = 221, WLAN_EID_EXTENSION = 255
};
#define WLAN_OUI_MICROSOFT 0x0050f2
#define WLAN_OUI_TYPE_MICROSOFT_WPA 1
#define WLAN_OUI_TYPE_MICROSOFT_WMM 2
#define WLAN_OUI_WFA 0x506f9a
#define WLAN_CIPHER_SUITE_USE_GROUP 0x000FAC00
#define WLAN_CIPHER_SUITE_WEP40 0x000FAC01
#define WLAN_CIPHER_SUITE_TKIP 0x000FAC02
#define WLAN_CIPHER_SUITE_CCMP 0x000FAC04
#define WLAN_CIPHER_SUITE_WEP104 0x000FAC05
#define WLAN_CIPHER_SUITE_AES_CMAC 0x000FAC06
#define WLAN_CIPHER_SUITE_GCMP 0x000FAC08
#define WLAN_CIPHER_SUITE_GCMP_256 0x000FAC09
#define WLAN_CIPHER_SUITE_CCMP_256 0x000FAC0A
#define WLAN_CIPHER_SUITE_BIP_GMAC_128 0x000FAC0B
#define WLAN_CIPHER_SUITE_BIP_GMAC_256 0x000FAC0C
#define WLAN_CIPHER_SUITE_BIP_CMAC_256 0x000FAC0D
#define WLAN_KEY_LEN_WEP40 5
#define WLAN_KEY_LEN_WEP104 13
#define WLAN_KEY_LEN_CCMP 16
#define WLAN_KEY_LEN_TKIP 32
#define WLAN_KEY_LEN_AES_CMAC 16
#define WLAN_AUTH_OPEN 0
#define WLAN_AUTH_SHARED_KEY 1
#define WLAN_STATUS_SUCCESS 0
#define WLAN_REASON_DEAUTH_LEAVING 3
#define WLAN_REASON_DISASSOC_STA_HAS_LEFT 8

struct element { u8 id; u8 datalen; u8 data[]; } __packed;
#define for_each_element(_elem, _data, _datalen) \
	for (_elem = (const struct element *)(_data); \
	     (const u8 *)(_data) + (_datalen) - (const u8 *)_elem >= (int)sizeof(*_elem) && \
	     (const u8 *)(_data) + (_datalen) - (const u8 *)_elem >= (int)sizeof(*_elem) + _elem->datalen; \
	     _elem = (const struct element *)(_elem->data + _elem->datalen))
#define for_each_element_id(element, _id, data, datalen) for_each_element(element, data, datalen) if (element->id == (_id))

static inline bool ieee80211_has_tods(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_TODS)) != 0; }
static inline bool ieee80211_has_fromds(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_FROMDS)) != 0; }
static inline bool ieee80211_has_a4(__le16 fc) { __le16 t = cpu_to_le16(IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS); return (fc & t) == t; }
static inline bool ieee80211_has_morefrags(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_MOREFRAGS)) != 0; }
static inline bool ieee80211_has_retry(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_RETRY)) != 0; }
static inline bool ieee80211_has_pm(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_PM)) != 0; }
static inline bool ieee80211_has_moredata(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_MOREDATA)) != 0; }
static inline bool ieee80211_has_protected(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_PROTECTED)) != 0; }
static inline bool ieee80211_has_order(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_ORDER)) != 0; }
static inline bool ieee80211_is_mgmt(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_FTYPE)) == cpu_to_le16(IEEE80211_FTYPE_MGMT); }
static inline bool ieee80211_is_ctl(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_FTYPE)) == cpu_to_le16(IEEE80211_FTYPE_CTL); }
static inline bool ieee80211_is_data(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_FTYPE)) == cpu_to_le16(IEEE80211_FTYPE_DATA); }
static inline bool ieee80211_is_data_qos(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_FTYPE | IEEE80211_STYPE_QOS_DATA)) == cpu_to_le16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_QOS_DATA); }
static inline bool ieee80211_is_data_present(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_FTYPE | 0x40)) == cpu_to_le16(IEEE80211_FTYPE_DATA); }
#define IEEE80211_IS_STYPE(name, ftype, stype) static inline bool ieee80211_is_##name(__le16 fc) { return (fc & cpu_to_le16(IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) == cpu_to_le16(ftype | stype); }
IEEE80211_IS_STYPE(assoc_req, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_ASSOC_REQ)
IEEE80211_IS_STYPE(assoc_resp, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_ASSOC_RESP)
IEEE80211_IS_STYPE(reassoc_req, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_REASSOC_REQ)
IEEE80211_IS_STYPE(reassoc_resp, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_REASSOC_RESP)
IEEE80211_IS_STYPE(probe_req, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_PROBE_REQ)
IEEE80211_IS_STYPE(probe_resp, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_PROBE_RESP)
IEEE80211_IS_STYPE(beacon, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_BEACON)
IEEE80211_IS_STYPE(atim, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_ATIM)
IEEE80211_IS_STYPE(disassoc, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_DISASSOC)
IEEE80211_IS_STYPE(auth, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_AUTH)
IEEE80211_IS_STYPE(deauth, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_DEAUTH)
IEEE80211_IS_STYPE(action, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_ACTION)
IEEE80211_IS_STYPE(back_req, IEEE80211_FTYPE_CTL, IEEE80211_STYPE_BACK_REQ)
IEEE80211_IS_STYPE(back, IEEE80211_FTYPE_CTL, IEEE80211_STYPE_BACK)
IEEE80211_IS_STYPE(pspoll, IEEE80211_FTYPE_CTL, IEEE80211_STYPE_PSPOLL)
IEEE80211_IS_STYPE(rts, IEEE80211_FTYPE_CTL, IEEE80211_STYPE_RTS)
IEEE80211_IS_STYPE(cts, IEEE80211_FTYPE_CTL, IEEE80211_STYPE_CTS)
IEEE80211_IS_STYPE(ack, IEEE80211_FTYPE_CTL, IEEE80211_STYPE_ACK)
IEEE80211_IS_STYPE(nullfunc, IEEE80211_FTYPE_DATA, IEEE80211_STYPE_NULLFUNC)
IEEE80211_IS_STYPE(qos_nullfunc, IEEE80211_FTYPE_DATA, IEEE80211_STYPE_QOS_NULLFUNC)
#undef IEEE80211_IS_STYPE
static inline bool ieee80211_is_any_nullfunc(__le16 fc) { return ieee80211_is_nullfunc(fc) || ieee80211_is_qos_nullfunc(fc); }
static inline bool ieee80211_is_bufferable_mmpdu(__le16 fc) { return ieee80211_is_mgmt(fc) && (ieee80211_is_action(fc) || ieee80211_is_disassoc(fc) || ieee80211_is_deauth(fc)); }
static inline unsigned int ieee80211_hdrlen(__le16 fc)
{
	unsigned int hdrlen = 24;
	if (ieee80211_is_data(fc)) {
		if (ieee80211_has_a4(fc)) hdrlen = 30;
		if (ieee80211_is_data_qos(fc)) { hdrlen += IEEE80211_QOS_CTL_LEN; if (ieee80211_has_order(fc)) hdrlen += IEEE80211_HT_CTL_LEN; }
	} else if (ieee80211_is_mgmt(fc)) {
		if (ieee80211_has_order(fc)) hdrlen += IEEE80211_HT_CTL_LEN;
	} else if (ieee80211_is_ctl(fc)) {
		hdrlen = (ieee80211_is_cts(fc) || ieee80211_is_ack(fc)) ? 10 : 16;
	}
	return hdrlen;
}
static inline unsigned int ieee80211_get_hdrlen_from_skb(const struct sk_buff *skb) { return ieee80211_hdrlen(((const struct ieee80211_hdr *)skb->data)->frame_control); }
static inline u8 *ieee80211_get_qos_ctl(struct ieee80211_hdr *hdr) { return (u8 *)hdr + (ieee80211_has_a4(hdr->frame_control) ? 30 : 24); }
static inline u8 ieee80211_get_tid(struct ieee80211_hdr *hdr) { return ieee80211_get_qos_ctl(hdr)[0] & IEEE80211_QOS_CTL_TID_MASK; }
static inline u8 *ieee80211_get_SA(struct ieee80211_hdr *hdr) { if (ieee80211_has_a4(hdr->frame_control)) return hdr->addr4; if (ieee80211_has_fromds(hdr->frame_control)) return hdr->addr3; return hdr->addr2; }
static inline u8 *ieee80211_get_DA(struct ieee80211_hdr *hdr) { if (ieee80211_has_tods(hdr->frame_control)) return hdr->addr3; return hdr->addr1; }
static inline bool ieee80211_is_robust_mgmt_frame(struct sk_buff *skb) { (void)skb; return false; }
static inline bool ieee80211_is_public_action(struct ieee80211_hdr *hdr, size_t len) { struct ieee80211_mgmt *m = (struct ieee80211_mgmt *)hdr; if (len < IEEE80211_MIN_ACTION_SIZE) return false; if (!ieee80211_is_action(hdr->frame_control)) return false; return m->u.action.category == 4; }
static inline u16 ieee80211_get_sn(struct ieee80211_hdr *hdr) { return le16_to_cpu(hdr->seq_ctrl) >> 4; }
static inline bool ieee80211_is_first_frag(__le16 seq_ctrl) { return (seq_ctrl & cpu_to_le16(IEEE80211_SCTL_FRAG)) == 0; }
static inline u8 ieee80211_ac_from_tid(int tid) { static const u8 m[8] = { 2, 3, 3, 2, 1, 1, 0, 0 }; return m[tid & 7]; }
static inline bool ieee80211_check_tim(const struct ieee80211_tim_ie *tim, u8 tim_len, u16 aid)
{
	u8 mask, index, indexn1, indexn2; if (!tim || tim_len < sizeof(*tim)) return false;
	aid &= 0x3fff; index = aid / 8; mask = 1 << (aid & 7); indexn1 = tim->bitmap_ctrl & 0xfe; indexn2 = tim_len + indexn1 - 4;
	if (index < indexn1 || index > indexn2)
		return false;
	index -= indexn1;
	return !!(tim->virtual_map[index] & mask);
}
#endif
