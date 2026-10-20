/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* a64wlan_api.h -- OURS (2026-10-10, PinePhone WiFi): the C
 * face of the chip core (rtl8703b/, the fresh RTL8703B driver) for the
 * Symbian side (a64wlan.cpp: the thread + the firmware; a64wlanpdd.cpp: the WHA PDD "wlan.phys" under the real
 * wlanldd). Plain C types only: both languages include it. */
#ifndef A64WLAN_API_H
#define A64WLAN_API_H
#ifdef __cplusplus
extern "C" {
#endif

void a64wlan_set_firmware(const unsigned char *aData, unsigned aSize);	/* rtw8703b_fw.bin (from the ROM) */
void a64wlan_set_wow_firmware(const unsigned char *aData, unsigned aSize);	/* WIFI_DRV=rtw88 only: rtw8703b_wow_fw.bin */
int  a64wlan_probe(void);			/* chip power-on check + the efuse; 0 = ok */
int  a64wlan_is_probed(void);
int  a64wlan_last_cfo_khz(void);	/* carrier offset of the frame in the rx sink right now (OFDM frames), kHz */
void a64wlan_cfo_feed(int khz);		/* CFO tracking: one OFDM frame's offset; steers the crystal cap */
/* the station sequencer (a64wlan_glue.c: rtw88's own ops in mac80211's order) */
int  a64wlan_core_up(void);			/* ieee80211_do_open + hw_conf_init: start, the station interface, ERP/WMM
									   defaults; ends idle = powered off (IPS) as Linux; idempotent; 0 = ok */
int  a64wlan_is_up(void);
int  a64wlan_set_channel(int aChannel);	/* 1..14 (2.4 GHz), 20 MHz: a scan step (or the W2 demo) */
void a64wlan_scan_notify(int aStart);	/* the software scan start (leave IPS, sw_scan_start, filter) / end (operating
									   channel, filter, sw_scan_complete, idle) */
int  a64wlan_join(int aChannel, const unsigned char *aBssid, unsigned aBeaconInterval);	/* WHA Join = prep_connection + mgd_prepare_tx */
void a64wlan_assoc(unsigned aAid, unsigned aDtim);	/* WHA SetBssParameters = assoc_success / set_associated */
void a64wlan_leave(void);			/* WHA Reset = set_disassoc */
void a64wlan_conf_tx(const unsigned short aCwMin[4], const unsigned short aCwMax[4], const unsigned char aAifsn[4],
					 const unsigned short aTxOpUs[4]);	/* WHA ConfigureAC (WHA queue order BE BK VI VO) */
void a64wlan_set_slot_time(unsigned aUs);	/* WHA dot11SlotTime MIB: 9 = short slot */
int  a64wlan_channel(void);
void a64wlan_mac_addr(unsigned char aOut[6]);	/* the efuse MAC (random when the efuse holds none, as Linux does) */
void a64wlan_irq_poll(void);		/* the SDIO "interrupt" (HISR -> RX0_REQ_LEN -> CMD53), polled; a no-op before core up */
void a64wlan_demo_report(void);		/* A64_WLAN_TEST: counters to the log */

/* the security CAM (r8703b_cam.c), WHA AddKey / RemoveKey: aCam = 0..31 (our map: the WHA entry index), aKeyId 0..3, aMac = the
 * key's address (the AP's, for group keys too), aKeyLen 5/13 (WEP) or 16; aGroup = BIT6 of the entry. 0 = ok */
#define A64WLAN_KEY_WEP  1u
#define A64WLAN_KEY_TKIP 2u
#define A64WLAN_KEY_CCMP 3u
int  a64wlan_set_key(unsigned aCam, unsigned aAlgo, unsigned aKeyId, const unsigned char *aMac, const unsigned char *aKey,
					 unsigned aKeyLen, int aGroup);
int  a64wlan_clear_key(unsigned aCam);
void a64wlan_clear_keys(void);		/* the whole CAM (also done at disassociation) */

/* every received frame (802.11, FCS stripped), from the chip core's thread: the WHA PDD's receive path.
 * aRateIdx = the legacy rate index on 2.4 GHz (0..11 = 1 2 5.5 11 6 9 12 18 24 36 48 54 Mbit/s) or the MCS when
 * aFlags has A64WLAN_RX_HT; aFlags: A64WLAN_RX_* below */
#define A64WLAN_RX_HT       1u
#define A64WLAN_RX_BAD_FCS  2u
/* security (wlan-security-design.md section 3.3): only set for protected data frames. DECRYPTED = the hardware decrypted
 * the frame (the IV / CCMP header and the ICV / MIC trailer are STILL in it); SWDEC = it did not (the host would have to);
 * the cipher the hardware identified is in bits 8..10 (A64WLAN_RX_SECTYPE: the A64WLAN_KEYALGO_* codes below).
 * Frames whose hardware ICV / MIC check failed are dropped in the chip core and counted (no flag). */
#define A64WLAN_RX_DECRYPTED 4u
#define A64WLAN_RX_SWDEC    8u
#define A64WLAN_RX_SECTYPE(f)     (((f) >> 8) & 7u)
#define A64WLAN_RX_SECTYPE_SET(s) (((unsigned)(s) & 7u) << 8)
#define A64WLAN_KEYALGO_WEP40  1u
#define A64WLAN_KEYALGO_TKIP   2u
#define A64WLAN_KEYALGO_AES    4u
#define A64WLAN_KEYALGO_WEP104 5u
typedef void (*a64wlan_rx_sink_fn)(void *aCtx, const unsigned char *aFrame, unsigned aLen, int aSignalDbm,
								   unsigned aRateIdx, unsigned aFreqMhz, unsigned aFlags);
void a64wlan_set_rx_sink(a64wlan_rx_sink_fn aFn, void *aCtx);
unsigned a64wlan_rx_frames(void);

/* W3b: transmit. aFrame = the 802.11 frame (header + body, no FCS), aMgmt = 1 for a management frame; the frame is
 * copied, aCookie comes back through the tx sink once the chip has taken the packet (aAcked = 1: written and, when
 * a status report was asked for, acknowledged). 0 = queued; -ENODEV before core up, -ENOMEM/-ENOSPC = dropped. */
int a64wlan_tx(const unsigned char *aFrame, unsigned aLen, unsigned aMgmt, void *aCookie, unsigned aSec);
/* aSec = the hardware encryption of a DATA frame: SEC_NONE; SEC_RC4 (WEP, TKIP) / SEC_AES (CCMP) = the host has built the
 * IV / CCMP header (and TKIP's MIC) already, the chip encrypts with the CAM key of the receiver address and appends the
 * ICV / MIC; SEC_FAIL = the frame is protected but no key exists: it is completed as not sent, never transmitted */
#define A64WLAN_SEC_NONE 0u
#define A64WLAN_SEC_RC4  1u
#define A64WLAN_SEC_AES  3u
#define A64WLAN_SEC_FAIL 0xFFu
typedef void (*a64wlan_tx_sink_fn)(void *aCtx, void *aCookie, int aAcked);
void a64wlan_set_tx_sink(a64wlan_tx_sink_fn aFn, void *aCtx);
unsigned a64wlan_tx_done_count(void);		/* frames the driver reported sent (any outcome) */
void a64wlan_tx_report(void);			/* the chip's TX queue/pause registers + the coex state to the log (bring-up) */
void a64wlan_tx_flush(void);			/* the WLAN thread: queued frames into the chip */
void a64wlan_kick_tx(void);			/* a64wlan.cpp: run a64wlan_tx_flush on the WLAN thread soon */

#ifdef __cplusplus
}
#endif
#endif
