//  Copyright (C) 2016,2017,2018 by Jonathan Naylor G4KLX (YSFGateway GPS.cpp, the position decoding)
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

#include "YSFGps.h"

static const uint8_t SHRT_GPS[] = { 0x22U, 0x62U };
static const uint8_t LONG_GPS[] = { 0x47U, 0x64U };

bool CYsfGps::AddVD2(uint8_t fn, uint8_t ft, const uint8_t *dt10, double &lat, double &lon)
{
	if ( m_bDone || (fn != 6U && fn != 7U) )
		return false;
	memcpy(m_Buffer + (fn - 6U) * 10U, dt10, 10U);
	if ( fn != ft )
		return false;
	m_bDone = Decode(m_Buffer, (fn - 5U) * 10U, lat, lon);
	return m_bDone;
}

bool CYsfGps::AddVD1(uint8_t fn, uint8_t ft, const uint8_t *dt20, double &lat, double &lon)
{
	if ( m_bDone || fn < 3U )
		return false;
	memcpy(m_Buffer + (fn - 3U) * 20U, dt20, 20U);
	if ( fn != ft )
		return false;
	m_bDone = Decode(m_Buffer, (fn - 2U) * 20U, lat, lon);
	return m_bDone;
}

bool CYsfGps::Decode(const uint8_t *b, unsigned len, double &lat, double &lon)
{
	// the end marker, the last 03 followed by the sum of everything up to it (YSFGateway starts one
	// byte further, past the data)
	bool valid = false;
	for ( unsigned i = len - 2U; i > 0U; i-- )
	{
		if ( b[i] == 0x03U )
		{
			uint8_t sum = 0U;
			for ( unsigned j = 0U; j <= i; j++ )
				sum += b[j];
			valid = (sum == b[i + 1U]);
			break;
		}
	}
	if ( !valid || len < 14U || (memcmp(b + 1U, SHRT_GPS, 2U) != 0 && memcmp(b + 1U, LONG_GPS, 2U) != 0) )
		return false;

	for ( unsigned i = 5U; i < 11U; i++ )
	{
		const uint8_t h = b[i] & 0xF0U;
		if ( h != 0x50U && h != 0x30U )
			return false;
	}

	unsigned tens = b[5U] & 0x0FU, units = b[6U] & 0x0FU;
	const unsigned lat_deg = tens * 10U + units;
	if ( tens > 9U || units > 9U || lat_deg > 89U )
		return false;
	tens = b[7U] & 0x0FU; units = b[8U] & 0x0FU;
	const unsigned lat_min = tens * 10U + units;
	if ( tens > 9U || units > 9U || lat_min > 59U )
		return false;
	tens = b[9U] & 0x0FU; units = b[10U] & 0x0FU;
	const unsigned lat_min_frac = tens * 10U + units;
	if ( tens > 9U || units > 10U || lat_min_frac > 99U )	// units 10: Yaesu firmware, as YSFGateway
		return false;

	int lat_dir;
	uint8_t h = b[8U] & 0xF0U;
	if ( h == 0x50U )
		lat_dir = 1;	// N
	else if ( h == 0x30U )
		lat_dir = -1;	// S
	else
		return false;

	unsigned lon_deg;
	uint8_t x = b[11U];
	h = b[9U] & 0xF0U;
	if ( h == 0x50U )
	{
		if ( x >= 0x76U && x <= 0x7FU )
			lon_deg = x - 0x76U;	// 0 to 9
		else if ( x >= 0x6CU && x <= 0x75U )
			lon_deg = 100U + (x - 0x6CU);	// 100 to 109
		else if ( x >= 0x26U && x <= 0x6BU )
			lon_deg = 110U + (x - 0x26U);	// 110 to 179
		else
			return false;
	}
	else if ( h == 0x30U )
	{
		if ( x >= 0x26U && x <= 0x7FU )
			lon_deg = 10U + (x - 0x26U);	// 10 to 99
		else
			return false;
	}
	else
		return false;

	unsigned lon_min;
	x = b[12U];
	if ( x >= 0x58U && x <= 0x61U )
		lon_min = x - 0x58U;	// 0 to 9
	else if ( x >= 0x26U && x <= 0x57U )
		lon_min = 10U + (x - 0x26U);	// 10 to 59
	else
		return false;

	x = b[13U];
	if ( x < 0x1CU || x > 0x7FU )
		return false;
	const unsigned lon_min_frac = x - 0x1CU;

	int lon_dir;
	h = b[10U] & 0xF0U;
	if ( h == 0x30U )
		lon_dir = 1;	// E
	else if ( h == 0x50U )
		lon_dir = -1;	// W
	else
		return false;

	lat = lat_dir * (lat_deg + (lat_min + lat_min_frac * 0.01) / 60.0);
	lon = lon_dir * (lon_deg + (lon_min + lon_min_frac * 0.01) / 60.0);
	return true;
}
