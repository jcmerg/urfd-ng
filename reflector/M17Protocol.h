//  Copyright © 2015 Jean-Luc Deltombe (LX3JL). All rights reserved.

// urfd -- The universal reflector
// Copyright © 2021 Thomas A. Early N7TAE
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include "Defines.h"
#include "Timer.h"
#include "Protocol.h"
#include "DVHeaderPacket.h"
#include "DVFramePacket.h"
#include "M17CRC.h"

#include <regex>

////////////////////////////////////////////////////////////////////////////////////////
// define

////////////////////////////////////////////////////////////////////////////////////////
// class

class CM17StreamCacheItem
{
public:
	CM17StreamCacheItem() : m_iSeqCounter(0) {}

	CDvHeaderPacket m_dvHeader;
	uint32_t        m_iSeqCounter;
	bool            m_bFromPeer = false;   // from an interlinked mrefd, not sent to other ones
};

class CM17Protocol : public CProtocol
{
public:
	// initialization
	bool Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6);

	// task
	void Task(void);

protected:
	// queue helper
	void HandleQueue(void);

	// keepalive helpers
	void HandleKeepalives(void);

	// mrefd interlink helpers, PEER_DROP marks peer data for no shared module
	static constexpr char PEER_DROP = '!';
	static std::string Designator(const CCallsign &);
	void HandlePeerLinks(void);
	char StripPeerModule(CBuffer &, const CIp &);
	void OnInterlinkConnectIn(const CCallsign &, const char *, const CIp &);
	void OnInterlinkAckIn(const CCallsign &, const char *, const CIp &);
	bool RemovePeer(const CIp &);
	bool IsPeer(const CIp &);
	bool IsPeerMastering(char);
	bool IsInterlinkListed(const CCallsign &, const CIp &, const char *, bool &) const;
	void SendToPeerClient(const SM17Frame &, const CClient &, char);

	// stream helpers
	void OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &, const CIp &, char peerModule = ' ');

	// client helpers
	bool IsListenOnlyClient(const CIp &);

	// position helpers
	void OnGnssIn(const CCallsign &, const uint8_t *);

	// echo helpers
	bool IsEchoPlaybackFor(const CClient &, char) const;

	// packet mode helpers
	void OnPacketModePacketIn(const CBuffer &, const CIp &, char peerModule = ' ');

	// the callsign checks of mrefd, on the raw M17 address
	static std::string M17Address(const uint8_t *);
	bool IsM17ClientCallsign(const uint8_t *) const;

	// packet decoding helpers
	bool IsValidConnectPacket(const CBuffer &, CCallsign &, char &);
	bool IsValidListenPacket(const CBuffer &, CCallsign &, char &);
	bool IsValidDisconnectPacket(const CBuffer &, CCallsign &);
	bool IsValidKeepAlivePacket(const CBuffer &, CCallsign &);
	bool IsValidDvPacket(const CBuffer &, const CIp &, char, std::unique_ptr<CDvHeaderPacket> &, std::unique_ptr<CDvFramePacket> &);
	bool IsValidPacketModePacket(const CBuffer &) const;
	bool IsValidInterlinkPacket(const CBuffer &, const char *, CCallsign &, char *) const;

	// packet encoding helpers
	void EncodeKeepAlivePacket(CBuffer &);
	void EncodeInterlinkPacket(CBuffer &, const char *tag, const char *modules) const;
	void EncodeInterlinkShortPacket(CBuffer &, const char *) const;
	void EncodeM17Packet(SM17Frame &, const CDvHeaderPacket &, const CDvFramePacket *, uint32_t) const;

protected:
	// for keep alive
	CTimer m_LastKeepaliveTime;

	// mrefd interlinks
	CTimer    m_LastPeersLinkTime;
	CCallsign m_InterlinkCallsign;
	bool      m_bInterlink = false;

	// for queue header caches
	std::unordered_map<char, CM17StreamCacheItem> m_StreamsCache;

private:
	CM17CRC m17crc;

	// as mrefd: a client (also a data source) and the more permissive listen-only callsign
	const std::regex m_ClientRegEx { "^([0-9]?[A-Z]{1,2}[0-9]{0,2}/)?[0-9]?[A-Z]{1,2}[0-9]{1,2}[A-Z]{1,4}([ -/\\.][A-Z0-9 -/\\.]*)?$", std::regex::extended };
	const std::regex m_ListenRegEx { "^[0-9]?[A-Z][A-Z0-9]{2,8}$", std::regex::extended };
};
