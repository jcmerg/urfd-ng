/*
 *	Copyright (C) 2009,2014,2015 by Jonathan Naylor, G4KLX
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

#ifndef	Utils_H
#define	Utils_H

#include <string>
#include <cstdint>

class CUtils
{
public:
	static void byteToBitsBE(unsigned char byte, bool* bits);
	static void byteToBitsLE(unsigned char byte, bool* bits);

	static void bitsToByteBE(const bool* bits, unsigned char& byte);
	static void bitsToByteLE(const bool* bits, unsigned char& byte);

	// text for an XML element: &, < and > as entities
	static std::string XmlEscape(const std::string &in);

	// PCM gain as a linear numerator, 256 = 0 dB, clipped to int16; dB clamped to -40..+40
	static void ApplyGain(int16_t *pcm, int count, int32_t gainNum);
	static int32_t GainFromDb(int db);

private:
};

#endif
