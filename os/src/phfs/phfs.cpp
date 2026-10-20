// phfs.cpp -- OURS (2026-10-11, PhoneLab Project panel): the user-side helper of the debug link's file service. The kernel
// debug link (phlink.cpp) has no file server; it forwards the PC's directory list / file read / file write requests to
// this process through the LDD "PhFs.A64" (src/phorepoc-arm/a64/usbcc/phfs_ldd.cpp, contract phfs_api.h), which runs them
// against the real file server (RFs) and replies. One request at a time; started by the SSM's non-critical list.
// Paths arrive as UTF-8 "/C:/dir/file" ("/" = the drive list). [phfs] lines (A64_RDEBUG_LOG).
// Since 2026-10-19 also delete, and the terminal requests: PhoneLab's Terminal panel reaches the phetty daemon's ttys here.
#include <e32base.h>
#include <e32debug.h>
#include <f32file.h>
#include "phfs_api.h"
#include "phetty.h"

static TUint32 Le32(const TUint8* aP)
	{
	return aP[0] | (aP[1] << 8) | (aP[2] << 16) | ((TUint32)aP[3] << 24);
	}

static void AppendLe32(TDes8& aDes, TUint32 aV)
	{
	aDes.Append((TUint8)aV); aDes.Append((TUint8)(aV >> 8)); aDes.Append((TUint8)(aV >> 16)); aDes.Append((TUint8)(aV >> 24));
	}

static void AppendUtf8(TDes8& aDes, const TDesC16& aText)
	{
	for (TInt i = 0; i < aText.Length(); ++i)
		{
		const TUint c = aText[i];
		if (aDes.MaxLength() - aDes.Length() < 3)
			return;
		if (c < 0x80)
			aDes.Append((TUint8)c);
		else if (c < 0x800)
			{ aDes.Append((TUint8)(0xC0 | (c >> 6))); aDes.Append((TUint8)(0x80 | (c & 0x3F))); }
		else
			{ aDes.Append((TUint8)(0xE0 | (c >> 12))); aDes.Append((TUint8)(0x80 | ((c >> 6) & 0x3F))); aDes.Append((TUint8)(0x80 | (c & 0x3F))); }
		}
	}

static void AppendNumber(TDes8& aDes, TInt64 aV)
	{
	TBuf8<24> n;
	n.Num(aV);
	aDes.Append(n);
	}

// UTF-8 -> UTF-16 (the BMP; a malformed byte stands for itself)
static void FromUtf8(TDes16& aOut, const TUint8* aP, TInt aLen)
	{
	aOut.Zero();
	for (TInt i = 0; i < aLen && aOut.Length() < aOut.MaxLength(); )
		{
		const TUint8 b = aP[i];
		if (b < 0x80 || (b & 0xC0) == 0x80 || i + 1 >= aLen)
			{ aOut.Append(b); ++i; }
		else if ((b & 0xE0) == 0xC0)
			{ aOut.Append(((b & 0x1F) << 6) | (aP[i + 1] & 0x3F)); i += 2; }
		else if (i + 2 < aLen)
			{ aOut.Append(((b & 0x0F) << 12) | ((aP[i + 1] & 0x3F) << 6) | (aP[i + 2] & 0x3F)); i += 3; }
		else
			{ aOut.Append(b); ++i; }
		}
	}

// "/C:/dir/file" -> "C:\dir\file"; an empty result = the root (the drive list). "/c/x" is taken as "C:\x".
static void ToSymbianPath(TFileName& aOut, const TUint8* aP, TInt aLen)
	{
	FromUtf8(aOut, aP, aLen);
	for (TInt i = 0; i < aOut.Length(); ++i)
		if (aOut[i] == '/')
			aOut[i] = '\\';
	while (aOut.Length() && aOut[0] == '\\')
		aOut.Delete(0, 1);
	if (aOut.Length() && (aOut.Length() == 1 || aOut[1] != ':'))
		aOut.Insert(1, _L(":"));
	}

class CPhFs : public CBase
	{
public:
	static CPhFs* NewL();
	CPhFs() : iRep(NULL, 0) {}
	~CPhFs();
	void Run();
private:
	void Handle(const TUint8* aReq, TInt aLen);
	TInt List(const TUint8* aP, TInt aLen);
	TInt Read(const TUint8* aP, TInt aLen);
	TInt Write(const TUint8* aP, TInt aLen);
	TInt Delete(const TUint8* aP, TInt aLen);
	TInt Tty(TInt aOp, const TUint8* aP, TInt aLen);
	void Finish(TInt aStatus);
private:
	RFs iFs;
	RPhFs iCh;
	HBufC8* iReqBuf;
	HBufC8* iRepBuf;
	HBufC8* iDataBuf;
	TPtr8 iRep;							// {status, data}: data is appended after the 4 status bytes
	RPheTty iTty;
	TBool iTtyOpen;
	};

CPhFs* CPhFs::NewL()
	{
	CPhFs* s = new (ELeave) CPhFs;
	CleanupStack::PushL(s);
	User::LeaveIfError(s->iFs.Connect());
	User::LeaveIfError(s->iCh.Open());
	s->iReqBuf = HBufC8::NewL(4 + KPhFsMaxRequest);
	s->iRepBuf = HBufC8::NewL(KPhFsMaxReply);
	s->iDataBuf = HBufC8::NewL(KPhFsMaxReply - 4);
	CleanupStack::Pop(s);
	return s;
	}

CPhFs::~CPhFs()
	{
	if (iTtyOpen)
		iTty.Close();
	iCh.Close();
	iFs.Close();
	delete iReqBuf;
	delete iRepBuf;
	delete iDataBuf;
	}

void CPhFs::Run()
	{
	for (;;)
		{
		TPtr8 req = iReqBuf->Des();
		TRequestStatus st;
		iCh.Get(st, req);
		User::WaitForRequest(st);
		if (st.Int() != KErrNone || req.Length() < 4)
			{
			RDebug::Printf("[phfs] get failed %d", st.Int());
			if (st.Int() == KErrCancel || st.Int() == KErrAbort)
				return;
			User::After(1000000);
			continue;
			}
		Handle(req.Ptr(), req.Length());
		}
	}

// run the request, send the reply: {status LE32, data}
void CPhFs::Handle(const TUint8* aReq, TInt aLen)
	{
	const TInt op = (TInt)Le32(aReq);
	iRep.Set(iRepBuf->Des());
	iRep.Zero();
	AppendLe32(iRep, 0);									// the status, patched in Finish
	TInt r;
	switch (op)
		{
	case EPhFsList:	r = List(aReq + 4, aLen - 4); break;
	case EPhFsRead:	r = Read(aReq + 4, aLen - 4); break;
	case EPhFsWrite:r = Write(aReq + 4, aLen - 4); break;
	case EPhFsDelete:r = Delete(aReq + 4, aLen - 4); break;
	case EPhTtyList: case EPhTtyNew: case EPhTtyRead: case EPhTtyWrite: case EPhTtyResize: case EPhTtyKill:
		r = Tty(op, aReq + 4, aLen - 4); break;
	default:		r = KErrNotSupported; break;
		}
	Finish(r);
	}

void CPhFs::Finish(TInt aStatus)
	{
	if (aStatus != KErrNone)
		iRep.SetLength(4);									// no data on an error
	TUint8* p = (TUint8*)iRep.Ptr();
	p[0] = (TUint8)aStatus; p[1] = (TUint8)(aStatus >> 8); p[2] = (TUint8)(aStatus >> 16); p[3] = (TUint8)(aStatus >> 24);
	const TInt r = iCh.Reply(iRep);
	if (r != KErrNone)
		RDebug::Printf("[phfs] reply failed %d", r);
	}

// "d|f <size> <mtime unix s> <name>\n" per entry
TInt CPhFs::List(const TUint8* aP, TInt aLen)
	{
	TFileName path;
	ToSymbianPath(path, aP, aLen);
	if (path.Length() == 0)
		{	// the root: the drives
		TDriveList drives;
		TInt r = iFs.DriveList(drives);
		if (r != KErrNone)
			return r;
		for (TInt d = 0; d < KMaxDrives; ++d)
			if (drives[d])
				{
				TChar c;
				if (iFs.DriveToChar(d, c) == KErrNone)
					{
					TBuf<2> name;
					name.Append(c);
					name.Append(':');
					iRep.Append(_L8("d 0 0 "));
					AppendUtf8(iRep, name);
					iRep.Append('\n');
					}
				}
		return KErrNone;
		}
	if (path[path.Length() - 1] != '\\')
		path.Append('\\');
	CDir* dir = NULL;
	TInt r = iFs.GetDir(path, KEntryAttMaskSupported, ESortByName, dir);
	if (r != KErrNone)
		return r;
	const TTime epoch(TDateTime(1970, EJanuary, 0, 0, 0, 0, 0));
	for (TInt i = 0; i < dir->Count() && iRep.MaxLength() - iRep.Length() > 300; ++i)
		{
		const TEntry& e = (*dir)[i];
		iRep.Append(e.IsDir() ? 'd' : 'f');
		iRep.Append(' ');
		AppendNumber(iRep, e.IsDir() ? 0 : e.iSize);
		iRep.Append(' ');
		TTimeIntervalSeconds secs(0);
		e.iModified.SecondsFrom(epoch, secs);
		AppendNumber(iRep, secs.Int());
		iRep.Append(' ');
		AppendUtf8(iRep, e.iName);
		iRep.Append('\n');
		}
	delete dir;
	return KErrNone;
	}

// offset u32, len u32, path -> data
TInt CPhFs::Read(const TUint8* aP, TInt aLen)
	{
	if (aLen < 8)
		return KErrArgument;
	const TInt off = (TInt)Le32(aP);
	TInt want = (TInt)Le32(aP + 4);
	if (want > iRepBuf->Des().MaxLength() - 4)
		want = iRepBuf->Des().MaxLength() - 4;
	TFileName path;
	ToSymbianPath(path, aP + 8, aLen - 8);
	RFile f;
	TInt r = f.Open(iFs, path, EFileRead | EFileShareAny);
	if (r != KErrNone)
		return r;
	TPtr8 data = iDataBuf->Des();
	r = f.Read(off, data, want);
	f.Close();
	if (r == KErrNone)
		iRep.Append(data);
	return r;
	}

// offset u32, flags u32 (1 = truncate), pathlen u32, path, data -> written u32
TInt CPhFs::Write(const TUint8* aP, TInt aLen)
	{
	if (aLen < 12)
		return KErrArgument;
	const TInt off = (TInt)Le32(aP);
	const TUint32 flags = Le32(aP + 4);
	const TInt plen = (TInt)Le32(aP + 8);
	if (plen < 0 || 12 + plen > aLen)
		return KErrArgument;
	TFileName path;
	ToSymbianPath(path, aP + 12, plen);
	if (path.Length() < 3)
		return KErrArgument;
	const TPtrC8 data(aP + 12 + plen, aLen - 12 - plen);
	RFile f;
	TInt r;
	if (flags & 1)
		{
		TParse p;
		if (p.Set(path, NULL, NULL) == KErrNone)
			(void)iFs.MkDirAll(p.DriveAndPath());		// an upload into a new folder
		r = f.Replace(iFs, path, EFileWrite | EFileShareAny);
		}
	else
		{
		r = f.Open(iFs, path, EFileWrite | EFileShareAny);
		if (r == KErrNotFound)
			r = f.Create(iFs, path, EFileWrite | EFileShareAny);
		}
	if (r != KErrNone)
		return r;
	r = f.Write(off, data);
	if (r == KErrNone)
		r = f.Flush();
	f.Close();
	if (r == KErrNone)
		AppendLe32(iRep, (TUint32)data.Length());
	return r;
	}

// path -> a file is deleted; a folder is removed with everything in it
TInt CPhFs::Delete(const TUint8* aP, TInt aLen)
	{
	TFileName path;
	ToSymbianPath(path, aP, aLen);
	if (path.Length() < 4)
		return KErrAccessDenied;							// never a whole drive
	TEntry e;
	TInt r = iFs.Entry(path, e);
	if (r != KErrNone)
		return r;
	if (!e.IsDir())
		{
		if (e.IsReadOnly())
			(void)iFs.SetAtt(path, 0, KEntryAttReadOnly);
		return iFs.Delete(path);
		}
	if (path[path.Length() - 1] != '\\')
		path.Append('\\');
	CFileMan* fm = NULL;
	TRAP(r, fm = CFileMan::NewL(iFs));
	if (r != KErrNone)
		return r;
	r = fm->RmDir(path);
	delete fm;
	return r;
	}

// the terminal requests (phfs_api.h): a thin bridge to phettyd; no waiting, PhoneLab polls
TInt CPhFs::Tty(TInt aOp, const TUint8* aP, TInt aLen)
	{
	if (!iTtyOpen)
		{
		const TBool start = aOp == EPhTtyList && aLen >= 4 && (Le32(aP) & 1);
		TInt r = start ? iTty.Connect() : iTty.ConnectNoStart();
		if (r != KErrNone)
			return r == KErrNotFound || r == KErrServerTerminated ? KErrNotFound : r;
		iTtyOpen = ETrue;
		}
	TInt r = KErrArgument;
	switch (aOp)
		{
	case EPhTtyList:
		{
		TUint8 ids[KPheTtyMax];
		const TInt n = iTty.List(ids, KPheTtyMax);
		if (n < 0)
			{ r = n; break; }
		for (TInt i = 0; i < n; ++i)
			{
			TPheTtyInfoBuf info;
			if (iTty.Info(ids[i], info()) != KErrNone)
				continue;
			AppendNumber(iRep, info().iId); iRep.Append(' ');
			AppendNumber(iRep, info().iCols); iRep.Append(' ');
			AppendNumber(iRep, info().iRows); iRep.Append(' ');
			AppendNumber(iRep, info().iShell); iRep.Append(' ');
			AppendNumber(iRep, info().iEndSeq); iRep.Append(' ');
			AppendUtf8(iRep, info().iTitle);
			iRep.Append('\n');
			}
		r = KErrNone;
		break;
		}
	case EPhTtyNew:
		if (aLen < 8)
			break;
		r = iTty.New((TInt)Le32(aP), (TInt)Le32(aP + 4));
		if (r >= 0)
			{ AppendLe32(iRep, (TUint32)r); r = KErrNone; }
		break;
	case EPhTtyRead:
		{
		if (aLen < 8)
			break;
		TPtr8 data = iDataBuf->Des();
		data.SetLength(0);
		TPtr8 buf((TUint8*)data.Ptr(), 0, Min(data.MaxLength(), 16 * 1024));
		TRequestStatus st;
		iTty.Read((TInt)Le32(aP), buf, Le32(aP + 4), EFalse, st);
		User::WaitForRequest(st);
		r = st.Int();
		if (r >= 0)
			{ iRep.Append(buf); r = KErrNone; }			// [u32 seq][bytes], as phettyd wrote it
		break;
		}
	case EPhTtyWrite:
		if (aLen < 4)
			break;
		r = KErrNone;
		for (TInt i = 4; i < aLen && r == KErrNone; i += 256)	// phettyd takes 256 bytes per call (a paste)
			r = iTty.Bytes((TInt)Le32(aP), TPtrC8(aP + i, Min(256, aLen - i)));
		break;
	case EPhTtyResize:
		if (aLen < 12)
			break;
		r = iTty.Resize((TInt)Le32(aP), (TInt)Le32(aP + 4), (TInt)Le32(aP + 8));
		break;
	case EPhTtyKill:
		if (aLen < 4)
			break;
		r = iTty.Kill((TInt)Le32(aP));
		break;
		}
	if (r == KErrServerTerminated)
		{ iTty.Close(); iTtyOpen = EFalse; r = KErrNotFound; }	// phettyd went away: reconnect next time
	return r;
	}

TInt E32Main()
	{
	__UHEAP_MARK;
	CTrapCleanup* cleanup = CTrapCleanup::New();
	TInt r = KErrNoMemory;
	if (cleanup)
		{
		CPhFs* s = NULL;
		TRAP(r, s = CPhFs::NewL());
		if (r == KErrNone)
			{
			RDebug::Printf("[phfs] serving the debug link's file requests");
			s->Run();
			delete s;
			}
		else
			RDebug::Printf("[phfs] start failed %d", r);
		delete cleanup;
		}
	__UHEAP_MARKEND;
	return r;
	}
