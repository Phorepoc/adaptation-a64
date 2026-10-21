// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// a64wlansec.cpp -- OURS (2026-10-11, PinePhone WiFi security; 3.2, 3.3, 3.5, 4):
// the host-side half of WLAN encryption under the WHA PDD. The chip (rtl8703b/r8703b_cam.c) holds the keys in its security
// CAM and does the cipher: WEP (RC4 + CRC-32), TKIP (RC4 with the per-packet key) and CCMP (AES). What the hardware does NOT
// do, and the 2010 UMAC does not either once the PDD advertises SSettings::KNoSecHdrAndTrailer, is done here:
//   TX  CCMP header / TKIP IV + ExtIV / WEP IV from a per-key counter, TKIP Michael MIC (software) -- for frames whose
//       Protected bit the UMAC set (it never sets it for the handshake's EAPOL frames or before a key exists);
//   RX  the chip leaves the IV / CCMP header and the ICV / MIC trailer in the frame it decrypted: replay check (PN),
//       TKIP Michael verify, strip, and the KEncrypt* flags of ReceivePacket (the UMAC's privacy filter wants them).
// A management frame with the Protected bit (the shared-key authentication response, WEP) is WEP-encrypted in software.
// Not here (TODO): software decrypt of frames the chip leaves undecrypted (SWDEC: design 3.3), TKIP countermeasures
// (3.5), PMF / BIP / protected management frames (section 5.1), the 16-byte WEP key.
#include <kernel/kernel.h>
#include <nkern.h>
#include <wlanpddiface.h>
#include "a64wlansec.h"
#include "a64wlan_api.h"
extern "C" {
#include "shim/shim_rt.h"
}

using namespace WlanHalApi;

static const TUint KSlots = 9;				// the UMAC's entry indices: WEP group 0..3, WEP pairwise 4, TKIP pw 5, TKIP grp 6, AES grp 7, AES pw 8
static const TUint KCamWepUnicastBase = 16;		// the pairwise-flagged copy of a WEP group key id n lives in CAM entry 16 + n
static const TUint KMaxRxFrame = 2400;			// the PDD's receive scratch buffer
enum TAlgo { ENone, EWep, ETkip, ECcmp };
static const TUint KPnSlots = 9;				// per TID 0..7, 8 = frames without QoS control

struct TSecKey
	{
	TBool iUsed;
	TUint8 iAlgo, iGroup, iKeyId, iKeyLen;
	TUint8 iKey[16];
	TUint8 iTxMic[8], iRxMic[8];			// TKIP Michael keys (the CAM only has the 16-byte temporal key)
	TUint8 iMac[6];						// pairwise: the peer; group: filled in from the BSSID at commit
	TUint64 iTxPn;						// CCMP PN / TKIP TSC / WEP IV counter of the last frame sent
	TUint64 iRxPn[KPnSlots];				// replay floor: the highest PN accepted
	};

static TSecKey Keys[KSlots];
static TUint8 Bssid[6];
static TUint8 WepTxKeyId;
static TUint8 TxBuf[2312 + 32];
// counters
static TUint TxProt, TxNoKey, RxProt, RxOk, RxReplay, RxMicFail, RxSwdec, RxBad, RxNoKey;

void A64SecSetBssid(const TUint8* aBssid) { memcpy(Bssid, aBssid, 6); }
void A64SecSetWepTxKeyId(TUint8 aId) { WepTxKeyId = aId & 3; }
void A64SecReset()
	{
	memclr(Keys, sizeof(Keys));
	WepTxKeyId = 0;
	}

// ---------------------------------------------------------------- keys (WHA AddKey / RemoveKey)
static void SetRsc(TSecKey& aK, const TUint8* aRsc)
	{
	// the initial replay floor of a group key: the RSC the supplicant handed over, PN0 first (ASSUMED little-endian as in the
	// EAPOL-Key RSC field; the 2010 UMAC copies the 8 bytes through untouched)
	TUint64 pn = 0;
	for (TUint i = 0; i < 6; ++i)
		pn |= (TUint64)aRsc[i] << (8 * i);
	for (TUint i = 0; i < KPnSlots; ++i)
		aK.iRxPn[i] = pn;
	}

TInt A64SecAddKey(TKeyType aType, const void* aKey, TUint8 aIndex)
	{
	if (aIndex >= KSlots)
		return KErrArgument;
	TSecKey k;
	memclr(&k, sizeof(k));
	k.iUsed = ETrue;
	switch (aType)
		{
		case EWepPairWiseKey:
			{
			const SWepPairwiseKey* s = static_cast<const SWepPairwiseKey*>(aKey);
			if (s->iKeyLengthInBytes != 5 && s->iKeyLengthInBytes != 13)
				return KErrNotSupported;		// (the 16-byte "deprecated" WEP key: capability KWep16ByteKey, not advertised)
			k.iAlgo = EWep; k.iGroup = 0; k.iKeyId = 0; k.iKeyLen = s->iKeyLengthInBytes;
			memcpy(k.iKey, s->iKey, k.iKeyLen);
			memcpy(k.iMac, s->iMacAddr.iMacAddress, 6);
			break;
			}
		case EWepGroupKey:
			{
			const SWepGroupKey* s = static_cast<const SWepGroupKey*>(aKey);
			if ((s->iKeyLengthInBytes != 5 && s->iKeyLengthInBytes != 13) || s->iKeyId > 3)
				return KErrNotSupported;
			k.iAlgo = EWep; k.iGroup = 1; k.iKeyId = s->iKeyId; k.iKeyLen = s->iKeyLengthInBytes;
			memcpy(k.iKey, s->iKey, k.iKeyLen);
			break;
			}
		case ETkipPairWiseKey:
			{
			const STkipPairwiseKey* s = static_cast<const STkipPairwiseKey*>(aKey);
			k.iAlgo = ETkip; k.iGroup = 0; k.iKeyId = s->iKeyId & 3; k.iKeyLen = 16;
			memcpy(k.iKey, s->iTkipKey, 16);
			memcpy(k.iRxMic, s->iRxMicKey, 8);
			memcpy(k.iTxMic, s->iTxMicKey, 8);
			memcpy(k.iMac, s->iMacAddr.iMacAddress, 6);
			break;
			}
		case ETkipGroupKey:
			{
			const STkipGroupKey* s = static_cast<const STkipGroupKey*>(aKey);
			k.iAlgo = ETkip; k.iGroup = 1; k.iKeyId = s->iKeyId & 3; k.iKeyLen = 16;
			memcpy(k.iKey, s->iTkipKey, 16);
			memcpy(k.iRxMic, s->iRxMicKey, 8);
			SetRsc(k, s->iRxSequenceCounter);
			break;
			}
		case EAesPairWiseKey:
			{
			const SAesPairwiseKey* s = static_cast<const SAesPairwiseKey*>(aKey);
			k.iAlgo = ECcmp; k.iGroup = 0; k.iKeyId = 0; k.iKeyLen = 16;
			memcpy(k.iKey, s->iAesKey, 16);
			memcpy(k.iMac, s->iMacAddr.iMacAddress, 6);
			break;
			}
		case EAesGroupKey:
			{
			const SAesGroupKey* s = static_cast<const SAesGroupKey*>(aKey);
			k.iAlgo = ECcmp; k.iGroup = 1; k.iKeyId = s->iKeyId & 3; k.iKeyLen = 16;
			memcpy(k.iKey, s->iAesKey, 16);
			SetRsc(k, s->iRxSequenceCounter);
			break;
			}
		default:					// WAPI (off), EKeyNone
			return KErrNotSupported;
		}
	// a key installed on an entry replaces what was there: a new PTK / GTK restarts its counters (the standard demands it)
	Keys[aIndex] = k;
	return KErrNone;
	}

static TUint CamAlgo(const TSecKey& aK)
	{
	return aK.iAlgo == EWep ? A64WLAN_KEY_WEP : aK.iAlgo == ETkip ? A64WLAN_KEY_TKIP : A64WLAN_KEY_CCMP;
	}

TInt A64SecCommit(TUint8 aIndex)
	{
	if (aIndex >= KSlots || !Keys[aIndex].iUsed)
		return KErrArgument;
	TSecKey& k = Keys[aIndex];
	if (k.iGroup)
		memcpy(k.iMac, Bssid, 6);		// a STA's group key is searched by the transmitter = the AP
	TInt r = a64wlan_set_key(aIndex, CamAlgo(k), k.iKeyId, k.iMac, k.iKey, k.iKeyLen, k.iGroup);
	if (!r && k.iAlgo == EWep && k.iGroup)
		{
		// static WEP: the AP's unicast frames use the same key (design 4.4): a pairwise-flagged entry as well, same id
		r = a64wlan_set_key(KCamWepUnicastBase + k.iKeyId, A64WLAN_KEY_WEP, k.iKeyId, Bssid, k.iKey, k.iKeyLen, 0);
		}
	return r;
	}

void A64SecRemoveKey(TUint8 aIndex)
	{
	if (aIndex >= KSlots)
		return;
	if (Keys[aIndex].iUsed && Keys[aIndex].iAlgo == EWep && Keys[aIndex].iGroup)
		a64wlan_clear_key(KCamWepUnicastBase + Keys[aIndex].iKeyId);
	memclr(&Keys[aIndex], sizeof(TSecKey));
	a64wlan_clear_key(aIndex);
	}

// ---------------------------------------------------------------- Michael (TKIP MIC), IEEE 802.11 section 12.5.2.3
static inline TUint32 Rol(TUint32 v, TUint n) { return (v << n) | (v >> (32 - n)); }
static inline TUint32 Ror(TUint32 v, TUint n) { return (v >> n) | (v << (32 - n)); }
static inline TUint32 Le32(const TUint8* p) { return p[0] | (TUint32)p[1] << 8 | (TUint32)p[2] << 16 | (TUint32)p[3] << 24; }

class TMichael
	{
public:
	TMichael(const TUint8* aKey) : iL(Le32(aKey)), iR(Le32(aKey + 4)), iN(0), iWord(0) {}
	void Feed(const TUint8* aData, TUint aLen)
		{
		for (TUint i = 0; i < aLen; ++i)
			{
			iWord |= (TUint32)aData[i] << (8 * iN);
			if (++iN == 4)
				Block();
			}
		}
	void Finish(TUint8 aOut[8])
		{
		const TUint8 pad[5] = { 0x5a, 0, 0, 0, 0 };		// 0x5a then zeros up to a multiple of 4, plus 4 more zero bytes
		Feed(pad, 1);
		while (iN)
			Feed(pad + 1, 1);
		Feed(pad + 1, 4);
		for (TUint i = 0; i < 4; ++i)
			{
			aOut[i] = (TUint8)(iL >> (8 * i));
			aOut[4 + i] = (TUint8)(iR >> (8 * i));
			}
		}
private:
	void Block()
		{
		iL ^= iWord;
		iR ^= Rol(iL, 17); iL += iR;
		iR ^= ((iL & 0xff00ff00u) >> 8) | ((iL & 0x00ff00ffu) << 8); iL += iR;
		iR ^= Rol(iL, 3); iL += iR;
		iR ^= Ror(iL, 2); iL += iR;
		iWord = 0; iN = 0;
		}
	TUint32 iL, iR;
	TUint iN;
	TUint32 iWord;
	};

// MIC over DA, SA, priority, 3 zero bytes, then the MSDU data (everything after the IV: LLC/SNAP included)
static void MichaelMic(const TUint8* aKey, const TUint8* aDa, const TUint8* aSa, TUint aPrio, const TUint8* aData, TUint aLen, TUint8 aOut[8])
	{
	TMichael m(aKey);
	const TUint8 pr[4] = { (TUint8)aPrio, 0, 0, 0 };
	m.Feed(aDa, 6); m.Feed(aSa, 6); m.Feed(pr, 4); m.Feed(aData, aLen);
	m.Finish(aOut);
	}

// ---------------------------------------------------------------- software WEP (protected management frames only)
static TUint32 Crc32(const TUint8* aData, TUint aLen)
	{
	TUint32 c = 0xffffffffu;
	for (TUint i = 0; i < aLen; ++i)
		{
		c ^= aData[i];
		for (TUint b = 0; b < 8; ++b)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	return ~c;
	}

static void Rc4Crypt(const TUint8* aKey, TUint aKeyLen, TUint8* aData, TUint aLen)
	{
	TUint8 s[256];
	for (TUint i = 0; i < 256; ++i) s[i] = (TUint8)i;
	TUint j = 0;
	for (TUint i = 0; i < 256; ++i)
		{
		j = (j + s[i] + aKey[i % aKeyLen]) & 255;
		const TUint8 t = s[i]; s[i] = s[j]; s[j] = t;
		}
	TUint a = 0, b = 0;
	for (TUint n = 0; n < aLen; ++n)
		{
		a = (a + 1) & 255; b = (b + s[a]) & 255;
		const TUint8 t = s[a]; s[a] = s[b]; s[b] = t;
		aData[n] ^= s[(s[a] + s[b]) & 255];
		}
	}

// ---------------------------------------------------------------- helpers
static TBool IsGroupAddr(const TUint8* aA) { return aA[0] & 1; }

static TSecKey* FindWepTxKey()
	{
	// dynamic WEP: the pairwise entry (slot 4); static WEP: the default key id's group entry
	if (Keys[4].iUsed && Keys[4].iAlgo == EWep)
		return &Keys[4];
	for (TUint i = 0; i < 4; ++i)
		if (Keys[i].iUsed && Keys[i].iAlgo == EWep && Keys[i].iKeyId == WepTxKeyId)
			return &Keys[i];
	return 0;
	}

static TSecKey* FindTxKey(const TUint8* aRa)
	{
	if (IsGroupAddr(aRa))
		return 0;					// a STA sends no group-addressed frames: never encrypt one with a pairwise key by accident
	for (TUint i = 5; i < KSlots; ++i)		// the pairwise TKIP / AES entries (5, 8) first
		if (Keys[i].iUsed && !Keys[i].iGroup && Keys[i].iAlgo != EWep && !memcompare(Keys[i].iMac, 6, aRa, 6))
			return &Keys[i];
	return FindWepTxKey();
	}

// ---------------------------------------------------------------- transmit
const TUint8* A64SecTx(const TUint8* aF, TUint aLen, TUint& aOutLen, TUint& aSec)
	{
	aOutLen = aLen;
	aSec = A64WLAN_SEC_NONE;
	if (aLen < 24 || !(aF[1] & 0x40))
		return aF;					// not protected: untouched (open networks, the handshake's EAPOL)
	const TUint type = (aF[0] >> 2) & 3;
	const TBool qos = type == 2 && (aF[0] & 0x80);
	const TUint hdr = qos ? 26 : 24;
	if (aLen < hdr || aLen + 32 > sizeof(TxBuf))
		{
		aSec = A64WLAN_SEC_FAIL;
		return aF;
		}
	++TxProt;
	TSecKey* k = FindTxKey(aF + 4);
	if (!k)
		{
		++TxNoKey;
		if (TxNoKey <= 8)
			shim_log("[sec] TX: protected frame fc=%02x%02x to %02x:%02x:%02x:%02x:%02x:%02x but no key -- dropped", aF[0], aF[1], aF[4], aF[5], aF[6], aF[7], aF[8], aF[9]);
		aSec = A64WLAN_SEC_FAIL;
		return aF;
		}
	memcpy(TxBuf, aF, hdr);
	const TUint8 id = (TUint8)(k->iKeyId << 6);
	const TUint8* body = aF + hdr;
	const TUint bodyLen = aLen - hdr;
	++k->iTxPn;
	const TUint64 pn = k->iTxPn;
	TUint8* p = TxBuf + hdr;
	if (type == 0)
		{
		// a protected management frame: only the WEP shared-key authentication (software WEP: IV, RC4(body + CRC-32 ICV));
		// TODO PMF: CCMP / BIP protected management frames, design section 5.1
		if (k->iAlgo != EWep)
			{
			aSec = A64WLAN_SEC_FAIL;
			return aF;
			}
		p[0] = (TUint8)pn; p[1] = (TUint8)(pn >> 8); p[2] = (TUint8)(pn >> 16); p[3] = id;
		memcpy(p + 4, body, bodyLen);
		const TUint32 icv = Crc32(body, bodyLen);
		for (TUint i = 0; i < 4; ++i) p[4 + bodyLen + i] = (TUint8)(icv >> (8 * i));
		TUint8 rk[3 + 13];
		memcpy(rk, p, 3); memcpy(rk + 3, k->iKey, k->iKeyLen);
		Rc4Crypt(rk, 3 + k->iKeyLen, p + 4, bodyLen + 4);
		aOutLen = hdr + 4 + bodyLen + 4;
		return TxBuf;
		}
	switch (k->iAlgo)
		{
		case EWep:				// IV: the 24-bit counter, then the key id << 6; the chip appends the ICV
			p[0] = (TUint8)pn; p[1] = (TUint8)(pn >> 8); p[2] = (TUint8)(pn >> 16); p[3] = id;
			memcpy(p + 4, body, bodyLen);
			aOutLen = hdr + 4 + bodyLen;
			aSec = A64WLAN_SEC_RC4;
			break;
		case ETkip:				// TSC1, (TSC1 | 0x20) & 0x7f, TSC0, ExtIV | key id, TSC2..5; the MIC is ours, the ICV the chip's
			p[0] = (TUint8)(pn >> 8); p[1] = (TUint8)(((pn >> 8) | 0x20) & 0x7f); p[2] = (TUint8)pn; p[3] = id | 0x20;
			p[4] = (TUint8)(pn >> 16); p[5] = (TUint8)(pn >> 24); p[6] = (TUint8)(pn >> 32); p[7] = (TUint8)(pn >> 40);
			memcpy(p + 8, body, bodyLen);
			// to-DS frame: DA = address 3, SA = address 2
			MichaelMic(k->iTxMic, aF + 16, aF + 10, qos ? (aF[24] & 7) : 0, body, bodyLen, p + 8 + bodyLen);
			aOutLen = hdr + 8 + bodyLen + 8;
			aSec = A64WLAN_SEC_RC4;
			break;
		default:				// CCMP: PN0, PN1, 0, ExtIV | key id, PN2..5; the chip appends the MIC
			p[0] = (TUint8)pn; p[1] = (TUint8)(pn >> 8); p[2] = 0; p[3] = id | 0x20;
			p[4] = (TUint8)(pn >> 16); p[5] = (TUint8)(pn >> 24); p[6] = (TUint8)(pn >> 32); p[7] = (TUint8)(pn >> 40);
			memcpy(p + 8, body, bodyLen);
			aOutLen = hdr + 8 + bodyLen;
			aSec = A64WLAN_SEC_AES;
			break;
		}
	if (TxProt <= 6)
		shim_log("[sec] TX protected: algo %u key id %u pn %u, %u -> %u bytes, sec %u", (unsigned)k->iAlgo, (unsigned)k->iKeyId, (unsigned)pn, aLen, aOutLen, aSec);
	return TxBuf;
	}

// ---------------------------------------------------------------- receive
TInt A64SecRx(const TUint8* aF, TUint aLen, TUint aFlags, TUint8* aOut, TUint& aOutLen, TUint32& aWhaFlags)
	{
	aOutLen = aLen;
	aWhaFlags = KEncryptNone;
	if (aLen < 24 || !(aF[1] & 0x40) || ((aF[0] >> 2) & 3) != 2)
		return 0;					// not a protected data frame (protected management frames: TODO PMF, design 5.1)
	++RxProt;
	if (aFlags & A64WLAN_RX_SWDEC)
		{
		// TODO: software decrypt with the PDD's own key copy (design 3.3); until then undecrypted frames cannot be used
		if (++RxSwdec <= 8)
			shim_log("[sec] RX: the chip did not decrypt fc=%02x%02x (%u bytes) -- dropped", aF[0], aF[1], aLen);
		return -1;
		}
	if (!(aFlags & A64WLAN_RX_DECRYPTED))
		{
		++RxNoKey;					// protected, but the chip found no key (SECURITY = 0)
		return -1;
		}
	const TBool qos = aF[0] & 0x80;
	const TUint hdr = qos ? 26 : 24;
	const TUint sec = A64WLAN_RX_SECTYPE(aFlags);
	TUint ivLen, trailer;
	TAlgo algo;
	switch (sec)
		{
		case A64WLAN_KEYALGO_WEP40: case A64WLAN_KEYALGO_WEP104: algo = EWep; ivLen = 4; trailer = 4; break;
		case A64WLAN_KEYALGO_TKIP: algo = ETkip; ivLen = 8; trailer = 4 + 8; break;		// payload | MIC 8 | ICV 4
		case A64WLAN_KEYALGO_AES: algo = ECcmp; ivLen = 8; trailer = 8; break;
		default: ++RxBad; return -1;
		}
	if (aLen > KMaxRxFrame || aLen < hdr + ivLen + trailer)
		{ ++RxBad; return -1; }
	const TUint8* iv = aF + hdr;
	const TUint keyId = iv[3] >> 6;
	const TBool group = IsGroupAddr(aF + 4);
	const TUint tid = qos ? (aF[24] & 7) : 8;
	TSecKey* k = 0;
	for (TUint i = 0; i < KSlots && !k; ++i)
		{
		TSecKey& c = Keys[i];
		if (!c.iUsed || c.iAlgo != algo)
			continue;
		if (algo == EWep)
			{
			if (c.iKeyId == keyId && (c.iGroup || !group))		// static WEP: the id picks the key; dynamic WEP: the pairwise entry
				k = &c;
			}
		else if (group ? (c.iGroup && c.iKeyId == keyId) : !c.iGroup)
			k = &c;
		}
	if (!k)
		{ ++RxNoKey; return -1; }
	TUint64 pn = 0;
	if (algo == ECcmp)
		{
		if (!(iv[3] & 0x20)) { ++RxBad; return -1; }
		pn = iv[0] | (TUint64)iv[1] << 8 | (TUint64)iv[4] << 16 | (TUint64)iv[5] << 24 | (TUint64)iv[6] << 32 | (TUint64)iv[7] << 40;
		}
	else if (algo == ETkip)
		{
		if (!(iv[3] & 0x20)) { ++RxBad; return -1; }
		pn = iv[2] | (TUint64)iv[0] << 8 | (TUint64)iv[4] << 16 | (TUint64)iv[5] << 24 | (TUint64)iv[6] << 32 | (TUint64)iv[7] << 40;
		}
	if (algo != EWep && pn <= k->iRxPn[tid])
		{
		if (++RxReplay <= 8)
			shim_log("[sec] RX: replayed / old PN %u <= %u (tid %u) -- dropped", (unsigned)pn, (unsigned)k->iRxPn[tid], tid);
		return -1;
		}
	const TUint dataLen = aLen - hdr - ivLen - trailer;
	if (algo == ETkip)
		{
		// from-DS frame: DA = address 1, SA = address 3
		TUint8 mic[8];
		MichaelMic(k->iRxMic, aF + 4, aF + 16, qos ? (aF[24] & 7) : 0, aF + hdr + ivLen, dataLen, mic);
		if (memcompare(mic, 8, aF + hdr + ivLen + dataLen, 8))
			{
			// TODO TKIP countermeasures: two failures within 60 s = report + 60 s silence (design 3.5)
			if (++RxMicFail <= 8)
				shim_log("[sec] RX: TKIP Michael MIC failure -- dropped");
			return -1;
			}
		}
	if (algo != EWep)
		k->iRxPn[tid] = pn;
	memcpy(aOut, aF, hdr);
	memcpy(aOut + hdr, aF + hdr + ivLen, dataLen);
	aOutLen = hdr + dataLen;
	aWhaFlags = algo == EWep ? KEncryptWep : algo == ETkip ? KEncryptTkip : KEncryptAes;
	++RxOk;
	return 1;
	}

void A64SecReport()
	{
	shim_log("[sec] tx protected %u (no key %u), rx protected %u ok %u replay %u mic-fail %u swdec %u no-key %u bad %u",
			 TxProt, TxNoKey, RxProt, RxOk, RxReplay, RxMicFail, RxSwdec, RxNoKey, RxBad);
	}
