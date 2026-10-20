// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// a64wlanpdd.cpp -- OURS (2026-10-10, PinePhone WiFi W3; docs/phorepoc/pinephone-port.md "WiFi: the chip core"):
// "wlan.phys", the WHA physical device under the REAL 2010 wlanldd (the UMAC), over the rtw88 chip core
// (a64wlan_glue.c / shim_mac80211.c, the vendored Linux driver on the WLAN DFC thread). Nokia's own adaptations
// (the 701's wlanpdd.pdd, TI's WVSS) have this exact shape: a DPhysicalDevice whose channel object is an
// MWlanPddIface handing the UMAC a WHA::Wha.
//
// Threads. The UMAC calls every Wha method on the LDD's thread ("WlanDdThread") holding the LDD mutex; the chip core
// lives on OUR thread ("A64WlanDfc": every rtw88 call, its timers, the polled SDIO interrupt). So a command is
// recorded here and run by a DFC on the WLAN thread; the WHA callbacks (CommandResponse, CommandComplete,
// RequestForBuffer/ReceivePacket, SendPacket*) are made from the WLAN thread under the LDD mutex
// (MWlanOsaExt::MutexAcquire), as TI's PDD does from its DFCs. WriteMib answers synchronously (KSuccess: the UMAC
// completes it through its own DFC), everything else through CommandResponse.
//
// What the half-MAC contract makes the PDD do in software (the chip has no scan/join offload):
//   Scan  = the channels the UMAC lists, one after another, dwelling max(iMaxChannelTime, 110 ms) -- passive for now
//           (no probe request transmitted: the TX path is W3b), beacons/probe responses -> ReceivePacket;
//   Join  = the channel + the BSSID remembered (frames flow to the UMAC from then on); association is the UMAC's.
// Security (docs/phorepoc/wlan-security-design.md; a64wlansec.cpp): AddKey / RemoveKey write the chip's security CAM, the PDD
// advertises KNoSecHdrAndTrailer and does the CCMP / TKIP / WEP header + Michael work itself.
// Holes (recorded in the port doc): the probe template, power save, BSS-lost indication, HT. v1 = scanning.
#include <kernel/kernel.h>
#include <nkern.h>
#include <wlanpddiface.h>
#include <wlanosa.h>
#include <wlanosaext.h>
#include <wlanhpa.h>
#include "a64wlan_api.h"
#include "a64wlansec.h"
extern "C" {
#include "shim/shim_rt.h"
}
extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(TUint32);
extern "C" TDfcQue* A64WlanDfcQ();

using namespace WlanHalApi;

class WlanSpia { };		// the SPI bus class the WHA base binds a reference to; this chip sits on SDIO, never used

// the host platform adaptation: power and the out-of-band interrupt line. The PinePhone has neither to do here --
// the SDIO host (a64sdio.cpp) powers the chip and the interrupt is in-band, polled on the WLAN thread.
class TA64Hpa : public WlanHpa
	{
public:
	TA64Hpa(MWlanOsaExt& aExt, MWlanOsa& aOsa) : WlanHpa(aExt, aOsa) {}
	virtual void PowerOnDevice() {}
	virtual void PowerOffDevice() {}
	virtual void InterruptServiced() {}
	virtual void Configure(const TConfig&) {}
	virtual void EnableIrq() {}
	virtual TBool DebugGpioToggle(TBool) { return EFalse; }
	};

static const TInt KCmdDfcPriority = 2;
static const TUint32 KMinDwellMs = 110;		// a beacon interval: passive listening needs one whole one per channel
static const TUint KMaxScanChannels = 16;
static const TUint KMaxTxPending = 16;

class TA64Wha : public Wha
	{
public:
	TA64Wha(MWlanOsa& aOsa, MWlanOsaExt& aExt, WlanHpa& aHpa, WlanSpia& aSpia);
	// WHA::Wha, the commands (the UMAC's thread)
	virtual void Initialize(const void* aData, TUint32 aLength);
	virtual void Configure(const SConfigureData& aData, SSettings& aSettings);
	virtual TStatus Release(TBool aSynchronous);
	virtual void Scan(TRate aMaxTransmitRate, TBand aBand, TUint8 aNumOfChannels, const SChannels* aChannels,
					  TScanType aScanType, TUint8 aNumOfProbeRequests, TBool aSplitScan, TUint8 aNumOfSSID, const SSSID* aSsid);
	virtual void StopScan();
	virtual void Join(TOperationMode aMode, const TMacAddress& aBSSID, const SSSID& aSSID, TBand aBand, TChannelNumber aChannel,
					  TUint32 aBeaconInterval, TRate aBasicRateSet, TUint16 aAtimWindow, TPreamble aPreambleType, TBool aProbeForJoin);
	virtual void Reset();
	virtual void SetPsMode(TPsMode aPsMode);
	virtual void SetBssParameters(TUint8 aDTIM, TUint16 aAID);
	virtual void ReadMib(TMib aMib);
	virtual TStatus WriteMib(TMib aMib, TUint16 aLength, const void* aData, TBool aMore);
	virtual void AddKey(TKeyType aType, const void* aKey, TUint8 aEntryIndex);
	virtual void RemoveKey(TUint8 aEntryIndex);
	virtual void ConfigureQueue(TQueueId aQueueId, TUint32 aMaxLifeTime, TPsScheme aPsScheme, const SSAPSDConfig& aSAPSDConfig,
								TAckPolicy aAckPolicy, TUint16 aMediumTime);
	virtual void ConfigureAC(TUint16 aCwMin[KNumOfEdcaQueues], TUint16 aCwMax[KNumOfEdcaQueues], TUint8 aAIFS[KNumOfEdcaQueues],
							 TUint16 aTxOplimit[KNumOfEdcaQueues], TUint16 aMaxReceiveLifeTime[KNumOfEdcaQueues]);
	virtual TStatus SendPacket(const void* aFrame, TUint16 aLength, TQueueId aQueueId, TUint8 aTxRateClassId, TRate aMaxTransmitRate,
							   TBool aMore, TPacketId aPacketId, TPowerLevel aPowerLevel, TUint32 aExpiryTime, void* aReserved);
	virtual void Plt(TPltType aType, void* aParams);
	// the chip core's thread
	void OnRx(const TUint8* aFrame, TUint aLen, TInt aDbm, TUint aRateIdx, TUint aFreqMhz, TUint aFlags);
	void Rebind(MWlanOsaExt& aExt) { iExt = &aExt; }
private:
	enum TCmd { ENone, EInitialize, EConfigure, ERelease, EScan, EStopScan, EJoin, EReset, ESetPsMode, ESetBssParams, EReadMib,
				EAddKey, ERemoveKey, EConfigureQueue, EConfigureAC };
	static void CmdDfcFn(TAny* aPtr) { static_cast<TA64Wha*>(aPtr)->RunCmd(); }
	static void ScanDfcFn(TAny* aPtr) { static_cast<TA64Wha*>(aPtr)->ScanStep(); }
	void Post(TCmd aCmd);
	void RunCmd();
	void Respond(TCommandId aId, TStatus aStatus);
	void Respond(TCommandId aId, TStatus aStatus, const UCommandResponseParams& aParams);
	void Complete(TCompleteCommandId aId, TStatus aStatus);
	void ScanStep();
	void ScanFinish(TStatus aStatus);
	void SendProbes();
public:
	void TxDone(TUint aSlot, TBool aAcked);
private:
	void Lock() { if (iExt) iExt->MutexAcquire(); }
	void Unlock() { if (iExt) iExt->MutexRelease(); }
	static const char* CmdName(TCmd aCmd);
	static const char* MibName(TMib aMib);
private:
	MWlanOsaExt* iExt;
	TDfc iCmdDfc;
	NTimer iScanTimer;
	// mac80211's ieee80211_assoc_success: the link is associated (rtw88 BSS_CHANGED_ASSOC -> MGD_LINKED, AID) as the
	// successful (re)association response arrives, before any beacon -- the UMAC's infrastructure-mode init waits for
	// a beacon of the BSS (EWAIT4BEACON, the DTIM) before its SetBssParameters, and the chip delivers the BSS's
	// beacons only once the port is linked (boot 273: associated, then nothing). Deferred to its own DFC: OnRx runs
	// inside rtw88's receive handler.
	TDfc iAssocDfc;
	TBool iAssocSeen;
	static void AssocDfcFn(TAny* aPtr);
	volatile TCmd iCmd;				// the one command in flight (the UMAC serialises them)
	TBool iReleaseSync;
	TPsMode iPsMode;				// the dot11 power-management mode the UMAC last asked for (echoed in the completion)
	// scan
	TBool iScanning;
	TUint iScanCount, iScanIdx;
	SChannels iScanCh[KMaxScanChannels];
	TUint32 iScanDwellMs;
	TUint iScanRx;
	// join
	TBool iJoined;
	TInt iKeyResult;				// AddKey: the parse result, answered once the CAM is written (EAddKey)
	TUint8 iKeyIndex;				// AddKey / RemoveKey: the entry
	TMacAddress iBssid;
	TChannelNumber iJoinChannel;
	TUint32 iBeaconInterval;
	TUint8 iDtim;
	// the UMAC's ConfigureAC (WHA queue order), programmed by the glue at the association
	TUint16 iAcCwMin[KNumOfEdcaQueues], iAcCwMax[KNumOfEdcaQueues], iAcTxOp[KNumOfEdcaQueues];
	TUint8 iAcAifs[KNumOfEdcaQueues];
	// mib
	TMib iReadMib;
	TUint8 iMibBuf[64];
	TRcpi iLastRcpi;
	// tx: each packet copied into an skb for rtw88 (a64wlan_tx), completed from the chip core's thread by the tx sink
	struct TTxSlot { TPacketId iId; TRate iRate; TBool iUsed; };
	TTxSlot iTx[KMaxTxPending];
	TUint iTxCount, iTxDropped, iTxAcked, iTxFailed;
	TUint16 iAid;
	TUint iToUsLogged, iRxCtrl, iProbeRespToUs, iProbeRespFromBss, iUnicastProbes, iRxJoinedAny, iRxJoinedBss, iRxIdle, iTestAuthSent, iTestAuthAnswered;
	TBool iBssidKnown;
	TInt iCfoN, iCfoSum, iCfoMin, iCfoMax, iCfoBssN, iCfoBssSum;		// carrier offset of received OFDM frames, kHz
	// the probe-request template (WriteMib templateFrame, KProbeRequestTemplate) and the current scan's probing
	TUint8 iProbeReq[256];
	TUint16 iProbeReqLen;
	TUint8 iScanProbes;
	SSSID iScanSsid[4];
	TUint iScanSsidCount, iProbesSent, iScanProbeResp;
	// counters
	TUint iRxDelivered, iRxDropped, iRxNoBuffer, iRxCck, iRxOfdm, iRxHt;
	};

static TA64Wha* TheWha;
static TA64Hpa* TheHpa;
static WlanSpia TheSpia;
static TUint ProbeTxOk, ProbeTxFailed;		// the PDD's own probe requests (no cookie): the firmware's report

// ---------------------------------------------------------------- the command DFC
TA64Wha::TA64Wha(MWlanOsa& aOsa, MWlanOsaExt& aExt, WlanHpa& aHpa, WlanSpia& aSpia)
	: Wha(aOsa, aHpa, aSpia), iExt(&aExt),
	  iCmdDfc(CmdDfcFn, this, A64WlanDfcQ(), KCmdDfcPriority),
	  iScanTimer(ScanDfcFn, this, A64WlanDfcQ(), KCmdDfcPriority),
	  iAssocDfc(AssocDfcFn, this, A64WlanDfcQ(), KCmdDfcPriority), iAssocSeen(EFalse),
	  iCmd(ENone), iReleaseSync(EFalse), iScanning(EFalse), iScanCount(0), iScanIdx(0), iScanDwellMs(KMinDwellMs), iScanRx(0),
	  iPsMode(KPsDisable), iJoined(EFalse), iJoinChannel(0), iBeaconInterval(100), iDtim(1), iReadMib(0), iLastRcpi(0), iTxCount(0), iTxDropped(0), iTxAcked(0), iTxFailed(0), iAid(0), iToUsLogged(0), iRxCtrl(0), iProbeRespToUs(0), iProbeRespFromBss(0), iUnicastProbes(0), iRxJoinedAny(0), iRxJoinedBss(0), iRxIdle(0), iTestAuthSent(0), iTestAuthAnswered(0), iBssidKnown(EFalse),
	  iProbeReqLen(0), iScanProbes(0), iScanSsidCount(0), iProbesSent(0), iScanProbeResp(0),
	  iRxDelivered(0), iRxDropped(0), iRxNoBuffer(0), iRxCck(0), iRxOfdm(0), iRxHt(0)
	{
	memclr(&iBssid, sizeof(iBssid));
	iKeyResult = 0; iKeyIndex = 0;
	iCfoN = iCfoSum = iCfoBssN = iCfoBssSum = 0; iCfoMin = 0x7fffffff; iCfoMax = -0x7fffffff;
	memclr(iMibBuf, sizeof(iMibBuf));
	memclr(iTx, sizeof(iTx));
	memclr(iProbeReq, sizeof(iProbeReq));
	memclr(iScanSsid, sizeof(iScanSsid));
	}

const char* TA64Wha::CmdName(TCmd aCmd)
	{
	static const char* const KNames[] = { "none", "Initialize", "Configure", "Release", "Scan", "StopScan", "Join", "Reset", "SetPsMode",
										  "SetBssParameters", "ReadMib", "AddKey", "RemoveKey", "ConfigureQueue", "ConfigureAC" };
	return (TUint)aCmd < sizeof(KNames) / sizeof(KNames[0]) ? KNames[aCmd] : "?";
	}

const char* TA64Wha::MibName(TMib aMib)
	{
	switch (aMib)
		{
		case KMibDot11StationId: return "dot11StationId";
		case KMibDot11MaxReceiveLifetime: return "dot11MaxReceiveLifetime";
		case KMibDot11SlotTime: return "dot11SlotTime";
		case KMibDot11GroupAddressesTable: return "dot11GroupAddressesTable";
		case KMibDot11WepDefaultKeyId: return "dot11WepDefaultKeyId";
		case KMibDot11CurrentTxPowerLevel: return "dot11CurrentTxPowerLevel";
		case KMibDot11RTSThreshold: return "dot11RTSThreshold";
		case KMibCtsToSelf: return "ctsToSelf";
		case KMibArpIpAddressTable: return "arpIpAddressTable";
		case KMibTemplateFrame: return "templateFrame";
		case KMibRxFilter: return "rxFilter";
		case KMibBeaconFilterIeTable: return "beaconFilterIeTable";
		case KMibBeaconFilterEnable: return "beaconFilterEnable";
		case KMibSleepMode: return "sleepMode";
		case KMibWlanWakeUpInterval: return "wlanWakeUpInterval";
		case KMibBeaconLostCount: return "beaconLostCount";
		case KMibRcpiThreshold: return "rcpiThreshold";
		case KMibStatisticsTable: return "statisticsTable";
		case KMibTxRatePolicy: return "txRatePolicy";
		case KMibCountersTable: return "countersTable";
		case KMibDot11Preamble: return "dot11Preamble";
		case KMibEtherTypeFilter: return "etherTypeFilter";
		case KMibBroadcastUdpFilter: return "broadcastUdpFilter";
		case KMibHtCapabilities: return "htCapabilities";
		case KMibHtBssOperation: return "htBssOperation";
		case KMibHtSecondaryBeacon: return "htSecondaryBeacon";
		case KMibHtBlockAckConfigure: return "htBlockAckConfigure";
		case KMibTxAutoRatePolicy: return "txAutoRatePolicy";
		default: return "?";
		}
	}

void TA64Wha::Post(TCmd aCmd)
	{
	if (iCmd != ENone)
		shim_log("[wha] WARNING: %s posted while %s still runs", CmdName(aCmd), CmdName(iCmd));
	iCmd = aCmd;
	iCmdDfc.Enque();
	}

void TA64Wha::Respond(TCommandId aId, TStatus aStatus)
	{
	UCommandResponseParams none;
	memclr(&none, sizeof(none));
	Respond(aId, aStatus, none);
	}

void TA64Wha::Respond(TCommandId aId, TStatus aStatus, const UCommandResponseParams& aParams)
	{
	Lock();
	WhaCb().CommandResponse(aId, aStatus, aParams);
	Unlock();
	}

void TA64Wha::Complete(TCompleteCommandId aId, TStatus aStatus)
	{
	UCommandCompletionParams p;
	memclr(&p, sizeof(p));
	/* the same first byte for EScanComplete and ESetPsModeComplete: the CURRENT dot11 power-management mode. It MUST be
	 * the mode the UMAC asked for: WlanDot11PwrMgmtTransitionMode re-issues SetPsMode for as long as current != desired
	 * (boot 255: an endless SetPsMode(1) loop once the engine wanted PS after the join attempt -- the WLAN thread +
	 * the serial log pinned two cores and flooded the debug link). The chip itself stays awake (no LPS yet). */
	p.iScanComplete.iDot11PowerManagementMode = iPsMode;
	Lock();
	WhaCb().CommandComplete(aId, aStatus, p);
	Unlock();
	}

// on the WLAN thread
void TA64Wha::RunCmd()
	{
	const TCmd cmd = iCmd;
	iCmd = ENone;
	switch (cmd)
		{
		case EInitialize:
			{
			TInt r = a64wlan_is_probed() ? a64wlan_core_up() : -19;
			shim_log("[wha] Initialize: chip core up r=%d", r);
			Respond(EInitializeResponse, r ? KFailed : KSuccess);
			break;
			}
		case EConfigure:
			Respond(EConfigureResponse, KSuccess);
			break;
		case ERelease:
			Respond(EReleaseResponse, KSuccess);
			break;
		case EScan:
			Respond(EScanCommandResponse, KSuccess);
			iScanning = ETrue;
			iScanIdx = 0;
			iScanRx = 0;
			a64wlan_scan_notify(1);		// what mac80211's sw_scan_start tells the driver (coex, firmware, DIG)
			ScanStep();
			break;
		case EStopScan:
			Respond(EStopScanResponse, KSuccess);
			if (iScanning)
				ScanFinish(KSuccess);
			break;
		case EJoin:
			{
			// mac80211's prep_connection + mgd_prepare_tx (channel, BSSID, the AP as a station, IQK)
			TInt r = a64wlan_join((int)iJoinChannel, iBssid.iMacAddress, (unsigned)iBeaconInterval);
			iJoined = (r == 0);
			iAssocSeen = EFalse;
			UCommandResponseParams p;
			memclr(&p, sizeof(p));
			p.iJoinResponse.iMinPowerLevel = 0;
			p.iJoinResponse.iMaxPowerLevel = 20;
			shim_log("[wha] Join: channel %u set r=%d", (unsigned)iJoinChannel, r);
			Respond(EJoinResponse, r ? KFailed : KSuccess, p);
			break;
			}
		case EReset:
			iJoined = EFalse;
			iAssocSeen = EFalse;
			iAssocDfc.Cancel();
			if (iScanning) { iScanning = EFalse; iScanTimer.Cancel(); a64wlan_scan_notify(0); }
			a64wlan_leave();		// mac80211's set_disassoc: the station removed, idle again
			A64SecReset();			// ... and the keys with it (the chip core cleared the CAM)
			Respond(EResetResponse, KSuccess);
			break;
		case ESetPsMode:
			Respond(ESetPsModeCommandResponse, KSuccess);
			Complete(ESetPsModeComplete, KSuccess);
			break;
		case ESetBssParams:
			a64wlan_assoc(iAid, iDtim);		// mac80211's set_associated: EDCA, linked, AID, reserved pages
			Respond(ESetBssParametersResponse, KSuccess);
			break;
		case EReadMib:
			{
			UCommandResponseParams p;
			memclr(&p, sizeof(p));
			memclr(iMibBuf, sizeof(iMibBuf));
			TStatus st = KSuccess;
			TUint16 len = 0;
			switch (iReadMib)
				{
				case KMibDot11StationId:
					a64wlan_mac_addr(reinterpret_cast<Sdot11StationId*>(iMibBuf)->iAddr.iMacAddress);
					len = sizeof(Sdot11StationId);
					break;
				case KMibStatisticsTable:
					reinterpret_cast<SstatisticsTable*>(iMibBuf)->iRcpi = iLastRcpi;
					len = sizeof(SstatisticsTable);
					break;
				case KMibCountersTable:
					len = sizeof(ScountersTable);
					break;
				default:
					st = KFailed;
					break;
				}
			p.iReadMibResponse.iMib = iReadMib;
			p.iReadMibResponse.iLength = len;
			p.iReadMibResponse.iData = iMibBuf;
			shim_log("[wha] ReadMib %s (0x%04x) -> %s, %u bytes", MibName(iReadMib), (unsigned)iReadMib, st == KSuccess ? "ok" : "FAILED", (unsigned)len);
			Respond(EReadMIBResponse, st, p);
			break;
			}
		case EAddKey:
			{
			// the CAM write (design 3.1, 4.3) on this thread; the key struct was parsed and copied by AddKey
			TInt r = iKeyResult ? iKeyResult : A64SecCommit(iKeyIndex);
			shim_log("[wha] AddKey index %u: CAM r=%d", (unsigned)iKeyIndex, r);
			Respond(EAddKeyResponse, r ? KFailed : KSuccess);
			break;
			}
		case ERemoveKey:
			A64SecRemoveKey(iKeyIndex);
			Respond(ERemoveKeyResponse, KSuccess);
			break;
		case EConfigureQueue:
			Respond(EConfigureQueueResponse, KSuccess);
			break;
		case EConfigureAC:
			a64wlan_conf_tx(iAcCwMin, iAcCwMax, iAcAifs, iAcTxOp);
			Respond(EConfigureACResponse, KSuccess);
			break;
		default:
			break;
		}
	}

// ---------------------------------------------------------------- scan, a channel per timer tick
void TA64Wha::ScanStep()
	{
	if (!iScanning)
		return;
	if (iScanIdx >= iScanCount)
		{
		ScanFinish(KSuccess);
		return;
		}
	const SChannels& c = iScanCh[iScanIdx++];
	TInt r = a64wlan_set_channel((int)c.iChannel);
	if (r)
		shim_log("[wha] scan: channel %u FAILED r=%d", (unsigned)c.iChannel, r);
	else if (iScanProbes && iProbeReqLen)
		SendProbes();			// an active scan: the UMAC's probe-request template, per SSID, aNumOfProbeRequests times
	TUint32 dwell = c.iMaxChannelTime > KMinDwellMs ? c.iMaxChannelTime : KMinDwellMs;
	iScanTimer.OneShot(NKern::TimerTicks((TInt)dwell));
	}

void TA64Wha::ScanFinish(TStatus aStatus)
	{
	iScanning = EFalse;
	iScanTimer.Cancel();
	a64wlan_tx_report();			// (before the scan end: an idle chip is powered off)
	a64wlan_scan_notify(0);			// sw_scan_complete, back to the operating channel or idle
	shim_log("[wha] scan done: %u channels, %u probes sent (%u tx completions so far), %u frames to the UMAC of which %u probe responses (dropped %u, no buffer %u)",
			 iScanCount, iProbesSent, a64wlan_tx_done_count(), iScanRx, iScanProbeResp, iRxDropped, iRxNoBuffer);
	shim_log("[wha] rx rates: CCK %u OFDM %u HT %u", iRxCck, iRxOfdm, iRxHt);
	if (iCfoN)
		shim_log("[wha] carrier offset of received OFDM frames: %d frames, mean %d kHz (min %d max %d); from the joined BSS: %d frames, mean %d kHz",
				 iCfoN, iCfoSum / iCfoN, iCfoMin, iCfoMax, iCfoBssN, iCfoBssN ? iCfoBssSum / iCfoBssN : 0);
	shim_log("[wha] tx: probes reported ok %u / failed %u; the UMAC's packets acked %u failed %u (%u sent, %u dropped); %u control frames seen, %u probe responses to us (%u from the joined BSS; %u of all with RETRY = our ACK missed); rx while joined %u (%u from the BSS), idle %u; test auths to other APs %u, answered %u",
			 ProbeTxOk, ProbeTxFailed, iTxAcked, iTxFailed, iTxCount, iTxDropped, iRxCtrl, iProbeRespToUs, iProbeRespFromBss, iUnicastProbes,
			 iRxJoinedAny, iRxJoinedBss, iRxIdle, iTestAuthSent, iTestAuthAnswered);
	Complete(EScanComplete, aStatus);
	}

// the template = the 24-byte header + the IEs, the first IE the SSID (empty in the UMAC's template): a directed scan
// puts each requested SSID there; a broadcast scan sends it as is
void TA64Wha::SendProbes()
	{
	TUint8 f[320];
	if (iProbeReqLen < 26)
		return;
	const TUint tmplSsidLen = iProbeReq[25];
	if (iProbeReqLen < 26 + tmplSsidLen)
		return;
	const TUint8* rest = iProbeReq + 26 + tmplSsidLen;
	const TUint restLen = iProbeReqLen - 26 - tmplSsidLen;
	const TUint n = iScanSsidCount ? iScanSsidCount : 1;
	for (TUint s = 0; s < n; ++s)
		{
		TUint len = 24;
		memcpy(f, iProbeReq, 24);
		const TUint ssidLen = iScanSsidCount ? (iScanSsid[s].iSSIDLength > 32 ? 32 : iScanSsid[s].iSSIDLength) : 0;
		f[len++] = 0;
		f[len++] = (TUint8)ssidLen;
		if (ssidLen) { memcpy(f + len, iScanSsid[s].iSSID, ssidLen); len += ssidLen; }
		if (len + restLen > sizeof(f)) return;
		memcpy(f + len, rest, restLen); len += restLen;
		// (the W4 bring-up tests that were here -- unicast probes to the joined BSS, test authentications to the
		// user's other APs during every dwell -- are gone: the ASUS main BSSIDs share the radio with the "Test"
		// guest BSS, so our MAC was authenticated on one BSS of the AP while joining another; Linux sends neither)
		for (TUint i = 0; i < iScanProbes; ++i)
			if (a64wlan_tx(f, len, 1, 0, A64WLAN_SEC_NONE) == 0)
				++iProbesSent;
		}
	}

// ---------------------------------------------------------------- receive (the chip core's thread)
static TRcpi DbmToRcpi(TInt aDbm)
	{
	TInt v = (aDbm + 110) * 2;		// 802.11k RCPI: 0 = -110 dBm, 220 = 0 dBm
	if (v < 0) v = 0;
	if (v > 220) v = 220;
	return (TRcpi)v;
	}

static TRate RateFromIdx(TUint aIdx, TUint aFlags)
	{
	static const TRate KLegacy[12] = { KRate1Mbits, KRate2Mbits, KRate5_5Mbits, KRate11Mbits, KRate6Mbits, KRate9Mbits,
									   KRate12Mbits, KRate18Mbits, KRate24Mbits, KRate36Mbits, KRate48Mbits, KRate54Mbits };
	if (aFlags & A64WLAN_RX_HT)
		return KRate54Mbits;		// no HT advertised to the UMAC: an MCS frame is reported as the top legacy rate
	return aIdx < 12 ? KLegacy[aIdx] : KRate1Mbits;
	}

static TChannelNumber ChannelFromFreq(TUint aMhz)
	{
	if (aMhz == 2484) return 14;
	if (aMhz >= 2412 && aMhz <= 2472) return (aMhz - 2407) / 5;
	return 0;
	}

void TA64Wha::OnRx(const TUint8* aFrame, TUint aLen, TInt aDbm, TUint aRateIdx, TUint aFreqMhz, TUint aFlags)
	{
	if (aLen < 10 || !iWhaCb)		// an ACK is 10 bytes (control frames are logged below, never delivered)
		return;
	// what rates do we receive at all? (boot 255: is OFDM reception alive, given OFDM transmission is not)
	if (aFlags & A64WLAN_RX_HT) ++iRxHt; else if (aRateIdx >= 4) ++iRxOfdm; else ++iRxCck;
	const TUint8 fc0 = aFrame[0];
	const TBool mgmt = (fc0 & 0x0c) == 0x00;
	const TUint8 subtype = (fc0 >> 4) & 0x0f;
	const TBool beaconLike = mgmt && (subtype == 8 || subtype == 5);		// beacon, probe response
	// W4 bring-up: every frame addressed to us other than a probe response, whatever the state -- the AP's ACK
	// (control frame, subtype 0xd, 10 bytes: fc, duration, RA) to our authentication request included (RCR ACF)
	const TBool ctrl = (fc0 & 0x0c) == 0x04;
	// (a probe response to OUR MAC = our probe request reached the air; the scan counter also counts responses to
	// other stations' requests, the RCR accepts every unicast frame)
	if (iToUsLogged < 64)
		{
		TUint8 mac[6];
		a64wlan_mac_addr(mac);
		if (aLen >= 10 && memcompare(aFrame + 4, 6, mac, 6) == 0)
			{
			if (mgmt && subtype == 5)
				{
				if (iBssidKnown && memcompare(aFrame + 10, 6, iBssid.iMacAddress, 6) == 0) ++iProbeRespFromBss;
				if (aFrame[1] & 0x08) ++iUnicastProbes;		// (reused: the AP retransmitted = it heard no ACK from us)
				if (++iProbeRespToUs <= 3)		// (not against the cap: they come by the dozen)
					shim_log("[wha] rx to us: PROBE RESPONSE from %02x:%02x:%02x:%02x:%02x:%02x, %d dBm rate idx %u%s (our probe request was on the air)",
							 aFrame[10], aFrame[11], aFrame[12], aFrame[13], aFrame[14], aFrame[15], aDbm, aRateIdx, (aFlags & A64WLAN_RX_HT) ? " HT" : "");
				}
			else if (mgmt && subtype == 0xb && (++iTestAuthAnswered, ++iToUsLogged))
				shim_log("[wha] rx to us: AUTHENTICATION from %02x:%02x:%02x:%02x:%02x:%02x, %u bytes, %d dBm rate idx %u%s%s: alg %u seq %u status %u",
						 aFrame[10], aFrame[11], aFrame[12], aFrame[13], aFrame[14], aFrame[15], aLen, aDbm, aRateIdx, (aFlags & A64WLAN_RX_HT) ? " HT" : "",
						 (aFrame[1] & 0x08) ? " RETRY" : "",		// the AP's retransmission = it heard no ACK from us
						 aLen >= 30 ? aFrame[24] | (aFrame[25] << 8) : 0, aLen >= 30 ? aFrame[26] | (aFrame[27] << 8) : 0,
						 aLen >= 30 ? aFrame[28] | (aFrame[29] << 8) : 0);
			else if (++iToUsLogged && ctrl)
				shim_log("[wha] rx to us: CONTROL subtype 0x%x (0xd = ACK), %u bytes, %d dBm rate idx %u%s", (unsigned)subtype, aLen, aDbm,
						 aRateIdx, (aFlags & A64WLAN_RX_HT) ? " HT" : "");
			else
				shim_log("[wha] rx to us: type %u subtype 0x%x from %02x:%02x:%02x:%02x:%02x:%02x, %u bytes, %d dBm (scanning %d joined %d)",
						 (unsigned)((fc0 >> 2) & 3), (unsigned)subtype, aFrame[10], aFrame[11], aFrame[12], aFrame[13], aFrame[14], aFrame[15],
						 aLen, aDbm, (int)iScanning, (int)iJoined);
			// the first data frames to us in hex (first DHCP bring-up: is it the offer, and in what form)
			static TUint dataDumps = 0;
			if (((fc0 >> 2) & 3) == 2 && dataDumps < 6)
				{
				++dataDumps;
				char hex[3 * 96 + 1];
				const TUint n = aLen < 96 ? aLen : 96;
				for (TUint i = 0; i < n; ++i)
					{
					static const char* d = "0123456789abcdef";
					hex[3 * i] = d[aFrame[i] >> 4]; hex[3 * i + 1] = d[aFrame[i] & 15]; hex[3 * i + 2] = ' ';
					}
				hex[3 * n] = 0;
				shim_log("[wha]   rx data: %s", hex);
				}
			}
		}
	// the carrier offset between each sender and us (OFDM frames: the PHY status CFO tail): our crystal runs on
	// rtw88's blank-efuse default 0x20; a large offset = the AP decodes us only sometimes
	if (aRateIdx >= 4 || (aFlags & A64WLAN_RX_HT))
		{
		const TInt cfo = a64wlan_last_cfo_khz();
		++iCfoN; iCfoSum += cfo;
		if (cfo < iCfoMin) iCfoMin = cfo;
		if (cfo > iCfoMax) iCfoMax = cfo;
		if (aLen >= 16 && iBssidKnown && memcompare(aFrame + 10, 6, iBssid.iMacAddress, 6) == 0)
			{ ++iCfoBssN; iCfoBssSum += cfo; }
		a64wlan_cfo_feed(cfo);
		if (iCfoN % 4 == 0)		// the window just closed: restart the per-scan stats on the new crystal cap
			{ iCfoMin = 0x7fffffff; iCfoMax = -0x7fffffff; }
		}
	if (ctrl) ++iRxCtrl;		// (any address: does RCR ACF deliver control frames at all?)
	if (ctrl || aLen < 24) { ++iRxDropped; return; }		// control frames are never the UMAC's
	if (iScanning)
		{
		if (!beaconLike) { ++iRxDropped; return; }
		if (subtype == 5) ++iScanProbeResp;
		}
	else if (iJoined)
		{
		++iRxJoinedAny;		// (is the receiver live at all between scans? boot 242 question)
		if (memcompare(aFrame + 16, 6, iBssid.iMacAddress, 6) == 0) ++iRxJoinedBss;
		// our BSS only, beacons included (boot 234: every beacon in the air went to the UMAC and its RX buffers ran out
		// -- "no buffer" climbing -- while it waited for the authentication response); addr3 = BSSID in a from-AP frame
		if (memcompare(aFrame + 16, 6, iBssid.iMacAddress, 6) != 0 && memcompare(aFrame + 10, 6, iBssid.iMacAddress, 6) != 0)
			{ ++iRxDropped; return; }
		if (mgmt && (subtype == 0xb || subtype == 0x1 || subtype == 0x3 || subtype == 0xa || subtype == 0xc))
			shim_log("[wha] rx mgmt subtype 0x%x from %02x:%02x:%02x:%02x:%02x:%02x to %02x:%02x:%02x:%02x:%02x:%02x, %u bytes, %d dBm",
					 (unsigned)subtype, aFrame[10], aFrame[11], aFrame[12], aFrame[13], aFrame[14], aFrame[15],
					 aFrame[4], aFrame[5], aFrame[6], aFrame[7], aFrame[8], aFrame[9], aLen, aDbm);
		// a successful (re)association response from the joined BSS: status (body +2) 0, AID (body +4)
		if (mgmt && (subtype == 0x1 || subtype == 0x3) && aLen >= 30 && !iAssocSeen &&
			memcompare(aFrame + 10, 6, iBssid.iMacAddress, 6) == 0 && (aFrame[26] | (aFrame[27] << 8)) == 0)
			{
			iAssocSeen = ETrue;
			iAid = (TUint16)((aFrame[28] | (aFrame[29] << 8)) & 0x3fff);
			iAssocDfc.Enque();
			}
		}
	else
		{
		++iRxIdle;
		++iRxDropped;
		return;
		}
	const TRcpi rcpi = DbmToRcpi(aDbm);
	if (iJoined && beaconLike && memcompare(aFrame + 16, 6, iBssid.iMacAddress, 6) == 0)
		iLastRcpi = rcpi;
	// a protected data frame the chip decrypted: replay check, Michael, IV / trailer off, the KEncrypt* flags (a64wlansec.cpp)
	static TUint8 secBuf[2400];
	TUint32 whaFlags = KEncryptNone;
	TUint outLen = aLen;
	const TUint8* outFrame = aFrame;
	const TInt sr = A64SecRx(aFrame, aLen, aFlags, secBuf, outLen, whaFlags);
	if (sr < 0)
		{ ++iRxDropped; return; }
	if (sr > 0)
		outFrame = secBuf;
	Lock();
	TAny* buf = WhaCb().RequestForBuffer((TUint16)outLen);
	if (buf)
		{
		memcpy(buf, outFrame, outLen);			// iRxoffset = 0: the frame starts the buffer
		WhaCb().ReceivePacket(KSuccess, buf, (TUint16)outLen, RateFromIdx(aRateIdx, aFlags), rcpi, ChannelFromFreq(aFreqMhz), buf, whaFlags);
		++iRxDelivered;
		if (iScanning) ++iScanRx;
		}
	else
		{
		++iRxNoBuffer;
		TUint8 mac[6];
		a64wlan_mac_addr(mac);
		if (memcompare(aFrame + 4, 6, mac, 6) == 0)
			shim_log("[wha] NO BUFFER for a frame TO US: fc=%02x%02x %u bytes (no-buffer %u so far)", aFrame[0], aFrame[1], aLen, iRxNoBuffer);
		}
	Unlock();
	}

void TA64Wha::AssocDfcFn(TAny* aPtr)
	{
	TA64Wha* w = static_cast<TA64Wha*>(aPtr);
	if (!w->iJoined)
		return;
	shim_log("[wha] association response: success, AID %u -> the link associated (mac80211's assoc_success)", (unsigned)w->iAid);
	a64wlan_assoc(w->iAid, w->iDtim);
	}

extern "C" void a64wlan_pdd_rx(void* aCtx, const unsigned char* aFrame, unsigned aLen, int aDbm, unsigned aRateIdx, unsigned aFreq, unsigned aFlags)
	{
	static_cast<TA64Wha*>(aCtx)->OnRx(aFrame, aLen, aDbm, aRateIdx, aFreq, aFlags);
	}

// ---------------------------------------------------------------- transmit completion (the chip core's thread)
void TA64Wha::TxDone(TUint aSlot, TBool aAcked)
	{
	if (aSlot >= KMaxTxPending || !iTx[aSlot].iUsed)
		return;
	const TPacketId id = iTx[aSlot].iId;
	const TRate rate = iTx[aSlot].iRate;
	iTx[aSlot].iUsed = EFalse;
	if (aAcked) ++iTxAcked; else ++iTxFailed;
	if (iTxAcked + iTxFailed <= 40)
		shim_log("[wha] tx done: packet %u %s", (unsigned)id, aAcked ? "ACKED" : "NOT acked");
	Lock();
	WhaCb().SendPacketComplete(aAcked ? KSuccess : KErrorRetryExceeded, id, rate, 0, 0, aAcked ? 0 : 1);
	Unlock();
	}

extern "C" void a64wlan_pdd_tx_done(void* aCtx, void* aCookie, int aAcked)
	{
	if (aCookie)
		static_cast<TA64Wha*>(aCtx)->TxDone((TUint)(TLinAddr)aCookie - 1, aAcked != 0);
	else if (aAcked)
		++ProbeTxOk;
	else
		++ProbeTxFailed;
	}

// ---------------------------------------------------------------- the commands (the UMAC's thread)
void TA64Wha::Initialize(const void* aData, TUint32 aLength)
	{
	shim_log("[wha] Initialize(fw %u bytes from wlanhwinit; ours comes from Z:)", (unsigned)aLength);
	(void)aData;
	Post(EInitialize);
	}

void TA64Wha::Configure(const SConfigureData& aData, SSettings& aSettings)
	{
	(void)aData;
	memclr(&aSettings, sizeof(aSettings));
	aSettings.iNumOfSSIDs = 1;
	aSettings.iNumOfBytesForSsid = SSSID::KMaxSSIDLength;
	aSettings.iRates = KDot11gRates;
	aSettings.iBand = KBand2dot4GHzMask;
	aSettings.iRxoffset = 0;
	aSettings.iNumOfGroupTableEntrys = 8;
	aSettings.iNumOfTxRateClasses = 4;
	aSettings.iTxFrameTrailerSpace = 0;		// (KNoSecHdrAndTrailer: no IV / ICV space on either side, see a64wlansec.cpp)
	aSettings.iTxFrameHeaderSpace = 0;
	aSettings.iTxPowerRange[0].iMinPowerLevel = 0;
	aSettings.iTxPowerRange[0].iMaxPowerLevel = 20;
	aSettings.iTxPowerRange[0].iStepping = 1;
	aSettings.iCapability = SSettings::KAutonomousRateAdapt | SSettings::KScanChannelTimes | SSettings::KDot11SlotTime |
		SSettings::KNoSecHdrAndTrailer;	// ONLY because a64wlansec.cpp builds / strips the headers and trailers (design 3.5)
	shim_log("[wha] Configure: 11g, 2.4 GHz, capability 0x%08x", (unsigned)aSettings.iCapability);
	Post(EConfigure);
	}

TStatus TA64Wha::Release(TBool aSynchronous)
	{
	shim_log("[wha] Release(%s)", aSynchronous ? "sync" : "async");
	iJoined = EFalse;
	if (aSynchronous)
		return KSuccess;
	Post(ERelease);
	return KPending;
	}

void TA64Wha::Scan(TRate aMaxTransmitRate, TBand aBand, TUint8 aNumOfChannels, const SChannels* aChannels, TScanType aScanType,
				   TUint8 aNumOfProbeRequests, TBool aSplitScan, TUint8 aNumOfSSID, const SSSID* aSsid)
	{
	(void)aMaxTransmitRate; (void)aBand; (void)aSplitScan;
	iScanCount = aNumOfChannels > KMaxScanChannels ? KMaxScanChannels : aNumOfChannels;
	for (TUint i = 0; i < iScanCount; ++i)
		iScanCh[i] = aChannels[i];
	iScanProbes = aNumOfProbeRequests;
	iScanSsidCount = aSsid ? (aNumOfSSID > 4 ? 4 : aNumOfSSID) : 0;
	for (TUint i = 0; i < iScanSsidCount; ++i)
		iScanSsid[i] = aSsid[i];
	iProbesSent = 0;
	iScanProbeResp = 0;
	shim_log("[wha] Scan: type %d, %u channels (first %u, dwell %u..%u ms), %u probes, %u SSIDs%s%.*s%s",
			 (int)aScanType, (unsigned)aNumOfChannels, iScanCount ? (unsigned)iScanCh[0].iChannel : 0u,
			 iScanCount ? (unsigned)iScanCh[0].iMinChannelTime : 0u, iScanCount ? (unsigned)iScanCh[0].iMaxChannelTime : 0u,
			 (unsigned)aNumOfProbeRequests, (unsigned)aNumOfSSID,
			 aNumOfSSID && aSsid && aSsid[0].iSSIDLength ? " \"" : "",
			 aNumOfSSID && aSsid ? (int)(aSsid[0].iSSIDLength > 32 ? 32 : aSsid[0].iSSIDLength) : 0,
			 aNumOfSSID && aSsid ? (const char*)aSsid[0].iSSID : "",
			 aNumOfSSID && aSsid && aSsid[0].iSSIDLength ? "\"" : "");
	Post(EScan);
	}

void TA64Wha::StopScan()
	{
	shim_log("[wha] StopScan");
	Post(EStopScan);
	}

void TA64Wha::Join(TOperationMode aMode, const TMacAddress& aBSSID, const SSSID& aSSID, TBand aBand, TChannelNumber aChannel,
				   TUint32 aBeaconInterval, TRate aBasicRateSet, TUint16 aAtimWindow, TPreamble aPreambleType, TBool aProbeForJoin)
	{
	(void)aBand; (void)aAtimWindow; (void)aPreambleType; (void)aProbeForJoin;
	iBssid = aBSSID;
	A64SecSetBssid(aBSSID.iMacAddress);
	iBssidKnown = ETrue;
	iJoinChannel = aChannel;
	iBeaconInterval = aBeaconInterval;
	shim_log("[wha] Join: %s bssid=%02x:%02x:%02x:%02x:%02x:%02x ssid=\"%.*s\" ch=%u beacon=%u basic rates=0x%04x",
			 aMode == EBSS ? "BSS" : "IBSS", aBSSID.iMacAddress[0], aBSSID.iMacAddress[1], aBSSID.iMacAddress[2],
			 aBSSID.iMacAddress[3], aBSSID.iMacAddress[4], aBSSID.iMacAddress[5],
			 (int)(aSSID.iSSIDLength > 32 ? 32 : aSSID.iSSIDLength), (const char*)aSSID.iSSID,
			 (unsigned)aChannel, (unsigned)aBeaconInterval, (unsigned)aBasicRateSet);
	Post(EJoin);
	}

void TA64Wha::Reset()
	{
	shim_log("[wha] Reset");
	Post(EReset);
	}

void TA64Wha::SetPsMode(TPsMode aPsMode)
	{
	shim_log("[wha] SetPsMode(%u) -- echoed, the chip stays awake", (unsigned)aPsMode);
	iPsMode = aPsMode;
	Post(ESetPsMode);
	}

void TA64Wha::SetBssParameters(TUint8 aDTIM, TUint16 aAID)
	{
	shim_log("[wha] SetBssParameters: DTIM %u AID %u", (unsigned)aDTIM, (unsigned)aAID);
	iAid = aAID;
	iDtim = aDTIM;
	Post(ESetBssParams);
	}

void TA64Wha::ReadMib(TMib aMib)
	{
	iReadMib = aMib;
	Post(EReadMib);
	}

TStatus TA64Wha::WriteMib(TMib aMib, TUint16 aLength, const void* aData, TBool aMore)
	{
	(void)aMore;
	switch (aMib)
		{
		case KMibTemplateFrame:
			{
			const StemplateFrame* t = static_cast<const StemplateFrame*>(aData);
			const TUint8* d = t->iTemplateData;
			shim_log("[wha] WriteMib templateFrame type %u, %u bytes, rate 0x%x: fc=%02x%02x da=%02x:%02x:%02x:%02x:%02x:%02x sa=%02x:%02x:%02x:%02x:%02x:%02x",
					 (unsigned)t->iFrameType, (unsigned)t->iLength, (unsigned)t->iInitialTransmitRate, d[0], d[1],
					 d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
			if (t->iFrameType == KProbeRequestTemplate && t->iLength >= 24 && t->iLength <= sizeof(iProbeReq))
				{
				memcpy(iProbeReq, d, t->iLength);
				iProbeReqLen = (TUint16)t->iLength;
				}
			break;
			}
		case KMibRxFilter:
			shim_log("[wha] WriteMib rxFilter 0x%08x", (unsigned)static_cast<const SrxFilter*>(aData)->iRxFilter);
			break;
		case KMibDot11SlotTime:
			shim_log("[wha] WriteMib dot11SlotTime %u us", (unsigned)static_cast<const Sdot11SlotTime*>(aData)->iDot11SlotTime);
			a64wlan_set_slot_time((unsigned)static_cast<const Sdot11SlotTime*>(aData)->iDot11SlotTime);	// stored only
			break;
		case KMibDot11WepDefaultKeyId:
			shim_log("[wha] WriteMib dot11WepDefaultKeyId %u", (unsigned)static_cast<const Sdot11WepDefaultKeyId*>(aData)->iDot11WepDefaultKeyId);
			A64SecSetWepTxKeyId(static_cast<const Sdot11WepDefaultKeyId*>(aData)->iDot11WepDefaultKeyId);
			break;
		case KMibDot11GroupAddressesTable:
			shim_log("[wha] WriteMib dot11GroupAddressesTable enable=%d n=%u", (int)static_cast<const Sdot11GroupAddressesTable*>(aData)->iEnable,
					 (unsigned)static_cast<const Sdot11GroupAddressesTable*>(aData)->iNumOfAddrs);
			break;
		default:
			shim_log("[wha] WriteMib %s (0x%04x), %u bytes", MibName(aMib), (unsigned)aMib, (unsigned)aLength);
			break;
		}
	return KSuccess;		// synchronous: the UMAC completes it from its own DFC
	}

void TA64Wha::AddKey(TKeyType aType, const void* aKey, TUint8 aEntryIndex)
	{
	// the UMAC's key struct is only valid during this call: parsed and copied into the security table now, the CAM
	// written by the command DFC (the chip is the WLAN thread's)
	iKeyIndex = aEntryIndex;
	iKeyResult = A64SecAddKey(aType, aKey, aEntryIndex);
	shim_log("[wha] AddKey type %d index %u: parse r=%d", (int)aType, (unsigned)aEntryIndex, iKeyResult);
	Post(EAddKey);
	}

void TA64Wha::RemoveKey(TUint8 aEntryIndex)
	{
	shim_log("[wha] RemoveKey index %u", (unsigned)aEntryIndex);
	iKeyIndex = aEntryIndex;
	Post(ERemoveKey);
	}

void TA64Wha::ConfigureQueue(TQueueId aQueueId, TUint32 aMaxLifeTime, TPsScheme aPsScheme, const SSAPSDConfig& aSAPSDConfig, TAckPolicy aAckPolicy, TUint16 aMediumTime)
	{
	(void)aSAPSDConfig; (void)aMediumTime;
	shim_log("[wha] ConfigureQueue %d: lifetime %u ps %d ack %d", (int)aQueueId, (unsigned)aMaxLifeTime, (int)aPsScheme, (int)aAckPolicy);
	Post(EConfigureQueue);
	}

void TA64Wha::ConfigureAC(TUint16 aCwMin[KNumOfEdcaQueues], TUint16 aCwMax[KNumOfEdcaQueues], TUint8 aAIFS[KNumOfEdcaQueues],
						  TUint16 aTxOplimit[KNumOfEdcaQueues], TUint16 aMaxReceiveLifeTime[KNumOfEdcaQueues])
	{
	(void)aMaxReceiveLifeTime;
	shim_log("[wha] ConfigureAC: cwmin %u/%u/%u/%u cwmax %u/%u/%u/%u aifs %u/%u/%u/%u txop %u/%u/%u/%u",
			 aCwMin[0], aCwMin[1], aCwMin[2], aCwMin[3], aCwMax[0], aCwMax[1], aCwMax[2], aCwMax[3],
			 aAIFS[0], aAIFS[1], aAIFS[2], aAIFS[3], aTxOplimit[0], aTxOplimit[1], aTxOplimit[2], aTxOplimit[3]);
	for (TUint q = 0; q < KNumOfEdcaQueues; ++q)
		{
		iAcCwMin[q] = aCwMin[q]; iAcCwMax[q] = aCwMax[q]; iAcAifs[q] = aAIFS[q]; iAcTxOp[q] = aTxOplimit[q];
		}
	Post(EConfigureAC);
	}

TStatus TA64Wha::SendPacket(const void* aFrame, TUint16 aLength, TQueueId aQueueId, TUint8 aTxRateClassId, TRate aMaxTransmitRate,
							TBool aMore, TPacketId aPacketId, TPowerLevel aPowerLevel, TUint32 aExpiryTime, void* aReserved)
	{
	(void)aTxRateClassId; (void)aMaxTransmitRate; (void)aMore; (void)aPowerLevel; (void)aExpiryTime; (void)aReserved;
	const TUint8* f = static_cast<const TUint8*>(aFrame);
	if (iTxCount < 16 || (iTxCount & 63) == 0)
		{
		shim_log("[wha] SendPacket #%u: %u bytes queue %d fc=%02x%02x to %02x:%02x:%02x:%02x:%02x:%02x rate 0x%x",
				 iTxCount, (unsigned)aLength, (int)aQueueId, f[0], f[1], f[4], f[5], f[6], f[7], f[8], f[9], (unsigned)aMaxTransmitRate);
			{					// the first 128 bytes (W4 bring-up: management in full, data headers)
			const TUint n = aLength < 128 ? aLength : 128;
			char hex[3 * 128 + 1];
			for (TUint i = 0; i < n; ++i)
				{
				static const char* d = "0123456789abcdef";
				hex[3 * i] = d[f[i] >> 4]; hex[3 * i + 1] = d[f[i] & 15]; hex[3 * i + 2] = ' ';
				}
			hex[3 * n] = 0;
			shim_log("[wha]   %s", hex);
			}
		}
	TUint slot = 0;
	while (slot < KMaxTxPending && iTx[slot].iUsed)
		++slot;
	if (slot == KMaxTxPending)
		{
		++iTxDropped;
		return KQueueFull;
		}
	iTx[slot].iId = aPacketId;
	iTx[slot].iRate = aMaxTransmitRate;
	iTx[slot].iUsed = ETrue;
	// a frame with the Protected bit gets its IV / CCMP header (and TKIP's MIC) here; the chip encrypts (a64wlansec.cpp)
	TUint txLen = aLength, sec = A64WLAN_SEC_NONE;
	const TUint8* tf = A64SecTx(f, aLength, txLen, sec);
	const TInt r = a64wlan_tx(tf, txLen, (f[0] & 0x0c) == 0 ? 1u : 0u, (void*)(slot + 1), sec);
	if (r)
		{
		iTx[slot].iUsed = EFalse;
		++iTxDropped;
		shim_log("[wha] SendPacket: a64wlan_tx r=%d", r);
		return KQueueFull;
		}
	++iTxCount;
	return KSuccessXfer;	// copied: the UMAC's buffer is free; SendPacketComplete follows from the tx sink (TxDone)
	}

void TA64Wha::Plt(TPltType aType, void* aParams)
	{
	(void)aParams;
	shim_log("[wha] Plt(%u) ignored", (unsigned)aType);
	}

// ---------------------------------------------------------------- the PDD objects
// the channel object the LDD gets from DPhysicalDevice::Create (deleted by DLogicalChannelBase at close); the WHA
// itself lives on: the chip stays up between opens
class TA64WlanPddIface : public MWlanPddIface
	{
public:
	virtual TBool Attach(MWlanOsa& aOsa, MWlanOsaExt& aOsaExt)
		{
		if (!TheWha)
			{
			TheHpa = new TA64Hpa(aOsaExt, aOsa);
			if (TheHpa)
				TheWha = new TA64Wha(aOsa, aOsaExt, *TheHpa, TheSpia);
			if (TheWha)
				{
				a64wlan_set_rx_sink(a64wlan_pdd_rx, TheWha);
				a64wlan_set_tx_sink(a64wlan_pdd_tx_done, TheWha);
				}
			}
		else
			TheWha->Rebind(aOsaExt);
		shim_log("[wha] Attach: osa %p -> wha %p", &aOsa, TheWha);
		return TheWha != 0;
		}
	virtual Wha& Extract() { return *TheWha; }
	// GetCapabilities: the default (no cached memory, the LDD's 32-byte allocation unit)
	};

_LIT(KA64WlanPddName, "wlan.phys");		// wlmdriverif.cpp: PDD_NAME, the name the engine frees by

class DA64WlanPddFactory : public DPhysicalDevice
	{
public:
	DA64WlanPddFactory() { iUnitsMask = 0x3; iVersion = TVersion(1, 0, 0); }		// KUnitEthernet | KUnitWlan (wlanlddcommon.h)
	virtual TInt Install() { return SetName(&KA64WlanPddName); }
	virtual void GetCaps(TDes8& aDes) const { aDes.FillZ(aDes.MaxLength()); }
	virtual TInt Create(DBase*& aChannel, TInt aUnit, const TDesC8*, const TVersion&)
		{
		shim_log("[wha] PDD Create unit %d", aUnit);
		aChannel = new TA64WlanPddIface;
		return aChannel ? KErrNone : KErrNoMemory;
		}
	virtual TInt Validate(TInt aUnit, const TDesC8*, const TVersion& aVer)
		{
		if (!Kern::QueryVersionSupported(iVersion, aVer))
			return KErrNotSupported;
		return (aUnit == 0 || aUnit == 1) ? KErrNone : KErrNotSupported;
		}
	};

// a64wlan.cpp installs it beside the REAL wlanldd once the WLAN DFC queue exists
extern "C" void A64WlanPddInstall()
	{
	DPhysicalDevice* pd = new DA64WlanPddFactory;
	TInt r = pd ? Kern::InstallPhysicalDevice(pd) : KErrNoMemory;
	ekseam_puts("[wlan] InstallPhysicalDevice(wlan.phys) r="); ekseam_puthex((TUint32)r);
	if (r == KErrNone) pd->Open();		// survives the engine's User::FreePhysicalDevice (see a64wlan.cpp)
	}
