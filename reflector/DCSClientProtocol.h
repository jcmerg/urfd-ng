#pragma once

// DCSClientProtocol -- Connects to a remote DCS reflector as a client node
// URFD acts as a DCS hotspot/node connecting to an external DCS reflector.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "Defines.h"
#include "Timer.h"
#include "LinkRetry.h"
#include "Protocol.h"
#include "DVHeaderPacket.h"
#include "DVFramePacket.h"

// DCS client protocol constants (aligned with ircDDBGateway timings)
#define DCSCLI_KEEPALIVE_PERIOD     5   // seconds between keepalives (g4klx: 5s)
#define DCSCLI_KEEPALIVE_TIMEOUT    60  // seconds before connection is considered dead (g4klx: 60s)

// A single module mapping: remote DCS reflector module <-> local reflector module
struct SDcsClientMapping
{
	std::string host;
	uint16_t    port;
	CIp         ip;
	bool        unresolved = false;	// host not found on the last try (LinkRetry.h)
	char        remoteModule;   // module on the remote DCS reflector
	char        localModule;    // module on our reflector
	std::string reflector;      // the remote reflector's callsign, from its streams
	bool        connected;
	CTimer      keepaliveTimer;
	CTimer      reconnectTimer;
	unsigned    tries = 0;      // link attempts since the last link (LinkRetry.h)

	SDcsClientMapping() : port(30051), remoteModule('A'), localModule('A'), connected(false) {}
};

// Stream cache for outbound DCS packets (header + sequence counter per module)
struct SDcsClientStreamCache
{
	CDvHeaderPacket header;
	uint32_t        seqCounter;
};

class CDcsClientProtocol : public CProtocol
{
public:
	// initialization
	bool Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6);

	// task
	void Task(void);

	// admin API
	void RequestReconnect(void) { m_ReconnectRequested = true; Wake(); }
	bool AddMapping(const std::string &host, uint16_t port, char remoteMod, char localMod);
	bool RemoveMapping(char localMod);
	std::vector<SDcsClientMapping> GetMappings(void) const;

protected:
	// connection management
	void HandleConnections(void);
	void SendConnect(SDcsClientMapping &mapping);
	void SendDisconnect(SDcsClientMapping &mapping);
	void SendKeepalive(SDcsClientMapping &mapping);


	// incoming packet handling
	void HandleIncoming(const CBuffer &Buffer, const CIp &Ip);
	void OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip);

	// outbound queue
	void HandleQueue(void);
	void HandleKeepalives(void) {}

	// packet decoding (reuse DCS wire format)
	bool IsValidDvPacket(const CBuffer &, std::unique_ptr<CDvHeaderPacket> &, std::unique_ptr<CDvFramePacket> &);
	bool IsValidConnectAckPacket(const CBuffer &, CCallsign *, char *);
	bool IsValidConnectNackPacket(const CBuffer &, CCallsign *, char *);
	bool IsValidKeepAlivePacket(const CBuffer &, CCallsign *);

	// packet encoding
	void EncodeConnectPacket(CBuffer *, const SDcsClientMapping &);
	void EncodeDisconnectPacket(CBuffer *, const SDcsClientMapping &);
	// a poll as ircDDBGateway sends it: our callsign with the module we link, 0x00, the
	// reflector with its module; reflector: its first 8 bytes as the reflector sent them
	void EncodeKeepAlivePacket(CBuffer *, const SDcsClientMapping &, const uint8_t *reflector = nullptr);
	// the reflector's callsign, 7 characters: as it sent it, else from the host name
	// (dcs001.xreflector.net -> DCS001, xlx262 -> DCS262 as ircDDBGateway does)
	std::string ReflectorName(const SDcsClientMapping &) const;
	// stream id 0 (a reflector's broken loop sends nothing else): dropped, logged once a minute
	unsigned m_ZeroSid = 0;        // packets with stream id 0 dropped since the last line
	bool     m_ZeroSidLogged = false;
	CTimer   m_ZeroSidTimer;
	void EncodeDCSPacket(const CDvHeaderPacket &, const CDvFramePacket &, uint32_t seq, CBuffer *) const;
	void EncodeLastDCSPacket(const CDvHeaderPacket &, const CDvFramePacket &, uint32_t seq, CBuffer *) const;

	// mapping helpers
	SDcsClientMapping *FindMappingByLocal(char localMod);
	SDcsClientMapping *FindMappingByRemote(const CIp &ip, char remoteMod);
	SDcsClientMapping *FindMappingByIp(const CIp &ip);

private:
	// config
	CCallsign   m_ClientCallsign;

	// module mappings (protected by m_MappingMutex)
	mutable std::mutex m_MappingMutex;
	std::vector<SDcsClientMapping> m_Mappings;

	// outbound stream cache per local module
	std::unordered_map<char, SDcsClientStreamCache> m_StreamsCache;

	// reconnect flag (set by admin API)
	std::atomic<bool> m_ReconnectRequested{false};
};
