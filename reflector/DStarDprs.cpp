// DStarDprs -- reads D-Star positions (DPRS) from the slow data of a stream
//
// Copyright (C) 2026 Jens-Christian Merg (DL4JC)
// Licensed under the GNU General Public License v3 or later

#include <cctype>
#include <cstdlib>
#include <vector>

#include "DStarDprs.h"

// slow data is scrambled with this, frame 0 of each superframe carries the sync instead
static const uint8_t SCRAMBLER[3] = { 0x70U, 0x4FU, 0x93U };
static const uint8_t SLOWDATA_GPS = 0x30U;
static const size_t  MAX_LINE = 256U;

// CRC-16/X.25 as the Icom radios and ircDDBGateway compute it for GPS-A
static uint16_t GpsaCrc(const std::string &s)
{
	uint16_t crc = 0xFFFFU;
	for (const unsigned char c : s)
	{
		uint8_t ch = c;
		for (int i = 0; i < 8; i++)
		{
			const bool flag = (crc ^ ch) & 0x01U;
			crc >>= 1;
			if (flag)
				crc ^= 0x8408U;
			ch >>= 1;
		}
	}
	return ~crc & 0xFFFFU;
}

// "ddmm.mm" (APRS, spaces for ambiguity) or "ddmm.mmmm" (NMEA) to degrees;
// degdigits is 2 for latitude, 3 for longitude
static bool Coordinate(std::string s, size_t degdigits, char hemi, double &value)
{
	for (auto &c : s)
		if (' ' == c)
			c = '0';
	if (s.size() < degdigits + 4 || '.' != s[degdigits + 2])
		return false;
	for (size_t i = 0; i < s.size(); i++)
		if (i != degdigits + 2 && !isdigit((unsigned char)s[i]))
			return false;
	const double deg = std::atof(s.substr(0, degdigits).c_str());
	const double min = std::atof(s.substr(degdigits).c_str());
	if (min >= 60.0)
		return false;
	value = deg + min / 60.0;
	switch (hemi)
	{
		case 'N': case 'E': break;
		case 'S': case 'W': value = -value; break;
		default: return false;
	}
	return (2 == degdigits) ? (value >= -90.0 && value <= 90.0) : (value >= -180.0 && value <= 180.0);
}

static std::vector<std::string> Fields(const std::string &s)
{
	std::vector<std::string> f;
	size_t start = 0;
	while (true)
	{
		const auto comma = s.find(',', start);
		f.push_back(s.substr(start, comma - start));
		if (std::string::npos == comma)
			return f;
		start = comma + 1;
	}
}

void CDStarDprs::Reset(void)
{
	m_Frame = 0xFF;
	m_Line.clear();
	m_HavePos = false;
}

bool CDStarDprs::Add(uint8_t frame, const uint8_t *dvdata, double &lat, double &lon)
{
	// slow data comes in 6 byte blocks over frames 1+2, 3+4 ... 19+20;
	// the protocols pass the frame byte on, with 0x40 set on the last one
	frame = (0x1FU & frame) % 21U;
	if (0U == frame)
	{
		m_Frame = 0xFF;
		return false;
	}
	if (1U == (frame & 1U))
	{
		for (int i = 0; i < 3; i++)
			m_Block[i] = dvdata[i] ^ SCRAMBLER[i];
		m_Frame = frame;
		return false;
	}
	if (m_Frame != frame - 1U)
		return false;	// first half lost
	m_Frame = 0xFF;
	for (int i = 0; i < 3; i++)
		m_Block[i + 3] = dvdata[i] ^ SCRAMBLER[i];

	if (SLOWDATA_GPS != (0xF0U & m_Block[0]))
		return false;
	const unsigned count = 0x0FU & m_Block[0];
	bool found = false;
	for (unsigned i = 1; i <= count && i < 6; i++)
	{
		const char c = char(m_Block[i]);
		if ('\r' == c || '\n' == c)
		{
			double la, lo;
			if (!m_Line.empty() && ParseLine(m_Line, la, lo) && (!m_HavePos || la != m_Lat || lo != m_Lon))
			{
				m_HavePos = true;
				m_Lat = lat = la;
				m_Lon = lon = lo;
				found = true;
			}
			m_Line.clear();
		}
		else if (m_Line.size() < MAX_LINE)
			m_Line += c;
		else
			m_Line.clear();	// no line end in sight, garbage
	}
	return found;
}

bool CDStarDprs::ParseLine(const std::string &line, double &lat, double &lon) const
{
	// GPS-A: "$$CRCxxxx," and an APRS packet "CALL>DEST,PATH:info"
	if (0 == line.compare(0, 5, "$$CRC"))
	{
		if (line.size() < 11 || ',' != line[9])
			return false;
		const auto crc = uint16_t(std::strtoul(line.substr(5, 4).c_str(), nullptr, 16));
		const std::string packet = line.substr(10);
		// radios differ in whether the CR counts
		if (crc != GpsaCrc(packet) && crc != GpsaCrc(packet + "\r"))
			return false;
		const auto colon = packet.find(':');
		if (std::string::npos == colon || colon + 1 >= packet.size())
			return false;
		const std::string info = packet.substr(colon + 1);
		size_t pos;
		switch (info[0])
		{
			case '!': case '=': pos = 1; break;	// no timestamp
			case '/': case '@': pos = 8; break;	// 7 character timestamp
			default: return false;	// compressed or no position, not sent by D-Star radios
		}
		// "ddmm.mmN" symbol table "dddmm.mmE" symbol
		if (info.size() < pos + 19)
			return false;
		return Coordinate(info.substr(pos, 7), 2, info[pos + 7], lat)
		    && Coordinate(info.substr(pos + 9, 8), 3, info[pos + 17], lon);
	}

	// NMEA: "$GPRMC,..." or "$GPGGA,...", any talker (GP, GN, GL, ...), with checksum
	if (line.size() < 7 || '$' != line[0])
		return false;
	const auto star = line.find('*');
	if (std::string::npos == star || star + 3 > line.size())
		return false;
	uint8_t sum = 0;
	for (size_t i = 1; i < star; i++)
		sum ^= uint8_t(line[i]);
	if (sum != uint8_t(std::strtoul(line.substr(star + 1, 2).c_str(), nullptr, 16)))
		return false;
	const auto f = Fields(line.substr(0, star));
	if (6 != f[0].size())
		return false;
	const std::string type = f[0].substr(3);
	if ("RMC" == type && f.size() > 6 && "A" == f[2])
		return !f[4].empty() && !f[6].empty() && Coordinate(f[3], 2, f[4][0], lat) && Coordinate(f[5], 3, f[6][0], lon);
	if ("GGA" == type && f.size() > 6 && !f[6].empty() && "0" != f[6])
		return !f[3].empty() && !f[5].empty() && Coordinate(f[2], 2, f[3][0], lat) && Coordinate(f[4], 3, f[5][0], lon);
	return false;
}
