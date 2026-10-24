#pragma once

// HyTalkGtrp -- GTRP, the GIS channel of HyTalk radios: login, keep-alive and
// GPS positions over plain TCP, apart from the PNAS signalling
//
// A message is an 8 byte header, 0x10, the message type, a transaction id
// (u32 big endian, a response echoes the request's) and the body length
// (u16 big endian), then the body: TLV elements, tag, length byte, value.
// Numbers are BCD (first digit in the low nibble, 0xF pads), positions are
// NMEA ddmm.mmmm as BCD digits with an implicit decimal point.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#define GTRP_MAGIC                  0x10
#define GTRP_HEADER_SIZE            8

// message types (header byte 1)
#define GTRP_KEEP_ALIVE             0x10    // request and response
#define GTRP_REGISTER_REDIRECT      0x13
#define GTRP_REGISTER_REQUEST       0x14
#define GTRP_REGISTER_RESPONSE      0x15
#define GTRP_DEREGISTER_REQUEST     0x16
#define GTRP_DEREGISTER_RESPONSE    0x17
#define GTRP_LOCATION_REQUEST       0x18
#define GTRP_LOCATION_RESPONSE      0x19
#define GTRP_SUBSCRIBE_REQUEST      0x1a
#define GTRP_SUBSCRIBE_RESPONSE     0x1b
#define GTRP_DESUBSCRIBE_REQUEST    0x1c
#define GTRP_DESUBSCRIBE_RESPONSE   0x1d
#define GTRP_GPS_CONFIG_REQUEST     0x1e
#define GTRP_CONFIG_RESPONSE        0x20
#define GTRP_LOCATION_REPORTING     0x21

// element tags
#define GTRP_IE_RESULT              0x23    // one byte, no length byte
#define GTRP_IE_IPV4                0xa3
#define GTRP_IE_OWN_NUMBER          0xa6
#define GTRP_IE_PEER_NUMBER         0xa7
#define GTRP_IE_CONFIG_FLAG         0xa8
#define GTRP_IE_GPS_BASE            0xa9
#define GTRP_IE_GPS_EXT             0xaa
#define GTRP_IE_GIS_CONFIG          0xab

struct SGtrpPosition
{
	bool valid = false;           // the radio has a fix
	uint32_t time = 0;            // UNIX seconds
	double lat = 0.0, lon = 0.0;  // degrees, south and west negative
	double alt = 0.0;             // metres
	bool hasExt = false;
	double heading = 0.0;         // degrees
	double speed = 0.0;           // km/h
	int power = -1;               // battery percent
	int rssi = 0;
};

struct SGtrpMsg
{
	uint8_t type = 0;
	uint32_t tid = 0;
	std::string own, peer;        // numbers, empty if absent
	bool hasPosition = false;
	SGtrpPosition pos;
	int result = -1;              // of a base response, -1 if absent
};

class CGtrp
{
public:
	// bytes of the first whole message in the stream, 0 if it is not complete yet,
	// -1 if the stream is not GTRP (resync impossible, close the connection)
	static long MessageSize(const uint8_t *data, size_t len);
	// a whole message, false if it is malformed
	static bool Decode(const uint8_t *data, size_t len, SGtrpMsg &msg);

	// server messages
	static std::vector<uint8_t> BaseResponse(uint8_t type, uint32_t tid, uint8_t result = 0);
	// switches the radio's periodic reports on (or off), every interval seconds
	static std::vector<uint8_t> GpsConfig(uint32_t tid, uint16_t interval, bool on);

	// helpers, public for the tests
	static std::string DecodeBcd(const uint8_t *bcd, size_t n);
	static std::vector<uint8_t> EncodeBcd(const std::string &digits);
	static bool DecodePosition(const uint8_t *v, size_t n, SGtrpPosition &pos);
	static void DecodeExt(const uint8_t *v, size_t n, SGtrpPosition &pos);
};
