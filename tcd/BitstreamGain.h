#pragma once

// Level changes in the frames of P25 (IMBE 4400) and M17 (Codec2 3200 and
// 1600) without decoding and coding them again, for P25 -> P25 and M17 -> M17
// (P25GainMode, M17GainMode = bitstream), as CAmbeGain does for DMR and
// DStarAmbe::Gain (AmbeGain.h) for D-Star.
//
// IMBE: tcd's P25 frame is the 88 parameter bits û0..û7 (most significant bit
// of byte 0 first, no FEC). b2 (6 bits) quantises G1, the mean of the log2
// spectral amplitudes: the decoder's prediction from the frame before has its
// mean removed (TIA-102.BABA, eq. 75-79), so log2 Ml of every harmonic moves
// with G1, one unit 6.02 dB, with no memory. b0 (8 bits: û0 bits 11..6, then
// the bits 85 and 86) gives L; b2's bits 5..3 are bits 6..8, bit 0 is bit 84,
// bits 2 and 1 sit where the bit prioritisation puts them for that L.
// References: mbelib-neo src/imbe/imbe7200x4400.c (imbe_decode_fundamental,
// imbe_decode_gains, imbe_update_spectral_amplitudes) and
// internal/imbe7200x4400_const.h (B2, bo).
//
// Codec2: the frame energy is 5 bits, a uniform quantiser in dB from -10 to
// +40 (decoded at 50/32 = 1.5625 dB per step), Gray coded. 3200: one per 20 ms
// frame, bits 9..13; 1600: two per 40 ms frame, bits 9..13 (first 20 ms) and
// 23..27 (second 20 ms). Reference: codec2/codec2.cpp (codec2_encode_3200,
// codec2_encode_1600), codec2/quantise.cpp (encode_energy, decode_energy).

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cstring>
#include "LevelTable.h"	// NearestLevel
#include "YSFDefines.h"	// READ_BIT, WRITE_BIT

// a bit of a frame, the most significant bit of byte 0 first
namespace MsbBits
{
	static inline int Get(const uint8_t *f, int i) { return READ_BIT(f, i) ? 1 : 0; }
	static inline void Put(uint8_t *f, int i, int b) { WRITE_BIT(f, i, b); }
}

namespace ImbeGain
{
	// G1 per b2 (mbelib-neo imbe7200x4400_const.h, B2)
	static const float B2[64] = {
		-2.842205f, -2.694235f, -2.558260f, -2.382850f, -2.221042f, -2.095574f, -1.980845f, -1.836058f,
		-1.645556f, -1.417658f, -1.261301f, -1.125631f, -0.958207f, -0.781591f, -0.555837f, -0.346976f,
		-0.147249f, 0.027755f, 0.211495f, 0.388380f, 0.552873f, 0.737223f, 0.932197f, 1.139032f,
		1.320955f, 1.483433f, 1.648297f, 1.801447f, 1.942731f, 2.118613f, 2.321486f, 2.504443f,
		2.653909f, 2.780654f, 2.925355f, 3.076390f, 3.220825f, 3.402869f, 3.585096f, 3.784606f,
		3.955521f, 4.155636f, 4.314009f, 4.444150f, 4.577542f, 4.735552f, 4.909493f, 5.085264f,
		5.254767f, 5.411894f, 5.568094f, 5.738523f, 5.919215f, 6.087701f, 6.280685f, 6.464201f,
		6.647736f, 6.834672f, 7.022583f, 7.211777f, 7.471016f, 7.738948f, 8.124863f, 8.695827f
	};

	using MsbBits::Get;
	using MsbBits::Put;

	// the bits of b2, its bit 5 first, for b0; false for a frame that is no voice
	// (b0 208..255: silence, tones, invalid)
	static inline bool B2Bits(const uint8_t *frame, int pos[6])
	{
		static const int b0_bits[8] = { 0, 1, 2, 3, 4, 5, 85, 86 };
		int b0 = 0;
		for (int i : b0_bits)
			b0 = (b0 << 1) | Get(frame, i);
		if (b0 > 207)
			return false;
		const float w0 = float(4.0 * M_PI) / (float(b0) + 39.5f);
		const int L = int(0.9254f * float(int(float(M_PI) / w0 + 0.25f)));
		if (L < 9 || L > 56)
			return false;
		// bits 2 and 1 of b2 by the prioritisation for L (bo in mbelib-neo): 51 for
		// L = 9, one further for every three harmonics more, 60 from L = 34 on
		const int p = 51 + std::min((L - 9 + 2) / 3, 9);
		const int at[6] = { 6, 7, 8, p, p + 1, 84 };
		std::copy(at, at + 6, pos);
		return true;
	}

	// the silence frame that fills lost P25 frames (MMDVMHost, P25Gateway, urfd,
	// tcd): left as it is, gateways know it by its bytes
	static const uint8_t SILENCE[11] = { 0x04U, 0x0CU, 0xFDU, 0x7BU, 0xFBU, 0x7DU, 0xF2U, 0x7BU, 0x3DU, 0x9EU, 0x45U };

	// change the decoded level of an 11-byte P25 frame by gainDb
	static inline void Process(uint8_t *frame, float gainDb)
	{
		if (0 == memcmp(frame, SILENCE, sizeof(SILENCE)))
			return;
		int pos[6];
		if (!B2Bits(frame, pos))
			return;
		int b2 = 0;
		for (int k = 0; k < 6; k++)
			b2 = (b2 << 1) | Get(frame, pos[k]);
		const int best = NearestLevel(B2, 64, b2, gainDb / 6.0206f);
		if (best != b2)
			for (int k = 0; k < 6; k++)
				Put(frame, pos[k], (best >> (5 - k)) & 1);
	}
}

namespace Codec2Gain
{
	constexpr float kStepDb = 50.0f / 32.0f;

	// the Codec2 3200 silence frame (M17 gateways, urfd, tcd): left as it is,
	// gateways know it by its bytes
	static const uint8_t SILENCE3200[8] = { 0x01U, 0x00U, 0x09U, 0x43U, 0x9CU, 0xE4U, 0x21U, 0x08U };

	// the energy steps a gain comes to; 0: the frame stays as it is
	static inline int Steps(float gainDb) { return int(lroundf(gainDb / kStepDb)); }

	// change the 5-bit Gray coded energy at bit `at` by gainDb
	static inline void Energy(uint8_t *bits, int at, float gainDb)
	{
		const int steps = Steps(gainDb);
		if (steps == 0)
			return;
		unsigned int g = 0;
		for (int k = 0; k < 5; k++)
			g = (g << 1) | unsigned(MsbBits::Get(bits, at + k));
		unsigned int e = g;	// Gray to binary
		e ^= e >> 1; e ^= e >> 2; e ^= e >> 4;
		const unsigned int n = (unsigned int)std::clamp(int(e) + steps, 0, 31);
		g = n ^ (n >> 1);
		for (int k = 0; k < 5; k++)
			MsbBits::Put(bits, at + k, int((g >> (4 - k)) & 1U));
	}

	// where the energy is: in a 3200 frame (20 ms, 8 bytes), and the two of a
	// 1600 frame's 8 voice bytes (40 ms: its first and its second 20 ms)
	constexpr int k3200 = 9, k1600First = 9, k1600Second = 23;

	// a 3200 frame (8 bytes) by gainDb, the silence frame left as it is
	static inline void Gain3200(uint8_t *frame, float gainDb)
	{
		if (0 != memcmp(frame, SILENCE3200, sizeof(SILENCE3200)))
			Energy(frame, k3200, gainDb);
	}
}
