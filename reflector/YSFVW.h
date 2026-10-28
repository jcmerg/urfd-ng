//  YSF VW (voice FR mode): IMBE 7200x4400 frames in YSF, with the FEC of P25 phase 1
//
//  A VW voice frame carries 5 IMBE frames of 18 bytes (144 bits) at 0, 18, 36, 54, 72 of its
//  90-byte payload; the first one after the header (FN 0, FT 1) carries data in its first 54
//  bytes (the 20-byte radio id, as a data FR channel) and 2 IMBE frames (54, 72). An IMBE frame:
//  4 Golay(23,12), 3 Hamming(15,11), 7 bits, whitened from c0 and interleaved (as MMDVMHost's
//  AMBEFEC). The 88 parameter bits u0..u7 go to and from the transcoder as 11 bytes, MSB first,
//  as P25's.
//
//  urfd -- The universal reflector
//  Copyright © 2026
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.

#pragma once

#include <cstdint>

// IMBE silence in the 11-byte form (MMDVMHost and P25Gateway fill a lost frame with it)
extern const uint8_t IMBE11_SILENCE[11];

class CYsfVW
{
public:
	// 18 bytes of a VW payload -> 11 bytes u0..u7, the FEC corrected; the number of corrected bits
	static unsigned DecodeImbe(const uint8_t *block18, uint8_t *imbe11);
	// 11 bytes u0..u7 -> 18 bytes of a VW payload
	static void EncodeImbe(const uint8_t *imbe11, uint8_t *block18);
	// the payload offsets of the IMBE frames of a voice frame (2 or 5)
	static unsigned Offsets(uint8_t fn, uint8_t ft, const unsigned **offsets);
	// the first voice frame's data part: 20 bytes <-> the first 45 bytes of the payload
	static void EncodeData(const uint8_t *dt20, uint8_t *payload);
	static bool DecodeData(const uint8_t *payload, uint8_t *dt20);
};
