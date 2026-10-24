// HyTalkGtrp -- GTRP, the GIS channel of HyTalk radios
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <cstdlib>
#include "HyTalkGtrp.h"

static std::vector<uint8_t> Header(uint8_t type, uint32_t tid)
{
	return { GTRP_MAGIC, type, uint8_t(tid >> 24), uint8_t(tid >> 16), uint8_t(tid >> 8), uint8_t(tid), 0, 0 };
}

// patch the body length into bytes 6 and 7
static std::vector<uint8_t> &Finish(std::vector<uint8_t> &m)
{
	const size_t body = m.size() - GTRP_HEADER_SIZE;
	m[6] = uint8_t(body >> 8);
	m[7] = uint8_t(body);
	return m;
}

long CGtrp::MessageSize(const uint8_t *data, size_t len)
{
	if (len < 1)
		return 0;
	if (data[0] != GTRP_MAGIC)
		return -1;
	if (len < GTRP_HEADER_SIZE)
		return 0;
	const size_t total = GTRP_HEADER_SIZE + ((size_t(data[6]) << 8) | data[7]);
	return (len < total) ? 0 : long(total);
}

std::string CGtrp::DecodeBcd(const uint8_t *bcd, size_t n)
{
	std::string s;
	for (size_t i = 0; i < n; i++)
	{
		for (int half = 0; half < 2; half++)
		{
			const uint8_t d = half ? (bcd[i] >> 4) : (bcd[i] & 0x0f);
			if (d > 9)
				return s;
			s += char('0' + d);
		}
	}
	return s;
}

std::vector<uint8_t> CGtrp::EncodeBcd(const std::string &digits)
{
	std::vector<uint8_t> v((digits.size() + 1) / 2, 0xff);
	for (size_t i = 0; i < digits.size(); i++)
	{
		const uint8_t d = uint8_t(digits[i] - '0') & 0x0f;
		if (i & 1)
			v[i / 2] = uint8_t((v[i / 2] & 0x0f) | (d << 4));
		else
			v[i / 2] = uint8_t(0xf0 | d);
	}
	return v;
}

// "DDMM.MMMM" digits without the point: degrees from the first degdigits digits
static bool NmeaDegrees(const std::string &d, size_t degdigits, double &value)
{
	if (d.size() < degdigits + 2)
		return false;
	const double deg = atof(d.substr(0, degdigits).c_str());
	const std::string min = d.substr(degdigits, 2) + "." + d.substr(degdigits + 2);
	const double minutes = atof(min.c_str());
	if (minutes >= 60.0)
		return false;
	value = deg + minutes / 60.0;
	return true;
}

// GpsBaseData: valid flag (0 = fix), time, longitude E/W + 5 BCD bytes (DDDMM.MMMM),
// latitude N/S + 4 BCD bytes (DDMM.MMMM), altitude 3 BCD bytes (AAAAA.A)
bool CGtrp::DecodePosition(const uint8_t *v, size_t n, SGtrpPosition &pos)
{
	if (n < 19)
		return false;
	pos.valid = (v[0] == 0);
	pos.time = (uint32_t(v[1]) << 24) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 8) | v[4];
	const char lonDir = char(v[5]), latDir = char(v[11]);
	double lon, lat;
	if (!NmeaDegrees(DecodeBcd(v + 6, 5), 3, lon) || !NmeaDegrees(DecodeBcd(v + 12, 4), 2, lat))
		return false;
	if ((lonDir != 'E' && lonDir != 'W') || (latDir != 'N' && latDir != 'S'))
		return false;
	pos.lon = (lonDir == 'W') ? -lon : lon;
	pos.lat = (latDir == 'S') ? -lat : lat;
	const std::string alt = DecodeBcd(v + 16, 3);
	if (alt.size() >= 2)
		pos.alt = atof((alt.substr(0, alt.size() - 1) + "." + alt.substr(alt.size() - 1)).c_str());
	return true;
}

// GpsExtData: heading DDD.D and speed SSS.S (2 BCD bytes each), power percent,
// rssi in sign-magnitude
void CGtrp::DecodeExt(const uint8_t *v, size_t n, SGtrpPosition &pos)
{
	if (n < 5)
		return;
	auto tenths = [](const std::string &d) { return d.empty() ? 0.0 : atof(d.c_str()) / 10.0; };
	pos.heading = tenths(DecodeBcd(v, 2));
	pos.speed = tenths(DecodeBcd(v + 2, 2));
	pos.power = v[4];
	if (n >= 6)
		pos.rssi = (v[5] & 0x80) ? -int(v[5] & 0x7f) : int(v[5] & 0x7f);
	pos.hasExt = true;
}

bool CGtrp::Decode(const uint8_t *data, size_t len, SGtrpMsg &msg)
{
	if (MessageSize(data, len) != long(len))
		return false;
	msg = SGtrpMsg();
	msg.type = data[1];
	msg.tid = (uint32_t(data[2]) << 24) | (uint32_t(data[3]) << 16) | (uint32_t(data[4]) << 8) | data[5];
	size_t i = GTRP_HEADER_SIZE;
	while (i < len)
	{
		const uint8_t tag = data[i];
		if (tag == GTRP_IE_RESULT)
		{
			if (i + 1 >= len)
				return false;
			msg.result = data[i + 1];
			i += 2;
			continue;
		}
		if (i + 1 >= len || i + 2 + data[i + 1] > len)
			return false;
		const uint8_t *v = data + i + 2;
		const size_t n = data[i + 1];
		switch (tag)
		{
		case GTRP_IE_OWN_NUMBER:
			msg.own = DecodeBcd(v, n);
			break;
		case GTRP_IE_PEER_NUMBER:
			msg.peer = DecodeBcd(v, n);
			break;
		case GTRP_IE_GPS_BASE:
			msg.hasPosition = DecodePosition(v, n, msg.pos);
			break;
		case GTRP_IE_GPS_EXT:
			DecodeExt(v, n, msg.pos);
			break;
		default:
			break;  // unknown elements are skipped by their length
		}
		i += 2 + n;
	}
	return true;
}

std::vector<uint8_t> CGtrp::BaseResponse(uint8_t type, uint32_t tid, uint8_t result)
{
	auto m = Header(type, tid);
	m.push_back(GTRP_IE_RESULT);
	m.push_back(result);
	return Finish(m);
}

// GisConfigParam: three u16 big endian, taken as {report interval in seconds,
// GIS switch, real-time report}; the order of the last two is not confirmed
// on the wire yet. The config flag element switches the reporting on or off.
std::vector<uint8_t> CGtrp::GpsConfig(uint32_t tid, uint16_t interval, bool on)
{
	auto m = Header(GTRP_GPS_CONFIG_REQUEST, tid);
	const uint8_t sw = on ? 1 : 0;
	const uint8_t cfg[] = { GTRP_IE_GIS_CONFIG, 6, uint8_t(interval >> 8), uint8_t(interval), 0, sw, 0, 0 };
	m.insert(m.end(), cfg, cfg + sizeof(cfg));
	const uint8_t flag[] = { GTRP_IE_CONFIG_FLAG, 1, sw };
	m.insert(m.end(), flag, flag + sizeof(flag));
	return Finish(m);
}
