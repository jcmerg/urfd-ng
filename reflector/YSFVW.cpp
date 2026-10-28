//  Copyright (C) 2010,2014,2016,2018 by Jonathan Naylor G4KLX (the IMBE FEC, MMDVMHost AMBEFEC)
//
//  urfd -- The universal reflector
//  Copyright © 2026
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <cstring>

#include "YSFVW.h"
#include "YSFPayload.h"
#include "Golay24128.h"
#include "Hamming.h"

const uint8_t IMBE11_SILENCE[11] = { 0x04U, 0x0CU, 0xFDU, 0x7BU, 0xFBU, 0x7DU, 0xF2U, 0x7BU, 0x3DU, 0x9EU, 0x45U };

static const unsigned IMBE_INTERLEAVE[144] = {
	0,  7, 12, 19, 24, 31, 36, 43, 48, 55, 60, 67, 72, 79, 84, 91,  96, 103, 108, 115, 120, 127, 132, 139,
	1,  6, 13, 18, 25, 30, 37, 42, 49, 54, 61, 66, 73, 78, 85, 90,  97, 102, 109, 114, 121, 126, 133, 138,
	2,  9, 14, 21, 26, 33, 38, 45, 50, 57, 62, 69, 74, 81, 86, 93,  98, 105, 110, 117, 122, 129, 134, 141,
	3,  8, 15, 20, 27, 32, 39, 44, 51, 56, 63, 68, 75, 80, 87, 92,  99, 104, 111, 116, 123, 128, 135, 140,
	4, 11, 16, 23, 28, 35, 40, 47, 52, 59, 64, 71, 76, 83, 88, 95, 100, 107, 112, 119, 124, 131, 136, 143,
	5, 10, 17, 22, 29, 34, 41, 46, 53, 58, 65, 70, 77, 82, 89, 94, 101, 106, 113, 118, 125, 130, 137, 142
};

static const unsigned OFFSETS5[5] = { 0U, 18U, 36U, 54U, 72U };
static const unsigned OFFSETS2[2] = { 54U, 72U };

static bool GetBit(const uint8_t *p, unsigned i)        { return (p[i >> 3] & (0x80U >> (i & 7U))) != 0U; }
static void SetBit(uint8_t *p, unsigned i, bool b)      { if (b) p[i >> 3] |= (0x80U >> (i & 7U)); else p[i >> 3] &= ~(0x80U >> (i & 7U)); }

static unsigned Word(const bool *b, unsigned n)
{
	unsigned w = 0U;
	for (unsigned i = 0U; i < n; i++)
		w = (w << 1) | (b[i] ? 1U : 0U);
	return w;
}

static void PutWord(bool *b, unsigned n, unsigned w)
{
	for (unsigned i = 0U; i < n; i++)
		b[i] = ((w >> (n - 1U - i)) & 1U) != 0U;
}

// the 23-bit codeword: CGolay24128::encode23127 returns it shifted up by one (24 bits, as
// MMDVMHost's AMBEFEC writes 24 bits and overwrites the last one with the next word)
static unsigned Golay23(unsigned data12)	{ return CGolay24128::encode23127(data12) >> 1; }

// the whitening of c1..c6 from the 12 data bits of c0
static void Whiten(bool *c, unsigned c0data)
{
	unsigned p = 16U * c0data;
	for (unsigned i = 0U; i < 114U; i++)
	{
		p = (173U * p + 13849U) % 65536U;
		c[23U + i] = c[23U + i] != (p >= 32768U);
	}
}

// the vectors u0..u7 in the de-interleaved 144 bits: start, data bits
static const unsigned U_START[8] = { 0U, 23U, 46U, 69U, 92U, 107U, 122U, 137U };
static const unsigned U_BITS[8]  = { 12U, 12U, 12U, 12U, 11U, 11U, 11U, 7U };

unsigned CYsfVW::DecodeImbe(const uint8_t *block18, uint8_t *imbe11)
{
	bool c[144];
	for (unsigned i = 0U; i < 144U; i++)
		c[i] = GetBit(block18, IMBE_INTERLEAVE[i]);

	unsigned corrected = 0U;
	// c0 first: its data bits seed the whitening of the rest
	const unsigned g0 = Word(c, 23U);
	const unsigned c0 = CGolay24128::decode23127(g0);
	const unsigned e0 = Golay23(c0);
	corrected += __builtin_popcount(e0 ^ g0);
	PutWord(c, 23U, e0);
	Whiten(c, c0);
	for (unsigned k = 1U; k < 4U; k++)
	{
		const unsigned g = Word(c + 23U * k, 23U);
		const unsigned e = Golay23(CGolay24128::decode23127(g));
		corrected += __builtin_popcount(e ^ g);
		PutWord(c + 23U * k, 23U, e);
	}
	for (unsigned k = 4U; k < 7U; k++)
	{
		bool h[15];
		memcpy(h, c + U_START[k], sizeof(h));
		if (CHamming::decode15113_1(h) && memcmp(h, c + U_START[k], sizeof(h)) != 0)
			corrected++;
		memcpy(c + U_START[k], h, sizeof(h));
	}

	memset(imbe11, 0, 11U);
	unsigned n = 0U;
	for (unsigned k = 0U; k < 8U; k++)
		for (unsigned i = 0U; i < U_BITS[k]; i++, n++)
			SetBit(imbe11, n, c[U_START[k] + i]);
	return corrected;
}

void CYsfVW::EncodeImbe(const uint8_t *imbe11, uint8_t *block18)
{
	bool c[144];
	unsigned n = 0U;
	for (unsigned k = 0U; k < 8U; k++)
		for (unsigned i = 0U; i < U_BITS[k]; i++, n++)
			c[U_START[k] + i] = GetBit(imbe11, n);

	unsigned c0 = 0U;
	for (unsigned k = 0U; k < 4U; k++)
	{
		const unsigned d = Word(c + 23U * k, 12U);
		if (k == 0U)
			c0 = d;
		PutWord(c + 23U * k, 23U, Golay23(d));
	}
	for (unsigned k = 4U; k < 7U; k++)
		CHamming::encode15113_1(c + U_START[k]);
	Whiten(c, c0);

	memset(block18, 0, 18U);
	for (unsigned i = 0U; i < 144U; i++)
		SetBit(block18, IMBE_INTERLEAVE[i], c[i]);
}

unsigned CYsfVW::Offsets(uint8_t fn, uint8_t ft, const unsigned **offsets)
{
	if (fn == 0U && ft == 1U)
	{
		*offsets = OFFSETS2;
		return 2U;
	}
	*offsets = OFFSETS5;
	return 5U;
}

// CYSFPayload writes and reads the data FR channel spread over the payload, 9 bytes every 18
// (the header's CSD1); the first VW frame has the same 45 bytes in one piece
void CYsfVW::EncodeData(const uint8_t *dt20, uint8_t *payload)
{
	uint8_t frame[120];
	memset(frame, 0, sizeof(frame));
	CYSFPayload ysf;
	ysf.writeDataFRModeData1(dt20, frame);
	for (unsigned j = 0U; j < 5U; j++)
		memcpy(payload + 9U * j, frame + 30U + 18U * j, 9U);
	memset(payload + 45U, 0, 9U);
}

bool CYsfVW::DecodeData(const uint8_t *payload, uint8_t *dt20)
{
	uint8_t frame[120];
	memset(frame, 0, sizeof(frame));
	for (unsigned j = 0U; j < 5U; j++)
		memcpy(frame + 30U + 18U * j, payload + 9U * j, 9U);
	CYSFPayload ysf;
	return ysf.readDataFRModeData1(frame, dt20);
}
