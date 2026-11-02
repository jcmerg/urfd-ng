#pragma once
// FEC check of the 72-bit AMBE frames the AMBE devices take and give: the
// Golay(24,12) of the first partition, parity bit included, and the Golay(23,12)
// of the second after its descrambling, as a decoder checks a frame before it
// plays it (TIA-102.BABA; the scheme mbelib implements). errors counts the bits
// the two codes corrected; uncorrectable is set for four or more errors in the
// first partition, which a decoder answers with a repeat of the last frame, and
// for bytes that are no AMBE frame at all. The frame bits are only read.
//
// Bit tables: DMR (AMBE2+, 72 bits in air order) from MMDVMHost's YSFDefines.h,
// D-Star (the 9 voice bytes of a DV frame, bit 0 of byte 0 first) from the
// D-Star specification.
#include <cstdint>
#include "Golay24128.h"
#include "YSFDefines.h"	// DMR_A_TABLE, DMR_B_TABLE, PRNG_TABLE, READ_BIT

struct SAmbeFec
{
	int errors;				// bits the FEC corrected, parity bit included
	bool uncorrectable;		// the first partition had four or more errors
};

namespace AmbeFrameCheck
{
	// air bit i of a D-Star DV frame carries bit dX[i] of partition dW[i]
	// (0: the Golay(24,12) word, bit 0 its overall parity, bits 1..23 the
	// (23,12) code word with the data in bits 12..23; 1: the scrambled (23,12)
	// word, data in bits 11..22; 2 and 3: unprotected). The bit index is the
	// bit's position in the word.
	static const uint8_t DSTAR_W[72] = {
		0, 0, 3, 2, 1, 1, 0, 0, 1, 1, 0, 0, 3, 2, 1, 1, 3, 2, 1, 1, 0, 0, 3, 2, 0, 0, 3, 2, 1, 1, 0, 0, 1, 1, 0, 0,
		3, 2, 1, 1, 3, 2, 1, 1, 0, 0, 3, 2, 0, 0, 3, 2, 1, 1, 0, 0, 1, 1, 0, 0, 3, 2, 1, 1, 3, 3, 2, 1, 0, 0, 3, 3,
	};
	static const uint8_t DSTAR_X[72] = {
		10, 22, 11, 9, 10, 22, 11, 23, 8, 20, 9, 21, 10, 8, 9, 21, 8, 6,  7,  19, 8, 20, 9, 7,
		6,  18, 7,  5, 6,  18, 7,  19, 4, 16, 5, 17, 6,  4, 5, 17, 4, 2,  3,  15, 4, 16, 5, 3,
		2,  14, 3,  1, 2,  14, 3,  15, 0, 12, 1, 13, 2,  0, 1, 13, 0, 12, 10, 11, 0, 12, 1, 13,
	};

	static inline int Popcount(unsigned int v) { return __builtin_popcount(v); }

	// a: the 24 bits of the first partition, the parity bit last; b: the 23
	// bits of the second, still scrambled
	static inline SAmbeFec Check(unsigned int a, unsigned int b)
	{
		// CGolay24128's tables hold the 24-bit words: the data in bits 23..12,
		// the Golay parity in bits 11..1 and, for the (24,12) code, the overall
		// parity in bit 0; decode23127() takes the 23-bit word in bits 22..0
		SAmbeFec r{0, false};
		const unsigned int u0 = CGolay24128::decode24128(a);
		r.errors = Popcount((a ^ CGolay24128::encode24128(u0)) & 0xFFFFFFU);
		r.uncorrectable = r.errors > 3;
		const unsigned int bs = b ^ (PRNG_TABLE[u0] >> 1);
		const unsigned int u1 = CGolay24128::decode23127(bs);
		r.errors += Popcount((bs ^ (CGolay24128::encode23127(u1) >> 1)) & 0x7FFFFFU);
		return r;
	}

	static inline SAmbeFec Dmr(const uint8_t *frame)
	{
		unsigned int a = 0, b = 0;
		for (int i = 0; i < 24; i++)
			a = (a << 1) | (READ_BIT(frame, DMR_A_TABLE[i]) ? 1U : 0U);
		for (int i = 0; i < 23; i++)
			b = (b << 1) | (READ_BIT(frame, DMR_B_TABLE[i]) ? 1U : 0U);
		return Check(a, b);
	}

	static inline SAmbeFec DStar(const uint8_t *frame)
	{
		unsigned int a = 0, b = 0;
		for (int i = 0; i < 72; i++)
		{
			const unsigned int bit = (frame[i >> 3] >> (i & 7)) & 1U;
			if (DSTAR_W[i] == 0)
				a |= bit << DSTAR_X[i];
			else if (DSTAR_W[i] == 1)
				b |= bit << DSTAR_X[i];
		}
		return Check(a, b);
	}
}
