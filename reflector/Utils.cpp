/*
 *	Copyright (C) 2009,2014,2015,2016 Jonathan Naylor, G4KLX
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; version 2 of the License.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 */

#include "Utils.h"

#include <cstdio>
#include <cassert>
#include <cmath>

void CUtils::byteToBitsBE(unsigned char byte, bool* bits)
{
	assert(bits != nullptr);

	bits[0U] = (byte & 0x80U) == 0x80U;
	bits[1U] = (byte & 0x40U) == 0x40U;
	bits[2U] = (byte & 0x20U) == 0x20U;
	bits[3U] = (byte & 0x10U) == 0x10U;
	bits[4U] = (byte & 0x08U) == 0x08U;
	bits[5U] = (byte & 0x04U) == 0x04U;
	bits[6U] = (byte & 0x02U) == 0x02U;
	bits[7U] = (byte & 0x01U) == 0x01U;
}

void CUtils::byteToBitsLE(unsigned char byte, bool* bits)
{
	assert(bits != nullptr);

	bits[0U] = (byte & 0x01U) == 0x01U;
	bits[1U] = (byte & 0x02U) == 0x02U;
	bits[2U] = (byte & 0x04U) == 0x04U;
	bits[3U] = (byte & 0x08U) == 0x08U;
	bits[4U] = (byte & 0x10U) == 0x10U;
	bits[5U] = (byte & 0x20U) == 0x20U;
	bits[6U] = (byte & 0x40U) == 0x40U;
	bits[7U] = (byte & 0x80U) == 0x80U;
}

void CUtils::bitsToByteBE(const bool* bits, unsigned char& byte)
{
	assert(bits != nullptr);

	byte  = bits[0U] ? 0x80U : 0x00U;
	byte |= bits[1U] ? 0x40U : 0x00U;
	byte |= bits[2U] ? 0x20U : 0x00U;
	byte |= bits[3U] ? 0x10U : 0x00U;
	byte |= bits[4U] ? 0x08U : 0x00U;
	byte |= bits[5U] ? 0x04U : 0x00U;
	byte |= bits[6U] ? 0x02U : 0x00U;
	byte |= bits[7U] ? 0x01U : 0x00U;
}

void CUtils::bitsToByteLE(const bool* bits, unsigned char& byte)
{
	assert(bits != nullptr);

	byte  = bits[0U] ? 0x01U : 0x00U;
	byte |= bits[1U] ? 0x02U : 0x00U;
	byte |= bits[2U] ? 0x04U : 0x00U;
	byte |= bits[3U] ? 0x08U : 0x00U;
	byte |= bits[4U] ? 0x10U : 0x00U;
	byte |= bits[5U] ? 0x20U : 0x00U;
	byte |= bits[6U] ? 0x40U : 0x00U;
	byte |= bits[7U] ? 0x80U : 0x00U;
}

std::string CUtils::XmlEscape(const std::string &in)
{
	std::string out;
	for (const char c : in)
	{
		switch (c)
		{
			case '&': out += "&amp;"; break;
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			default:  out += c; break;
		}
	}
	return out;
}

void CUtils::ApplyGain(int16_t *pcm, int count, int32_t gainNum)
{
	if (gainNum == 256)
		return;
	for (int i = 0; i < count; i++)
	{
		int32_t v = ((int32_t)pcm[i] * gainNum) >> 8;
		pcm[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
	}
}

int32_t CUtils::GainFromDb(int db)
{
	if (db < -40) db = -40; else if (db > 40) db = 40;
	return (int32_t)roundf(256.0f * powf(10.0f, (float)db / 20.0f));
}
