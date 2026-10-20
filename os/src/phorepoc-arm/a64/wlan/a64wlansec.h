// a64wlansec.h -- OURS: the host-side WLAN security of the WHA PDD (a64wlansec.cpp). Design: docs/phorepoc/wlan-security-design.md.
#ifndef A64WLANSEC_H
#define A64WLANSEC_H
#include <kernel/kernel.h>
#include <wlanpddiface.h>

// WHA AddKey: parse the UMAC's key struct, remember the key (the PDD's own copy: TX IV / PN state, the Michael keys, the
// RX replay floor) in slot aIndex. 0 = ok, KErrNotSupported = a key type / length we do not do. The CAM is not touched.
TInt A64SecAddKey(WlanHalApi::TKeyType aType, const void* aKey, TUint8 aEntryIndex);
// ... and the CAM write for the slot (the chip core: the WLAN thread). 0 = ok
TInt A64SecCommit(TUint8 aEntryIndex);
void A64SecRemoveKey(TUint8 aEntryIndex);	// slot + CAM entry (WLAN thread)
void A64SecReset();						// every key forgotten (the table; the CAM is cleared by the chip core at disassociation)
void A64SecSetBssid(const TUint8* aBssid);	// the AP's address: the group keys' CAM address, the WEP unicast copy
void A64SecSetWepTxKeyId(TUint8 aId);		// MIB dot11WepDefaultKeyId

// Transmit: aFrame = the UMAC's plain 802.11 frame. Returns the frame to hand to the chip (aFrame itself when nothing
// changes, else an internal buffer valid until the next call, serialised by the LDD mutex) and its length; aSec is
// the A64WLAN_SEC_* the chip core must apply. Only a frame with the Protected bit is touched.
const TUint8* A64SecTx(const TUint8* aFrame, TUint aLen, TUint& aOutLen, TUint& aSec);

// Receive: a protected data frame with the chip core's flags. Returns 0 = deliver aFrame unchanged (not protected),
// 1 = deliver aOut (aOutLen bytes: IV and trailer stripped, MIC verified, replay checked) with the WHA KEncrypt* in
// aWhaFlags, <0 = drop (counted). aOut must hold aLen bytes.
TInt A64SecRx(const TUint8* aFrame, TUint aLen, TUint aRxFlags, TUint8* aOut, TUint& aOutLen, TUint32& aWhaFlags);

void A64SecReport();					// the counters to the log
#endif
