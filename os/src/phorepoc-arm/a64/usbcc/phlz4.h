/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

// phlz4.h -- OURS: a minimal LZ4 *block* compressor (no libc, no allocation, header-only) for the PhoneLab live
// view. Output is the standard LZ4 block format (any decoder reads it: the Go
// side is tools/phorepoc-arm/phonelab/server/internal/lz4). Input must be <= 65535 bytes (offsets are 16 bits and the
// hash table stores 16-bit positions); the live view feeds it one tile (<= 3072 bytes).
// Single pass, greedy, one 4096-entry hash table of the last position seen per 4-byte hash. The table is NOT cleared
// between calls: every candidate is verified (bytes compared, position < current), so stale entries only cost a miss.
#ifndef PHLZ4_H
#define PHLZ4_H

#define PHLZ4_BOUND(n) ((n) + (n) / 255 + 16)	// the worst case output size for n input bytes

typedef struct { unsigned short iPos[4096]; } TPhLz4Table;

static inline unsigned PhLz4Read32(const unsigned char* p)
	{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
	}

static inline unsigned char* PhLz4Len(unsigned char* op, unsigned rem)	// the 255-run length extension
	{
	while (rem >= 255) { *op++ = 255; rem -= 255; }
	*op++ = (unsigned char)rem;
	return op;
	}

// Returns the compressed size (>= 1). aDst must hold PHLZ4_BOUND(aN) bytes.
static inline int PhLz4Compress(const unsigned char* aSrc, int aN, unsigned char* aDst, TPhLz4Table* aT)
	{
	const unsigned char* ip = aSrc;
	const unsigned char* anchor = aSrc;
	const unsigned char* const iend = aSrc + aN;
	unsigned char* op = aDst;
	if (aN >= 13)
		{
		const unsigned char* const mflimit = iend - 12;		// the last match starts before here
		const unsigned char* const mlimit = iend - 5;		// ... and the last 5 bytes are literals
		while (ip < mflimit)
			{
			const unsigned v = PhLz4Read32(ip);
			const unsigned h = (v * 2654435761u) >> 20;
			const unsigned char* cand = aSrc + aT->iPos[h];
			aT->iPos[h] = (unsigned short)(ip - aSrc);
			if (cand < ip && PhLz4Read32(cand) == v)
				{
				while (ip > anchor && cand > aSrc && ip[-1] == cand[-1]) { --ip; --cand; }
				unsigned ml = 4;
				while (ip + ml < mlimit && ip[ml] == cand[ml]) ++ml;
				const unsigned lit = (unsigned)(ip - anchor);
				unsigned char* tok = op++;
				*tok = (unsigned char)((lit >= 15 ? 15 : lit) << 4);
				if (lit >= 15) op = PhLz4Len(op, lit - 15);
				for (unsigned i = 0; i < lit; ++i) *op++ = anchor[i];
				const unsigned off = (unsigned)(ip - cand);
				*op++ = (unsigned char)off; *op++ = (unsigned char)(off >> 8);
				if (ml - 4 >= 15) { *tok |= 15; op = PhLz4Len(op, ml - 4 - 15); }
				else *tok |= (unsigned char)(ml - 4);
				ip += ml;
				anchor = ip;
				continue;
				}
			++ip;
			}
		}
	const unsigned lit = (unsigned)(iend - anchor);			// the final literals
	*op++ = (unsigned char)((lit >= 15 ? 15 : lit) << 4);
	if (lit >= 15) op = PhLz4Len(op, lit - 15);
	for (unsigned i = 0; i < lit; ++i) *op++ = anchor[i];
	return (int)(op - aDst);
	}

#endif
