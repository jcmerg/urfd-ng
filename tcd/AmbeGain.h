#pragma once

// AMBE2+ bitstream gain via the b2 (delta-gamma) parameter, without vocoder
// decode/re-encode. Uses Golay(24,12) FEC for the A partition and Golay(23,12)
// plus the PRNG scrambling for the B partition.
//
// b2 is a 5-bit field (0-31) at voice bit positions [8,9,10,11,36].
// Bits 8-11 are in the Golay-protected A partition (u0), bit 36 is in the C
// partition. The decoder computes
//     gamma = AmbeDg[b2] + 0.5 * gamma_prev
// and the mean log2 spectral amplitude of a frame is gamma - 0.5*log2(L)
// (eq. 43 removes the mean of the inter-frame prediction), so one unit of
// gamma is 6.02 dB. AmbeDg[] is coarse at the bottom and fine at the top, so
// shifting b2 by a fixed number of steps changes the level by 3-16 dB per step
// on quiet audio and by about 1.2 dB on loud audio. CAmbeGain instead tracks
// gamma for the original and the modified stream and picks, frame by frame,
// the b2 that brings the modified gamma closest to the original one plus the
// wanted gain; the rounding error stays in the tracked gamma and is made up
// in the next frame. The result does not depend on the level.
//
// References:
//   - mbelib ambe3600x2450.c (b0/b2 extraction, gamma recursion, eq. 40-43)
//     and ambe3600x2450_const.h (AmbeDg table)
//   - ETSI TS 102 361 (DMR air interface)

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include "Golay24128.h"
#include "YSFDefines.h"	// DMR_A/B/C_TABLE (72-bit AMBE2+ partitions) and PRNG_TABLE

#define AMBE_READ_BIT(p,i)     (((p)[(i)>>3] & BIT_MASK_TABLE[(i)&7]) != 0)
#define AMBE_WRITE_BIT(p,i,b)  (p)[(i)>>3] = (b) ? ((p)[(i)>>3] | BIT_MASK_TABLE[(i)&7]) : ((p)[(i)>>3] & ~BIT_MASK_TABLE[(i)&7])

// delta gamma per b2 value (mbelib ambe3600x2450_const.h)
static const float AMBE_DG[32] = {
	-2.0f, -0.67f, 0.297941f, 0.663728f, 1.036829f, 1.438136f, 1.890077f, 2.227970f,
	2.478289f, 2.667544f, 2.793619f, 2.893261f, 3.020630f, 3.138586f, 3.237579f, 3.322570f,
	3.432367f, 3.571863f, 3.696650f, 3.814917f, 3.920932f, 4.022503f, 4.123569f, 4.228291f,
	4.370569f, 4.543700f, 4.707695f, 4.848879f, 5.056757f, 5.326468f, 5.777581f, 6.874496f
};

// The parameter fields of one 9-byte AMBE2+ frame that a gain change touches
struct SAmbeFields
{
	unsigned int u0;	// A partition data (12 bits, FEC corrected)
	unsigned int u1;	// B partition data (12 bits, unscrambled, FEC corrected)
	int b0;				// pitch / frame type (7 bits)
	int b2;				// delta gamma index (5 bits)
};

static inline SAmbeFields AmbeReadFields(const uint8_t *ambe)
{
	// de-interleave A (24 bits) and B (23 bits) partitions from the 72-bit frame
	unsigned int a_bits = 0, b_bits = 0;
	for (int i = 0; i < 24; i++)
	{
		a_bits <<= 1;
		if (AMBE_READ_BIT(ambe, DMR_A_TABLE[i]))
			a_bits |= 1;
	}
	for (int i = 0; i < 23; i++)
	{
		b_bits <<= 1;
		if (AMBE_READ_BIT(ambe, DMR_B_TABLE[i]))
			b_bits |= 1;
	}

	SAmbeFields f;
	f.u0 = CGolay24128::decode24128(a_bits);
	// B is scrambled with PRNG_TABLE[u0]
	f.u1 = CGolay24128::decode23127(b_bits ^ (PRNG_TABLE[f.u0] >> 1));
	// voice bits 0-11 are u0 bits 11..0, voice bits 24.. are the C partition
	// b0 = voice bits 0,1,2,3,37,38,39; b2 = voice bits 8,9,10,11,36
	f.b0 = (int)(((f.u0 >> 8) & 0x0F) << 3)
	     | (AMBE_READ_BIT(ambe, DMR_C_TABLE[13]) ? 4 : 0)
	     | (AMBE_READ_BIT(ambe, DMR_C_TABLE[14]) ? 2 : 0)
	     | (AMBE_READ_BIT(ambe, DMR_C_TABLE[15]) ? 1 : 0);
	f.b2 = (int)((f.u0 & 0x0F) << 1) | (AMBE_READ_BIT(ambe, DMR_C_TABLE[12]) ? 1 : 0);
	return f;
}

// Write a new b2 into the frame; A gets its FEC again, B is scrambled with the new u0
static inline void AmbeWriteB2(uint8_t *ambe, const SAmbeFields &f, int b2)
{
	if (b2 < 0) b2 = 0;
	if (b2 > 31) b2 = 31;
	unsigned int u0 = (f.u0 & 0xFF0) | ((unsigned int)b2 >> 1);

	unsigned int a_new = CGolay24128::encode24128(u0);
	unsigned int b_new = (CGolay24128::encode23127(f.u1) >> 1) ^ (PRNG_TABLE[u0] >> 1);
	for (int i = 0; i < 24; i++)
		AMBE_WRITE_BIT(ambe, DMR_A_TABLE[i], (a_new >> (23 - i)) & 1);
	for (int i = 0; i < 23; i++)
		AMBE_WRITE_BIT(ambe, DMR_B_TABLE[i], (b_new >> (22 - i)) & 1);
	AMBE_WRITE_BIT(ambe, DMR_C_TABLE[12], b2 & 1);
}

// Erasure (b0 120-123) and tone (b0 126-127) frames carry no gamma
static inline bool AmbeIsVoiceOrSilence(int b0)
{
	return (b0 < 120) || (b0 == 124) || (b0 == 125);
}

// Level independent gain on AMBE2+ streams, one gamma tracker per stream
class CAmbeGain
{
public:
	// change the decoded level of this frame by gainDb. maxStepDb > 0 limits how
	// far the applied gain may move from one frame to the next: at the bottom of
	// the table the b2 steps are 6-8 dB, and making up the rounding error in the
	// next frame lets the gain alternate between two of them, a 25-50 Hz level
	// modulation that sounds like crackling on quiet passages. Limited, the gain
	// follows within a few frames on speech and no longer jumps on quiet frames
	void Process(uint8_t *ambe, uint16_t streamId, float gainDb, float maxStepDb = 0.0f)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const auto now = std::chrono::steady_clock::now();
		auto it = m_streams.find(streamId);
		if (it == m_streams.end())
		{
			DropEndedStreams(now);
			it = m_streams.emplace(streamId, SState{}).first;
		}
		SState &st = it->second;
		st.lastUsed = now;

		SAmbeFields f = AmbeReadFields(ambe);
		if (!AmbeIsVoiceOrSilence(f.b0))
			return;

		// the decoder's gamma for the original frame, and what we want it to be
		st.gammaOrig = AMBE_DG[f.b2] + 0.5f * st.gammaOrig;
		float offset = gainDb / 6.0206f;
		if (maxStepDb > 0.0f)
		{
			const float step = maxStepDb / 6.0206f;
			offset = std::clamp(offset, st.offset - step, st.offset + step);
		}
		const float want = st.gammaOrig + offset;

		// the b2 whose gamma comes closest, given the modified stream's history
		const float base = 0.5f * st.gammaMod;
		int best = f.b2;
		float bestErr = fabsf(AMBE_DG[best] + base - want);
		for (int b = 0; b < 32; b++)
		{
			float err = fabsf(AMBE_DG[b] + base - want);
			if (err < bestErr)
			{
				bestErr = err;
				best = b;
			}
		}
		st.gammaMod = AMBE_DG[best] + base;
		st.offset = st.gammaMod - st.gammaOrig;

		if (best != f.b2)
			AmbeWriteB2(ambe, f, best);
	}

private:
	struct SState
	{
		float gammaOrig = 0.0f;
		float gammaMod = 0.0f;
		float offset = 0.0f;   // gammaMod - gammaOrig of the last frame
		std::chrono::steady_clock::time_point lastUsed{};
	};

	// streams not heard for 30 s have ended
	void DropEndedStreams(std::chrono::steady_clock::time_point now)
	{
		for (auto it = m_streams.begin(); it != m_streams.end(); )
		{
			if (now - it->second.lastUsed > std::chrono::seconds(30))
				it = m_streams.erase(it);
			else
				++it;
		}
	}

	std::mutex m_mutex;
	std::unordered_map<uint16_t, SState> m_streams;
};

#undef AMBE_READ_BIT
#undef AMBE_WRITE_BIT
