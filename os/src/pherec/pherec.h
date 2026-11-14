/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

// pherec.h -- OURS (2026-11-06, PhoneLab "record"): the screen recorder server pherec.exe and its client RPheRec. The
// debug link's file service (phfs.exe) is the one client: PhoneLab's EPhRecStart / EPhRecRead / EPhRecStop requests
// (phfs_api.h, which has the record format) arrive there and are passed on. pherec.exe captures the screen as the user
// sees it (RPhFsCapture, 360x720), skips unchanged pictures, converts the rest to NV12 (NEON) and encodes them with the
// encoder plug-in (pherecenc.h) into a byte ring the PC polls. It refuses every client but phfs.exe.
#ifndef PHEREC_H
#define PHEREC_H
#include <e32std.h>

_LIT(KPheRecServerName, "PheRec");
_LIT(KPheRecServerExe, "pherec.exe");
const TUint32 KPheRecClientSid = 0xE7015739;	// phfs.exe

enum TPheRecMsg { EPrStart, EPrRead, EPrStop };

class RPheRec : public RSessionBase
	{
public:
	inline TInt Connect();
	// fps 0 = 30, qp 0 = 28; KErrNotFound = the encoder plug-in is not installed, KErrInUse = recording already
	inline TInt Start(TInt aFps, TInt aQp) { return SendReceive(EPrStart, TIpcArgs(aFps, aQp)); }
	// {state u32, seq u32 of the first byte} + the ring's bytes from aFrom, appended to aOut
	inline TInt Read(TUint32 aFrom, TDes8& aOut) { return SendReceive(EPrRead, TIpcArgs(aFrom, &aOut)); }
	// stops (waits for the recorder), appends the statistics line
	inline TInt Stop(TDes8& aOut) { return SendReceive(EPrStop, TIpcArgs(&aOut)); }
private:
	inline static TInt StartServer();
	};

inline TInt RPheRec::StartServer()
	{
	RProcess p;
	TInt r = p.Create(KPheRecServerExe, KNullDesC);
	if (r != KErrNone)
		return r;
	TRequestStatus st;
	p.Rendezvous(st);
	if (st != KRequestPending)
		p.Kill(0);						// rendezvous refused: the process is dead already
	else
		p.Resume();
	User::WaitForRequest(st);
	r = p.ExitType() == EExitPanic ? KErrGeneral : st.Int();
	p.Close();
	return r;
	}

inline TInt RPheRec::Connect()
	{
	TInt r = CreateSession(KPheRecServerName, TVersion(1, 0, 0), 2);
	if (r == KErrNotFound)
		{
		r = StartServer();
		if (r == KErrNone || r == KErrAlreadyExists)
			r = CreateSession(KPheRecServerName, TVersion(1, 0, 0), 2);
		}
	return r;
	}

#endif
