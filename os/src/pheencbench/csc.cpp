// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// csc.cpp -- OURS: RGB -> YUV 4:2:0 colour conversion (BT.601 limited range, scalar + NEON), shared by pheencbench and the
// screen recorder in phfs (pherec.cpp). Plain C++03 without libc headers: memcpy comes from euser or libc alike.
#include "csc.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define PB_NEON 1
#endif
#define memcpy __builtin_memcpy

namespace pb {

// ---------------------------------------------------------------- colour conversion
static inline void Px(const uint8_t* aP, int aX, bool a565, int& r, int& g, int& b)
	{
	if (a565)
		{
		const uint32_t v = ((const uint16_t*)aP)[aX];
		r = (v >> 11) & 31; r = (r << 3) | (r >> 2);
		g = (v >> 5) & 63;  g = (g << 2) | (g >> 4);
		b = v & 31;         b = (b << 3) | (b >> 2);
		}
	else
		{
		const uint32_t v = ((const uint32_t*)aP)[aX];
		r = (v >> 16) & 255; g = (v >> 8) & 255; b = v & 255;
		}
	}

// scalar: columns [aX0, aW) of all rows
static void CscScalar(bool a565, const uint8_t* aSrc, int aSs, int aX0, int aW, int aH, uint8_t* aY, int aYs, uint8_t* aU, uint8_t* aV, int aCs)
	{
	for (int r = 0; r < aH; r += 2)
		{
		const uint8_t* s0 = aSrc + r * aSs;
		const uint8_t* s1 = s0 + aSs;
		uint8_t* y0 = aY + r * aYs;
		uint8_t* y1 = y0 + aYs;
		for (int c = aX0; c < aW; c += 2)
			{
			int rs = 0, gs = 0, bs = 0;
			for (int k = 0; k < 4; ++k)
				{
				int rr, gg, bb;
				Px((k & 2) ? s1 : s0, c + (k & 1), a565, rr, gg, bb);
				((k & 2) ? y1 : y0)[c + (k & 1)] = (uint8_t)(((66 * rr + 129 * gg + 25 * bb + 4224) >> 8));
				rs += rr; gs += gg; bs += bb;
				}
			rs = (rs + 2) >> 2; gs = (gs + 2) >> 2; bs = (bs + 2) >> 2;
			int u = ((-38 * rs - 74 * gs + 112 * bs + 128) >> 8) + 128;
			int v = ((112 * rs - 94 * gs - 18 * bs + 128) >> 8) + 128;
			u = u < 0 ? 0 : (u > 255 ? 255 : u);
			v = v < 0 ? 0 : (v > 255 ? 255 : v);
			if (aV)
				{
				aU[(r / 2) * aCs + c / 2] = (uint8_t)u;
				aV[(r / 2) * aCs + c / 2] = (uint8_t)v;
				}
			else
				{
				aU[(r / 2) * aCs + c] = (uint8_t)u;
				aU[(r / 2) * aCs + c + 1] = (uint8_t)v;
				}
			}
		}
	}

#ifdef PB_NEON
struct TXrgbLoad
	{
	static const int KBytes = 4;
	static inline void Load(const uint8_t* aP, uint8x8_t& r, uint8x8_t& g, uint8x8_t& b)
		{
		const uint8x8x4_t v = vld4_u8(aP);
		b = v.val[0]; g = v.val[1]; r = v.val[2];
		}
	};
struct T565Load
	{
	static const int KBytes = 2;
	static inline void Load(const uint8_t* aP, uint8x8_t& r, uint8x8_t& g, uint8x8_t& b)
		{
		const uint16x8_t v = vld1q_u16((const uint16_t*)aP);
		const uint8x8_t r5 = vmovn_u16(vshrq_n_u16(v, 11));
		const uint8x8_t g6 = vand_u8(vmovn_u16(vshrq_n_u16(v, 5)), vdup_n_u8(63));
		const uint8x8_t b5 = vand_u8(vmovn_u16(v), vdup_n_u8(31));
		r = vorr_u8(vshl_n_u8(r5, 3), vshr_n_u8(r5, 2));
		g = vorr_u8(vshl_n_u8(g6, 2), vshr_n_u8(g6, 4));
		b = vorr_u8(vshl_n_u8(b5, 3), vshr_n_u8(b5, 2));
		}
	};

static inline uint8x8_t LumaN(uint8x8_t r, uint8x8_t g, uint8x8_t b)
	{
	uint16x8_t t = vmull_u8(r, vdup_n_u8(66));
	t = vmlal_u8(t, g, vdup_n_u8(129));
	t = vmlal_u8(t, b, vdup_n_u8(25));
	return vshrn_n_u16(vaddq_u16(t, vdupq_n_u16(4224)), 8);
	}

// 16 columns x 2 rows
template <class L>
static inline void Block16(const uint8_t* aS0, const uint8_t* aS1, uint8_t* aY0, uint8_t* aY1, uint8_t* aU, uint8_t* aV, bool aPlanar)
	{
	uint8x8_t r0[2], g0[2], b0[2], r1[2], g1[2], b1[2];
	for (int h = 0; h < 2; ++h)
		{
		L::Load(aS0 + h * 8 * L::KBytes, r0[h], g0[h], b0[h]);
		L::Load(aS1 + h * 8 * L::KBytes, r1[h], g1[h], b1[h]);
		vst1_u8(aY0 + h * 8, LumaN(r0[h], g0[h], b0[h]));
		vst1_u8(aY1 + h * 8, LumaN(r1[h], g1[h], b1[h]));
		}
	// 2x2 sums: 4 chroma samples per 8-pixel half, 8 per block
	const uint16x8_t rs = vcombine_u16(vadd_u16(vpaddl_u8(r0[0]), vpaddl_u8(r1[0])), vadd_u16(vpaddl_u8(r0[1]), vpaddl_u8(r1[1])));
	const uint16x8_t gs = vcombine_u16(vadd_u16(vpaddl_u8(g0[0]), vpaddl_u8(g1[0])), vadd_u16(vpaddl_u8(g0[1]), vpaddl_u8(g1[1])));
	const uint16x8_t bs = vcombine_u16(vadd_u16(vpaddl_u8(b0[0]), vpaddl_u8(b1[0])), vadd_u16(vpaddl_u8(b0[1]), vpaddl_u8(b1[1])));
	const int16x8_t r = vreinterpretq_s16_u16(vmovl_u8(vrshrn_n_u16(rs, 2)));
	const int16x8_t g = vreinterpretq_s16_u16(vmovl_u8(vrshrn_n_u16(gs, 2)));
	const int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(vrshrn_n_u16(bs, 2)));
	int16x8_t u = vmulq_n_s16(b, 112);
	u = vmlaq_n_s16(u, r, -38);
	u = vmlaq_n_s16(u, g, -74);
	int16x8_t v = vmulq_n_s16(r, 112);
	v = vmlaq_n_s16(v, g, -94);
	v = vmlaq_n_s16(v, b, -18);
	const uint8x8_t u8 = vqmovun_s16(vaddq_s16(vrshrq_n_s16(u, 8), vdupq_n_s16(128)));
	const uint8x8_t v8 = vqmovun_s16(vaddq_s16(vrshrq_n_s16(v, 8), vdupq_n_s16(128)));
	if (aPlanar)
		{
		vst1_u8(aU, u8);
		vst1_u8(aV, v8);
		}
	else
		{
		uint8x8x2_t uv;
		uv.val[0] = u8;
		uv.val[1] = v8;
		vst2_u8(aU, uv);
		}
	}

template <class L>
static void CscNeon(const uint8_t* aSrc, int aSs, int aW, int aH, uint8_t* aY, int aYs, uint8_t* aU, uint8_t* aV, int aCs)
	{
	const int w16 = aW & ~15;
	for (int r = 0; r < aH; r += 2)
		{
		const uint8_t* s0 = aSrc + r * aSs;
		uint8_t* y0 = aY + r * aYs;
		uint8_t* cu = aU + (r / 2) * aCs;
		uint8_t* cv = aV ? aV + (r / 2) * aCs : 0;
		for (int c = 0; c < w16; c += 16)
			Block16<L>(s0 + c * L::KBytes, s0 + aSs + c * L::KBytes, y0 + c, y0 + aYs + c, aV ? cu + c / 2 : cu + c, cv ? cv + c / 2 : 0, aV != 0);
		}
	}
#endif

void Csc(TSrc aFmt, const void* aSrc, int aSs, int aW, int aH, uint8_t* aY, int aYs, uint8_t* aU, uint8_t* aV, int aCs, bool aNeon)
	{
	int x0 = 0;
#ifdef PB_NEON
	if (aNeon)
		{
		if (aFmt == EXrgb)
			CscNeon<TXrgbLoad>((const uint8_t*)aSrc, aSs, aW, aH, aY, aYs, aU, aV, aCs);
		else
			CscNeon<T565Load>((const uint8_t*)aSrc, aSs, aW, aH, aY, aYs, aU, aV, aCs);
		x0 = aW & ~15;
		}
#else
	(void)aNeon;
#endif
	if (x0 < aW)
		CscScalar(aFmt == ERgb565, (const uint8_t*)aSrc, aSs, x0, aW, aH, aY, aYs, aU, aV, aCs);
	}

void PadNv12(uint8_t* aY, uint8_t* aUv, int aW, int aH, int aAW, int aAH)
	{
	for (int r = 0; r < aH; ++r)
		for (int c = aW; c < aAW; ++c)
			aY[r * aAW + c] = aY[r * aAW + aW - 1];
	for (int r = aH; r < aAH; ++r)
		memcpy(aY + r * aAW, aY + (aH - 1) * aAW, aAW);
	const int cw = aW / 2, ch = aH / 2;			// chroma samples (pairs)
	for (int r = 0; r < ch; ++r)
		for (int c = cw; c < aAW / 2; ++c)
			{
			aUv[r * aAW + 2 * c] = aUv[r * aAW + 2 * cw - 2];
			aUv[r * aAW + 2 * c + 1] = aUv[r * aAW + 2 * cw - 1];
			}
	for (int r = ch; r < aAH / 2; ++r)
		memcpy(aUv + r * aAW, aUv + (ch - 1) * aAW, aAW);
	}

}	// namespace pb
