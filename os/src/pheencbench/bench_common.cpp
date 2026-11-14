// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// bench_common.cpp -- OURS (pheencbench): the test content and PSNR (the colour conversion is csc.cpp). C++03.
#include "bench_common.h"
#include <string.h>
#include <math.h>

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
