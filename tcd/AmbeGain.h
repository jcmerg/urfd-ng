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
#include <cstring>
#include <cmath>
#include <algorithm>
#include <mutex>
#include "ModuleIndex.h"
#include <chrono>
#include <unordered_map>
#include "Golay24128.h"
#include "YSFDefines.h"	// DMR_A/B/C_TABLE (72-bit AMBE2+ partitions) and PRNG_TABLE
#include "AmbeFrameCheck.h"	// DSTAR_W/X: the D-Star frame's bits in the partitions
#include "LevelTable.h"	// NearestLevel

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
	bool uncorrectable;	// four or more errors in the A partition: its data is not known
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
	f.uncorrectable = AmbeFrameCheck::Uncorrectable24(a_bits, f.u0);
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

// Write a new b2 into the frame: A changes by the difference of its code words
// for the old and the new u0, B by the difference of their scrambling, so the
// bit errors the frame came with stay where they were and the receiver's
// decoder still counts them (and miscorrects a B with four or more errors as it
// would have without the change)
static inline void AmbeWriteB2(uint8_t *ambe, const SAmbeFields &f, int b2)
{
	if (b2 < 0) b2 = 0;
	if (b2 > 31) b2 = 31;
	const unsigned int u0 = (f.u0 & 0xFF0) | ((unsigned int)b2 >> 1);
	const unsigned int da = CGolay24128::encode24128(f.u0) ^ CGolay24128::encode24128(u0);
	const unsigned int db = (PRNG_TABLE[f.u0] ^ PRNG_TABLE[u0]) >> 1;
	for (int i = 0; i < 24; i++)
		if ((da >> (23 - i)) & 1)
			ambe[DMR_A_TABLE[i] >> 3] ^= BIT_MASK_TABLE[DMR_A_TABLE[i] & 7];
	for (int i = 0; i < 23; i++)
		if ((db >> (22 - i)) & 1)
			ambe[DMR_B_TABLE[i] >> 3] ^= BIT_MASK_TABLE[DMR_B_TABLE[i] & 7];
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
	void Process(uint8_t *ambe, uint32_t streamKey, float gainDb, float maxStepDb = 0.0f)
	{
		// the streams of a module, under its lock: the modules' workers run in parallel
		auto &shard = m_shards[ModuleSlotOfKey(streamKey)];
		std::lock_guard<std::mutex> lock(shard.mutex);
		SState &st = State(shard, streamKey);

		// a frame whose A partition the FEC cannot correct stays as it is, so
		// the decoder still takes it for a bad one
		SAmbeFields f = AmbeReadFields(ambe);
		if (f.uncorrectable || !AmbeIsVoiceOrSilence(f.b0))
			return;
		// the silence frame (AMBE72_SILENCE: gateways know it by its bytes) stays
		// as it is: both gammas follow it
		if (0 == memcmp(ambe, AMBE72_SILENCE, sizeof(AMBE72_SILENCE)))
		{
			Unchanged(st, f.b2);
			return;
		}

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
		const int best = NearestLevel(AMBE_DG, 32, f.b2, want - base - AMBE_DG[f.b2]);
		st.gammaMod = AMBE_DG[best] + base;
		st.offset = st.gammaMod - st.gammaOrig;

		if (best != f.b2)
			AmbeWriteB2(ambe, f, best);
	}

	// a frame that goes out unchanged (DmrGainMode = off): both gammas follow
	// it, so that switching the mode on in a stream starts from what the
	// receiver's decoder has
	void Follow(const uint8_t *frame, uint32_t streamKey)
	{
		auto &shard = m_shards[ModuleSlotOfKey(streamKey)];
		std::lock_guard<std::mutex> lock(shard.mutex);
		SState &st = State(shard, streamKey);
		const SAmbeFields f = AmbeReadFields(frame);
		if (f.uncorrectable || !AmbeIsVoiceOrSilence(f.b0))
			return;
		Unchanged(st, f.b2);
	}

private:
	struct SState
	{
		float gammaOrig = 0.0f;
		float gammaMod = 0.0f;
		float offset = 0.0f;   // gammaMod - gammaOrig of the last frame
		std::chrono::steady_clock::time_point lastUsed{};
	};
	struct SShard;
	// a frame that goes out as it came, with this b2: both gammas follow it
	static void Unchanged(SState &st, int b2)
	{
		st.gammaOrig = AMBE_DG[b2] + 0.5f * st.gammaOrig;
		st.gammaMod = AMBE_DG[b2] + 0.5f * st.gammaMod;
		st.offset = st.gammaMod - st.gammaOrig;
	}
	// the stream's state, under the shard's lock; a new one drops the ended streams
	SState &State(SShard &shard, uint32_t streamKey)
	{
		const auto now = std::chrono::steady_clock::now();
		auto it = shard.streams.find(streamKey);
		if (it == shard.streams.end())
		{
			DropEndedStreams(shard.streams, now);
			it = shard.streams.emplace(streamKey, SState{}).first;
		}
		it->second.lastUsed = now;
		return it->second;
	}

public:
	// the streams not heard for 30 s, of every module (once a second, the reflector
	// thread; a module's are otherwise dropped only when it gets a new stream)
	void Purge(void)
	{
		const auto now = std::chrono::steady_clock::now();
		for (auto &shard : m_shards)
		{
			std::lock_guard<std::mutex> lock(shard.mutex);
			DropEndedStreams(shard.streams, now);
		}
	}
private:
	// streams not heard for 30 s have ended
	static void DropEndedStreams(std::unordered_map<uint32_t, SState> &streams, std::chrono::steady_clock::time_point now)
	{
		for (auto it = streams.begin(); it != streams.end(); )
		{
			if (now - it->second.lastUsed > std::chrono::seconds(30))
				it = streams.erase(it);
			else
				++it;
		}
	}

	// the streams by CTranscoderPacket::GetStreamKey, per module (ModuleSlotOfKey)
	struct SShard { std::mutex mutex; std::unordered_map<uint32_t, SState> streams; };
	SShard m_shards[kModuleSlots];
};

// D-Star (AMBE 3600x2400): the same idea on the 72-bit D-Star frame. b2 is a
// 6-bit field there, at parameter bits [6,7,8,9,42,43]: the upper four in the
// Golay(24,12) protected first partition (its data bits 17..14), the lower two
// in the unprotected fourth (its bits 6 and 5). The decoder takes
//     gamma = 2 * AmbePlusDg[b2] - 0.09
// with no prediction from the frame before (mbelib-neo ambe3600x2400.c,
// ambe2400_decode_gain), and one unit of gamma is 6.02 dB as in AMBE2+. The
// table's steps are 0.06-0.15, 0.7-1.8 dB: each frame gets the b2 whose gamma
// comes closest to its own plus the wanted gain, no state per stream needed.
//
// The third partition's bit 10 is spare in D-Star; the encoders (DVSI's, and
// mbelib-neo's after it) put the even parity of the second partition's code
// word there, scrambled with the 24th bit of the PRNG of the first partition's
// data. A new b2 changes that data, so the bit is written again where it held
// that parity before. As for DMR, the frame keeps the bit errors it came with.
//
// References:
//   - mbelib-neo src/ambe/ambe3600x2400.c (b0/b2, gain), ambe_common.c (the
//     partitions, the PRNG scrambling), ambe_encoder.c (ambe_enc_c1: the spare
//     bit), internal/ambe3600x2400_const.h (AmbePlusDg)

// AmbePlusDg (mbelib-neo); its last entry repeats the one before, DVSI's
// decoder plays b2 63 about 0.2 louder
static const float DSTAR_DG[64] = {
	0.000000f, 0.118200f, 0.215088f, 0.421167f, 0.590088f, 0.749075f, 0.879395f, 0.996388f, 1.092285f, 1.171577f, 1.236572f,
	1.313450f, 1.376465f, 1.453342f, 1.516357f, 1.600346f, 1.669189f, 1.742847f, 1.803223f, 1.880234f, 1.943359f, 2.025067f,
	2.092041f, 2.178042f, 2.248535f, 2.331718f, 2.399902f, 2.492343f, 2.568115f, 2.658677f, 2.732910f, 2.816496f, 2.885010f,
	2.956386f, 3.014893f, 3.078890f, 3.131348f, 3.206615f, 3.268311f, 3.344785f, 3.407471f, 3.484885f, 3.548340f, 3.623339f,
	3.684814f, 3.764509f, 3.829834f, 3.915298f, 3.985352f, 4.072560f, 4.144043f, 4.231251f, 4.302734f, 4.399066f, 4.478027f,
	4.572883f, 4.650635f, 4.760785f, 4.851074f, 4.972361f, 5.071777f, 5.226203f, 5.352783f, 5.55f
};

namespace DStarAmbe
{
	// the air bit (bit 0 of byte 0 first) of bit x of partition w: DSTAR_W/X inverted
	struct SAirBits
	{
		uint8_t at[4][24];
		SAirBits()
		{
			for (int i = 0; i < 72; i++)
				at[AmbeFrameCheck::DSTAR_W[i]][AmbeFrameCheck::DSTAR_X[i]] = uint8_t(i);
		}
	};
	static inline const SAirBits &Air(void) { static const SAirBits air; return air; }
	static inline unsigned int Get(const uint8_t *f, int i) { return (f[i >> 3] >> (i & 7)) & 1U; }
	static inline void Put(uint8_t *f, int i, unsigned int b) { f[i >> 3] = uint8_t(b ? (f[i >> 3] | (1U << (i & 7))) : (f[i >> 3] & ~(1U << (i & 7)))); }

	struct SFields
	{
		unsigned int u0;	// first partition's data (12 bits, FEC corrected): parameter bits 0..11 in bits 11..0
		unsigned int u1;	// second partition's data (12 bits, unscrambled, FEC corrected)
		int b0;				// pitch / frame type (7 bits)
		int b2;				// gain index (6 bits)
		bool uncorrectable;	// four or more errors in the first partition: its data is not known
	};

	// the spare bit for u0 and u1: the even parity of u1's (23,12) code word,
	// scrambled with the 24th bit of the PRNG seeded with u0 (PRNG_TABLE's bit 0;
	// bits 23..1 scramble the second partition)
	static inline unsigned int Spare(unsigned int u0, unsigned int u1)
	{
		return (unsigned int)(AmbeFrameCheck::Popcount(CGolay24128::encode23127(u1) >> 1) & 1) ^ (PRNG_TABLE[u0] & 1U);
	}

	static inline SFields Read(const uint8_t *frame)
	{
		const auto &air = Air();
		unsigned int a, b;
		AmbeFrameCheck::DStarWords(frame, a, b);
		SFields f;
		f.u0 = CGolay24128::decode24128(a);
		f.uncorrectable = AmbeFrameCheck::Uncorrectable24(a, f.u0);
		f.u1 = CGolay24128::decode23127(b ^ (PRNG_TABLE[f.u0] >> 1));
		// b0: parameter bits 0..5 (u0 bits 11..6) and 48 (fourth partition bit 0)
		f.b0 = int(((f.u0 >> 6) & 0x3F) << 1) | int(Get(frame, air.at[3][0]));
		// b2: parameter bits 6..9 (u0 bits 5..2), 42 and 43 (fourth partition bits 6, 5)
		f.b2 = int(((f.u0 >> 2) & 0x0F) << 2) | int(Get(frame, air.at[3][6]) << 1) | int(Get(frame, air.at[3][5]));
		return f;
	}

	// a new b2 into the frame, as AmbeWriteB2: the first partition changes by
	// the difference of its code words, the second by that of its scrambling,
	// so the frame keeps its bit errors; the spare bit follows where it held
	// the parity
	static inline void WriteB2(uint8_t *frame, const SFields &f, int b2)
	{
		b2 = std::clamp(b2, 0, 63);
		const auto &air = Air();
		const unsigned int u0 = (f.u0 & ~0x3CU) | ((unsigned int)(b2 >> 2) << 2);
		const unsigned int da = CGolay24128::encode24128(f.u0) ^ CGolay24128::encode24128(u0);
		const unsigned int db = (PRNG_TABLE[f.u0] ^ PRNG_TABLE[u0]) >> 1;
		const unsigned int spare = Get(frame, air.at[2][10]);
		for (int x = 0; x < 24; x++)
			if ((da >> x) & 1U)
				frame[air.at[0][x] >> 3] ^= uint8_t(1U << (air.at[0][x] & 7));
		for (int x = 0; x < 23; x++)
			if ((db >> x) & 1U)
				frame[air.at[1][x] >> 3] ^= uint8_t(1U << (air.at[1][x] & 7));
		Put(frame, air.at[3][6], (b2 >> 1) & 1);
		Put(frame, air.at[3][5], b2 & 1);
		if (spare == Spare(f.u0, f.u1))
			Put(frame, air.at[2][10], Spare(u0, f.u1));
	}

	// voice and silence: b0 0..124 (D-Star's 120..123 are voice, unlike AMBE2+'s
	// erasures); 125 and 127 play as lost frames, 126 is a tone (mbelib-neo
	// ambe3600x2400.c, ambe2400_decode_b0)
	static inline bool IsVoiceOrSilence(int b0) { return b0 < 125; }

	// the D-Star silence frame (gateways, repeaters, urfd, tcd): left as it is,
	// they know it by its bytes
	static const uint8_t (&SILENCE)[9] = AmbeFrameCheck::DSTAR_SILENCE;

	// change the decoded level of a D-Star frame by gainDb; a frame whose first
	// partition the FEC cannot correct stays as it is, so the decoder still
	// takes it for a bad one
	static inline void Gain(uint8_t *frame, float gainDb)
	{
		if (0 == memcmp(frame, SILENCE, sizeof(SILENCE)))
			return;
		const SFields f = Read(frame);
		if (f.uncorrectable || !IsVoiceOrSilence(f.b0))
			return;
		// gamma = 2 * DSTAR_DG[b2] - 0.09: the offset is the same for every b2
		const int best = NearestLevel(DSTAR_DG, 64, f.b2, gainDb / 6.0206f / 2.0f);
		if (best != f.b2)
			WriteB2(frame, f, best);
	}
}

#undef AMBE_READ_BIT
#undef AMBE_WRITE_BIT
