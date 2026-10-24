//
//  Copyright © 2020 Thomas A. Early, N7TAE
//
// ----------------------------------------------------------------------------
//
//    m17ref is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    m17ref is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    with this software.  If not, see <http://www.gnu.org/licenses/>.
// ----------------------------------------------------------------------------

#include <arpa/inet.h>

#include "M17Packet.h"

CM17Packet::CM17Packet(const uint8_t *buf)
{
	memcpy(m17.magic, buf, sizeof(SM17Frame));

	// @ALL is no callsign, the destination stays empty
	if (! IsBroadcast())
		destination.CodeIn(m17.lich.addr_dst);
	source.CodeIn(m17.lich.addr_src);
}

// the destination @ALL (all ones), which some clients send to a reflector
bool CM17Packet::IsBroadcast() const
{
	for (auto b : m17.lich.addr_dst)
		if (0xFFU != b)
			return false;
	return true;
}

const CCallsign &CM17Packet::GetDestCallsign() const
{
	return destination;
}

const CCallsign &CM17Packet::GetSourceCallsign() const
{
	return source;
}

char CM17Packet::GetDestModule() const
{
	return destination.GetCSModule();
}

uint16_t CM17Packet::GetFrameNumber() const
{
	return ntohs(m17.framenumber);
}

uint16_t CM17Packet::GetFrameType() const
{
	return ntohs(m17.lich.frametype);
}

// The TYPE field comes in two layouts. The V#3 layout has the payload type in the
// top nibble (never zero), the legacy layout leaves that nibble empty.
//   legacy: bit 0 stream, bits 1-2 data type (2 = 3200, 3 = 1600), bits 3-4 encryption
//   V#3:    bits 12-15 payload (2 = 3200, 3 = 1600), bits 9-11 encryption

ECodecType CM17Packet::GetCodecType() const
{
	const uint16_t t = GetFrameType();
	const unsigned payload = (0xF000U & t) ? (t >> 12) : (0x3U & (t >> 1));
	switch (payload)
	{
		case 2U:
			return ECodecType::c2_3200;
		case 3U:
			return ECodecType::c2_1600;
		default:
			return ECodecType::none;
	}
}

bool CM17Packet::IsEncrypted() const
{
	const uint16_t t = GetFrameType();
	return (0xF000U & t) ? (0 != (0x7U & (t >> 9))) : (0 != (0x3U & (t >> 3)));
}

//   legacy: bits 5-6 META subtype (0 text, 1 GNSS, 2 ECD) if unencrypted, bits 7-10 CAN
//   V#3:    bits 4-7 META type (0 none, 1 GNSS, 2 ECD, 3 text), bits 0-3 CAN

EM17Meta CM17Packet::GetMetaType() const
{
	const uint16_t t = GetFrameType();
	if (0xF000U & t)
	{
		switch (0xFU & (t >> 4))
		{
			case 1U: return EM17Meta::gnss;
			case 2U: return EM17Meta::ecd;
			case 3U: return EM17Meta::text;
			default: return EM17Meta::none;
		}
	}
	if (IsEncrypted())
		return EM17Meta::none;
	switch (0x3U & (t >> 5))
	{
		case 0U: return EM17Meta::text;
		case 1U: return EM17Meta::gnss;
		case 2U: return EM17Meta::ecd;
		default: return EM17Meta::none;
	}
}

uint8_t CM17Packet::GetCan() const
{
	const uint16_t t = GetFrameType();
	return (0xF000U & t) ? (0xFU & t) : (0xFU & (t >> 7));
}

// same GNSS layout as mrefd: byte 0 source/station, byte 1 bit 7 position valid,
// bytes 3-5 and 6-8 latitude and longitude as signed 24 bit fractions of 90 and 180 degrees
bool CM17Packet::GnssPosition(const uint8_t *meta, double &lat, double &lon)
{
	if (0 == (0x80U & meta[1]))
		return false;

	const int32_t nlat = int32_t(uint32_t(meta[3]) << 24 | uint32_t(meta[4]) << 16 | uint32_t(meta[5]) << 8) >> 8;
	const int32_t nlon = int32_t(uint32_t(meta[6]) << 24 | uint32_t(meta[7]) << 16 | uint32_t(meta[8]) << 8) >> 8;
	lat =  90.0 * nlat / 8388607.0;
	lon = 180.0 * nlon / 8388607.0;
	return (lat >= -90.0) && (lat < 90.0) && (lon >= -180.0) && (lon < 180.0);
}

const uint8_t *CM17Packet::GetPayload() const
{
	return m17.payload;
}

const uint8_t *CM17Packet::GetNonce() const
{
	return m17.lich.nonce;
}

void CM17Packet::SetPayload(const uint8_t *newpayload)
{
	memcpy(m17.payload, newpayload, 16);
}

uint16_t CM17Packet::GetStreamId() const
{
	return ntohs(m17.streamid);
}

uint16_t CM17Packet::GetCRC() const
{
	return ntohs(m17.crc);
}

void CM17Packet::SetCRC(uint16_t crc)
{
	m17.crc = htons(crc);
}

bool CM17Packet::IsLastPacket() const
{
	return ((0x8000u & ntohs(m17.framenumber)) == 0x8000u);
}
