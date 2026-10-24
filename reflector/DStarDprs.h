#pragma once

// DStarDprs -- reads D-Star positions (DPRS) from the slow data of a stream
//
// D-Star radios with GPS send their position in the slow data, as GPS-A
// ("$$CRCxxxx," and an APRS packet) or as NMEA sentences ($GPRMC, $GPGGA).
// One decoder per stream: feed it every frame, it reports each new position.
//
// Copyright (C) 2026 Jens-Christian Merg (DL4JC)
// Licensed under the GNU General Public License v3 or later

#include <cstdint>
#include <string>

class CDStarDprs
{
public:
	void Reset(void);
	// one frame's 3 slow data bytes and its D-Star frame number (0-20);
	// true when the frame completed a position that differs from the last one
	bool Add(uint8_t frame, const uint8_t *dvdata, double &lat, double &lon);

private:
	bool ParseLine(const std::string &line, double &lat, double &lon) const;

	uint8_t     m_Block[6] = {};
	uint8_t     m_Frame = 0xFF;	// frame of the first half in m_Block, 0xFF = none
	std::string m_Line;
	bool        m_HavePos = false;
	double      m_Lat = 0.0, m_Lon = 0.0;
};
