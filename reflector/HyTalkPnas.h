#pragma once

// HyTalkPnas -- the message layer of Hytera's HyTalk PoC protocol ("PNAS"),
// as the HyTalk app and the P50 radios speak it over KCP/UDP.
//
// A KCP stream carries "Package_Data" units: one options byte (0 = plain,
// 1 = a 4 byte option word follows: 00 00 <type|0x80> <key id>, then the
// encrypted message), then a PNAS message:
//   50 <seq u32> <transaction id u32> <type u8> 00 C0 <total length u24>
// followed by information elements, each <IEI u16> <len u16> <value>.
// All numbers are big-endian. The receiver skips IEs it does not know.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <utility>

// message types
#define PNAS_REGISTER_REQUEST          0x01
#define PNAS_REGISTER_ACCEPT           0x02
#define PNAS_REGISTER_REJECT           0x03
#define PNAS_REGISTER_COMPLETE         0x04
#define PNAS_DEREGISTER_REQUEST        0x05
#define PNAS_DEREGISTER_ACCEPT         0x06
#define PNAS_CALL_REQUEST              0x07
#define PNAS_CALL_ACCEPT               0x08
#define PNAS_CALL_COMPLETE             0x09
#define PNAS_CALL_REJECT               0x0c
#define PNAS_CALL_RELEASE_REQUEST      0x12
#define PNAS_CALL_RELEASE_RESPONSE     0x13
#define PNAS_FLOOR_REQUEST             0x14
#define PNAS_FLOOR_GRANT               0x15
#define PNAS_FLOOR_GRANT_ACK           0x16
#define PNAS_FLOOR_REJECT              0x17
#define PNAS_FLOOR_WAIT                0x18
#define PNAS_FLOOR_INFORM              0x19
#define PNAS_FLOOR_RELEASE             0x1a
#define PNAS_FLOOR_RELEASE_ACK         0x1b
#define PNAS_MESSAGE_REQUEST           0x42
#define PNAS_MESSAGE_RESPONSE          0x43
#define PNAS_HEARTBEAT_REQUEST         0x46
#define PNAS_HEARTBEAT_RESPONSE        0x47
#define PNAS_GROUP_INFO_NOTIFY_REQ     0x48
#define PNAS_GROUP_INFO_NOTIFY_RSP     0x49
#define PNAS_GROUP_INFO_UPDATE_REQ     0x4a
#define PNAS_GROUP_INFO_UPDATE_RSP     0x4b
#define PNAS_GROUP_AFFI_NOTIFY_REQ     0x4c
#define PNAS_GROUP_AFFI_NOTIFY_RSP     0x4d
#define PNAS_UNAUTHENTICATED           0x52
#define PNAS_USER_CONFIG_NOTIFY_RSP    0x73
#define PNAS_UE_CAPABILITY_NOTIFY_REQ  0x78
#define PNAS_UE_CAPABILITY_NOTIFY_RSP  0x79
#define PNAS_GROUP_UPDATE_REPORT_REQ   0x7a
#define PNAS_GROUP_UPDATE_REPORT_RSP   0x7b
#define PNAS_HEARTBEAT_PING            0x8e
#define PNAS_HEARTBEAT_PONG            0x8f

// information elements
#define PNAS_IE_AUDIO_DESCRIPTION      0x0003
#define PNAS_IE_CALL_ATTRIBUTE         0x0004
#define PNAS_IE_CALL_ID                0x0005
#define PNAS_IE_CALL_TYPE              0x0006
#define PNAS_IE_CALLER_NUMBER          0x0008
#define PNAS_IE_CAUSE                  0x0009
#define PNAS_IE_FLOOR_INFORM_TYPE      0x000c
#define PNAS_IE_FLOOR_STATUS           0x000d
#define PNAS_IE_GROUP_NUMBER           0x0010
#define PNAS_IE_IP_ADDRESS             0x001d
#define PNAS_IE_PORT                   0x0021
#define PNAS_IE_CODEC_LIST             0x0028
#define PNAS_IE_USER_NAME              0x002d
#define PNAS_IE_AUDIO_RTP_RES          0x0047
#define PNAS_IE_KEEPALIVE_TIME         0x0049
#define PNAS_IE_SUBSCRIBER_NUMBER      0x004c
#define PNAS_IE_HTTP_PORT              0x0063
#define PNAS_IE_HTTPS_PORT             0x0064
#define PNAS_IE_PRESENCE_SERVER        0x0065
#define PNAS_IE_GIS_SERVER             0x00CC
#define PNAS_IE_UID_TYPE               0x0073
#define PNAS_IE_REALM                  0x0074
#define PNAS_IE_MASTER_IP              0x0076
#define PNAS_IE_MASTER_PORT            0x0077
#define PNAS_IE_AMBE_PARAM             0x0081
#define PNAS_IE_CODEC_PRIORITY         0x00c2
#define PNAS_IE_AUTH_CHALLENGE         0x00c8
#define PNAS_IE_AUTH_RESPONSE          0x00c9
#define PNAS_IE_GROUP_UPDATE_ID        0x00cb
#define PNAS_IE_GROUP_AFFI_LIST        0x00d7
#define PNAS_IE_AMRWB_PARAM            0x00ec
#define PNAS_IE_SSRC                   0x00ed
#define PNAS_IE_AUTH_RESPONSE_OLD      0x00a5
#define PNAS_IE_GROUP_LIST_DISPLAY     0x0149

// codec bits of the codec list IE
#define PNAS_CODEC_AMRWB               0x04
#define PNAS_CODEC_AMBE                0x40

struct SPnasIe
{
	uint16_t iei;
	std::vector<uint8_t> value;
};

// one decoded PNAS message
class CPnasMsg
{
public:
	uint8_t type = 0;
	uint32_t seq = 0;
	uint32_t tid = 0;
	std::vector<SPnasIe> ies;

	// a whole message from data: false if the header is broken or an IE runs past the end
	bool Parse(const uint8_t *data, size_t len);

	const SPnasIe *Find(uint16_t iei) const;
	bool Has(uint16_t iei) const { return Find(iei) != nullptr; }
	std::string GetString(uint16_t iei) const;
	// a 1 to 4 byte big-endian value, def if the IE is missing or longer
	uint32_t GetUint(uint16_t iei, uint32_t def = 0) const;

	// the IEs inside a value (AudioDescription, GroupAffiNumList)
	static bool ParseIes(const uint8_t *data, size_t len, std::vector<SPnasIe> &out);
};

// builds one PNAS message
class CPnasWriter
{
public:
	CPnasWriter(uint8_t type, uint32_t tid);

	CPnasWriter &AddBytes(uint16_t iei, const uint8_t *data, size_t len);
	CPnasWriter &AddBytes(uint16_t iei, const std::vector<uint8_t> &v) { return AddBytes(iei, v.data(), v.size()); }
	CPnasWriter &AddUint8(uint16_t iei, uint8_t v);
	CPnasWriter &AddUint16(uint16_t iei, uint16_t v);
	CPnasWriter &AddUint32(uint16_t iei, uint32_t v);
	CPnasWriter &AddString(uint16_t iei, const std::string &s, size_t maxlen = 45);
	// type 1 + IPv4 address in network order
	CPnasWriter &AddIpv4(uint16_t iei, uint32_t addr_be);

	// the message with header, seq is the sender's running message counter
	std::vector<uint8_t> Finish(uint32_t seq) const;

	// an IE body built with the same helpers (for nested IEs)
	const std::vector<uint8_t> &Body(void) const { return m_Body; }

	uint8_t Type(void) const { return m_Type; }
	uint32_t Tid(void) const { return m_Tid; }

private:
	uint8_t m_Type;
	uint32_t m_Tid;
	std::vector<uint8_t> m_Body;
};

// Package_Data framing and the static keys of the HyTalk stack.
// encType 0 is plain, 1..3 AES-128/192/256-CBC, 4 DES-CBC, 5 3DES-CBC, 6 RC4
namespace PnasPackage
{
	// takes the first unit from a KCP stream buffer: 1 one unit taken (msg holds the
	// plain message, encType/keyId what it came with), 0 needs more data,
	// -1 garbage (the caller drops the session)
	int Take(std::vector<uint8_t> &stream, std::vector<uint8_t> &msg, uint8_t &encType, uint8_t &keyId);

	// the app's keys from the operator's key file (80 bytes as hex text); false with err
	bool LoadKeys(const std::string &path, std::string &err);
	bool HaveKeys(void);

	// one unit for a message, encrypted like the peer's last unit
	bool Wrap(const std::vector<uint8_t> &msg, uint8_t encType, uint8_t keyId, std::vector<uint8_t> &out);
}

// HTTP digest the HyTalk client computes for REGISTER, as lowercase hex:
// MD5(MD5(user:realm:password) ":" nonce ":" MD5("REGISTER:123456789"))
std::string PnasDigest(const std::string &user, const std::string &realm, const std::string &password, const std::string &nonce);
// the same with HA1 given
std::string PnasDigestHa1(const std::string &ha1, const std::string &nonce);
std::string PnasMd5Hex(const std::string &s);
