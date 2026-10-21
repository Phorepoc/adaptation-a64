/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// phfs_api.h -- OURS (2026-10-11, PhoneLab Project panel): the user/kernel contract of the file-service LDD "PhFs.A64"
// (phfs_ldd.cpp). The debug link (phlink.cpp) is kernel side and has no file server; the one client of this LDD is the
// user-side helper src/phfs (phfs.exe), which asks for the next request, runs it against the file server and replies.
//  * Get (async): completes with {op LE32, payload...} when phlink has received an 'F' command from the PC;
//  * Reply (control): a descriptor {status LE32, data...}, sent back to the PC as a blob frame (kind 4).
// Request ops and payloads (phlink 'F' command; all integers little endian, paths UTF-8, "/" separated, "/C:/dir/file"):
//   EPhFsList  1: path                              -> text, one line per entry "d|f <size> <mtime unix s> <name>\n"
//   EPhFsRead  2: offset u32, len u32, path         -> data (status 0), the file's bytes [offset, offset+len)
//   EPhFsWrite 3: offset u32, flags u32 (1 = truncate), pathlen u32, path, data -> (status 0) bytes written as u32
//   EPhFsDelete 4: path                            -> (status 0) a file is deleted, a folder removed with its contents
// Terminal requests (2026-10-19): bridged to the phetty daemon (src/phetty, RPheTty); KErrNotFound = phettyd not running.
//   EPhTtyList   5: flags u32 (1 = start phettyd)  -> text, one line per tty "<id> <cols> <rows> <shell> <endseq> <title>\n"
//   EPhTtyNew    6: cols u32, rows u32              -> id u32
//   EPhTtyRead   7: id u32, from seq u32            -> seq u32 of the first byte, then the bytes (no wait; may be empty)
//   EPhTtyWrite  8: id u32, bytes                   -> (status 0) raw terminal input
//   EPhTtyResize 9: id u32, cols u32, rows u32      -> (status 0)
//   EPhTtyKill  10: id u32                          -> (status 0)
#ifndef PHFS_API_H
#define PHFS_API_H
#include <e32cmn.h>
#include <e32ver.h>
#ifndef __KERNEL_MODE__
#include <e32std.h>
#endif

_LIT(KPhFsLddName, "PhFs.A64");

enum TPhFsOp { EPhFsList = 1, EPhFsRead = 2, EPhFsWrite = 3, EPhFsDelete = 4,
	EPhTtyList = 5, EPhTtyNew = 6, EPhTtyRead = 7, EPhTtyWrite = 8, EPhTtyResize = 9, EPhTtyKill = 10 };
const TInt KPhFsMaxRequest = 4096 + 512;		// payload bytes phlink accepts (a 3 KB write chunk + header + path)
const TInt KPhFsMaxReply = 60 * 1024;			// status + data

class RPhFs : public RBusLogicalChannel
	{
public:
	enum { EMajorVersion = 1 };
	enum TControl { EControlReply = 0 };
	enum TRequest { ERequestGet = 0 };
#ifndef __KERNEL_MODE__
	inline TInt Open()
		{ return DoCreate(KPhFsLddName, TVersion(EMajorVersion, 0, 0), KNullUnit, NULL, NULL, EOwnerThread); }
	inline void Get(TRequestStatus& aStatus, TDes8& aRequest)	{ DoRequest(ERequestGet, aStatus, &aRequest); }
	inline void GetCancel()										{ DoCancel(1u << ERequestGet); }
	inline TInt Reply(const TDesC8& aReply)						{ return DoControl(EControlReply, (TAny*)&aReply); }
#endif
	};

#endif
