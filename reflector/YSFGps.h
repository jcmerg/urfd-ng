//  YSF positions: the GPS data a Yaesu radio sends in the data channel, as YSFGateway's GPS.cpp
//  reads it. DN (V/D mode 2) has it in DT1 and DT2 (FN 6 and 7, 10 bytes each), V/D mode 1 in DT1
//  to DT5 (FN 3 to 7, 20 bytes each). A short (22 62) or long (47 64) position, an end marker 03 and
//  a sum; latitude and longitude as Mic-E like digits.
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

class CYsfGps
{
public:
	// a new transmission
	void Reset(void) { m_bDone = false; }
	// the data of a DN frame (10 bytes) or of a V/D mode 1 frame (20 bytes); true with a position,
	// once per transmission
	bool AddVD2(uint8_t fn, uint8_t ft, const uint8_t *dt10, double &lat, double &lon);
	bool AddVD1(uint8_t fn, uint8_t ft, const uint8_t *dt20, double &lat, double &lon);
	// the position in buf (len bytes from the start of DT1), false if there is none
	static bool Decode(const uint8_t *buf, unsigned len, double &lat, double &lon);

private:
	uint8_t m_Buffer[100] = {};
	bool m_bDone = false;
};
