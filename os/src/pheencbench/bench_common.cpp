// bench_common.cpp -- OURS (pheencbench): the test content, colour conversion (scalar + NEON) and PSNR. C++03.
#include "bench_common.h"
#include <string.h>
#include <math.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define PB_NEON 1
#endif

namespace pb {

// ---------------------------------------------------------------- content
static uint64_t Hash(uint32_t a, uint32_t b, uint32_t c)
	{
	uint64_t h = 0x9E3779B97F4A7C15ull ^ ((uint64_t)a * 0xBF58476D1CE4E5B9ull) ^ ((uint64_t)b << 21) ^ ((uint64_t)c << 42);
	h ^= h >> 31;
	h *= 0x94D049BB133111EBull;
	h ^= h >> 29;
	return h;
	}

static void FillRect(uint32_t* aBuf, int aW, int aH, int aX, int aY, int aRw, int aRh, uint32_t aColour)
	{
	if (aX < 0) { aRw += aX; aX = 0; }
	if (aY < 0) { aRh += aY; aY = 0; }
	if (aX + aRw > aW) aRw = aW - aX;
	if (aY + aRh > aH) aRh = aH - aY;
	for (int r = 0; r < aRh; ++r)
		{
		uint32_t* p = aBuf + (aY + r) * aW + aX;
		for (int c = 0; c < aRw; ++c)
			p[c] = aColour;
		}
	}

// a line of "text": glyph cells of 5x7 strokes from a hash (many small high-contrast edges, like real text to an encoder)
static void TextLine(uint32_t* aBuf, int aW, int aH, int aX, int aY, int aGlyphs, int aStroke, uint32_t aColour, uint32_t aSeed)
	{
	const int adv = 6 * aStroke;
	for (int g = 0; g < aGlyphs; ++g)
		{
		if ((Hash(aSeed, g, 7) & 7) == 0)
			continue;							// a space
		const uint64_t bits = Hash(aSeed, g, 1);
		for (int i = 0; i < 35; ++i)
			if ((bits >> i) & 1)
				FillRect(aBuf, aW, aH, aX + g * adv + (i % 5) * aStroke, aY + (i / 5) * aStroke, aStroke, aStroke, aColour);
		}
	}

void GenXrgb(int aW, int aH, int aK, uint32_t* aDst)
	{
	FillRect(aDst, aW, aH, 0, 0, aW, aH, 0xF4F4F8u);
	const int stroke = aW >= 600 ? 3 : (aW >= 300 ? 2 : 1);
	const int cell = 7 * stroke;
	const int hdr = aH / 12;
	FillRect(aDst, aW, aH, 0, 0, aW, hdr, 0x1E6FD9u);
	TextLine(aDst, aW, aH, aW / 16, hdr / 2 - cell / 2, 10, stroke, 0xFFFFFFu, 99);
	const int rowH = cell * 4 + 2 * stroke;
	for (int i = 0; hdr + (i + 1) * rowH < aH; ++i)
		{
		const int y0 = hdr + i * rowH;
		FillRect(aDst, aW, aH, 0, y0 + rowH - 1, aW, 1, 0xD0D0D8u);
		const uint32_t ic = 0x404040u + (uint32_t)(Hash(i, 3, 3) & 0xBFBFBFu);
		FillRect(aDst, aW, aH, aW / 20, y0 + stroke, rowH - 2 * stroke - 1, rowH - 2 * stroke - 1, ic & 0xFFFFFFu);
		const int tx = aW / 20 + rowH + aW / 40;
		const int maxGlyphs = (aW - tx - aW / 20) / (6 * stroke);
		TextLine(aDst, aW, aH, tx, y0 + stroke, (int)(maxGlyphs * (45 + Hash(i, 1, 1) % 40) / 100), stroke, 0x202020u, (uint32_t)(i * 2 + 1));
		TextLine(aDst, aW, aH, tx, y0 + stroke * 2 + cell, (int)(maxGlyphs * (30 + Hash(i, 2, 2) % 50) / 100), stroke, 0x808088u, (uint32_t)(i * 2 + 2));
		}
	// the moving region: a gradient box with a checker corner, drifting right and down the screen; periodic in KRing frames
	const int bw = aW / 4, bh = aH / 10;
	const int ph = aK % KRing;
	const int bx = (ph * (aW - bw - 8)) / KRing + 4;
	const int by = aH / 3 + (ph * aH / 40);
	for (int r = 0; r < bh; ++r)
		for (int c = 0; c < bw; ++c)
			{
			uint32_t px = ((uint32_t)(40 + c * 200 / bw) << 16) | ((uint32_t)(60 + r * 150 / bh) << 8) | 0xA0u;
			if (c < bw / 3 && r < bh / 3 && (((c / stroke / 2) + (r / stroke / 2)) & 1))
				px = 0xFFFFFFu;
			if (bx + c >= 0 && bx + c < aW && by + r < aH)
				aDst[(by + r) * aW + bx + c] = px;
			}
	}

void GenStress(int aW, int aH, int aK, uint32_t* aDst)
	{
	const int s = aW / 8;
	const int vx = aW / 160, vy = aH / 240;
	const int ph = aK % KRing;
	const int x0 = (16 + (ph * vx * 3) % (aW - s - 32)) & ~1, y0 = (16 + (ph * vy * 3) % (aH - s - 32)) & ~1;
	const int barY = aH * 3 / 4;
	for (int r = 0; r < aH; ++r)
		for (int c = 0; c < aW; ++c)
			{
			const int g = 30 + (c * 150) / aW + (r * 60) / aH;
			uint32_t px = ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)(g + 20 > 255 ? 255 : g + 20);
			if (r >= barY && r < barY + 4)
				px = 0x1E1E1Eu;
			if (r >= y0 && r < y0 + s && c >= x0 && c < x0 + s)
				px = ((((c - x0) >> 3) + ((r - y0) >> 3)) & 1) ? 0xE1E1E1u : (r < y0 + s / 2 ? 0x4646A0u : 0x464646u);
			aDst[r * aW + c] = px;
			}
	}

void ToRgb565(const uint32_t* aSrc, int aW, int aH, uint16_t* aDst)
	{
	for (int i = 0; i < aW * aH; ++i)
		{
		const uint32_t p = aSrc[i];
		aDst[i] = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
		}
	}

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

int PsnrY(const uint8_t* aA, int aSa, const uint8_t* aB, int aSb, int aW, int aH)
	{
	uint64_t sse = 0;
	for (int r = 0; r < aH; ++r)
		{
		const uint8_t* a = aA + r * aSa;
		const uint8_t* b = aB + r * aSb;
		uint32_t row = 0;
		for (int c = 0; c < aW; ++c)
			{
			const int d = a[c] - b[c];
			row += (uint32_t)(d * d);
			}
		sse += row;
		}
	if (!sse)
		return 9999;
	const double mse = (double)sse / ((double)aW * aH);
	return (int)(1000.0 * log10(255.0 * 255.0 / mse) + 0.5);
	}

}	// namespace pb
