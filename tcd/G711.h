#pragma once

// tcd - a hybid transcoder using DVSI hardware and Codec2 software
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// G.711 µ-law and A-law with 16-bit linear samples, for an AMBE device whose speech
// packets are companded (DeviceCompand): the values of PKT_COMPAND, 8 bits a sample
// on the serial line, 16 bits in tcd.

#include <cstdint>

#define COMPAND_LINEAR 0
#define COMPAND_ULAW   1	// PKT_COMPAND bit 0: on, bit 1: 0 µ-law
#define COMPAND_ALAW   3	// bit 1: 1 A-law

inline int16_t UlawToLinear(uint8_t u)
{
	u = ~u;
	int t = ((u & 0x0F) << 3) + 0x84;
	t <<= (u & 0x70) >> 4;
	return int16_t((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

inline uint8_t LinearToUlaw(int16_t sample)
{
	int pcm = sample;
	uint8_t mask;
	if (pcm < 0)
	{
		pcm = 0x84 - pcm;
		mask = 0x7F;
	}
	else
	{
		pcm += 0x84;
		mask = 0xFF;
	}
	if (pcm > 0x7FFF)
		pcm = 0x7FFF;
	int seg = 0;
	for (int v = pcm >> 8; v; v >>= 1)
		seg++;
	if (seg > 7)
		seg = 7;
	return uint8_t(((seg << 4) | ((pcm >> (seg + 3)) & 0x0F)) ^ mask);
}

// the AMBE chips' A-law has the sign bit the other way round from G.711's tables
// (a decoded test file was the linear one inverted)
inline int16_t AlawToLinear(uint8_t a)
{
	a ^= 0x55 ^ 0x80;
	int t = (a & 0x0F) << 4;
	const int seg = (a & 0x70) >> 4;
	if (0 == seg)
		t += 8;
	else
	{
		t += 0x108;
		t <<= seg - 1;
	}
	return int16_t((a & 0x80) ? t : -t);
}

inline uint8_t LinearToAlaw(int16_t sample)
{
	int pcm = sample >> 3;
	uint8_t mask;
	if (pcm >= 0)
		mask = 0xD5;
	else
	{
		mask = 0x55;
		pcm = -pcm - 1;
	}
	int seg = 0;
	while (seg < 8 && pcm > (0x20 << seg) - 1)
		seg++;
	uint8_t a;
	if (seg >= 8)
		a = 0x7F;
	else
		a = uint8_t((seg << 4) | ((seg < 2 ? pcm >> 1 : pcm >> seg) & 0x0F));
	return uint8_t((a ^ mask) ^ 0x80);
}

inline const char *CompandName(int compand)
{
	return (COMPAND_ULAW == compand) ? "ulaw" : (COMPAND_ALAW == compand) ? "alaw" : "linear";
}

inline int16_t CompandToLinear(int compand, uint8_t b)
{
	return (COMPAND_ALAW == compand) ? AlawToLinear(b) : UlawToLinear(b);
}

inline uint8_t LinearToCompand(int compand, int16_t s)
{
	return (COMPAND_ALAW == compand) ? LinearToAlaw(s) : LinearToUlaw(s);
}
