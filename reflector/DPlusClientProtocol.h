#pragma once

// DPlusClientProtocol -- Connects to a remote DPlus reflector as a client
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "Defines.h"
#include "Timer.h"
#include "LinkRetry.h"
#include "Protocol.h"
#include "DVHeaderPacket.h"
#include "DVFramePacket.h"

// DPlus client constants
#define DPLUSCLI_KEEPALIVE_PERIOD     1   // seconds (DPlus uses fast keepalive)
#define DPLUSCLI_KEEPALIVE_TIMEOUT    30  // seconds (ircDDBGateway: 30)
#define DPLUSCLI_HANDSHAKE_TIMEOUT    10  // seconds for connect+login handshake

// DPlus client state machine
enum class EDPlusState
{
	DISCONNECTED,
	CONNECTING,      // sent connect packet, waiting for echo
	LOGGING_IN,      // sent login packet, waiting for OKRW/BUSY
	CONNECTED        // running, sending keepalives
};

// A single DPlus module mapping
struct SDPlusClientMapping
{
	std::string host;
	uint16_t    port;
	CIp         ip;
	bool        unresolved = false;	// host not found on the last try (LinkRetry.h)
	char        remoteModule;   // not used in connect (DPlus discovers module from header)
	char        localModule;
	std::string reflector;      // the remote reflector's callsign, from its streams
	bool        connected;
	EDPlusState state;
	CTimer      stateTimer;
	CTimer      keepaliveTimer;
	CTimer      timeoutTimer;
	unsigned    tries = 0;      // link attempts since the last link (LinkRetry.h)

	SDPlusClientMapping() : port(20001), remoteModule('A'), localModule('A'),
		connected(false), state(EDPlusState::DISCONNECTED) {}
};

class CDPlusClientProtocol : public CProtocol
{
public:
	bool Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6);
	void Task(void);

	void RequestReconnect(void) { m_ReconnectRequested = true; Wake(); }
	bool AddMapping(const std::string &host, uint16_t port, char remoteMod, char localMod);
	bool RemoveMapping(char localMod);
	std::vector<SDPlusClientMapping> GetMappings(void) const;

protected:
	void HandleConnections(void);
	void HandleIncoming(const CBuffer &Buffer, const CIp &Ip);
	void OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip);

	void HandleQueue(void);
	void HandleKeepalives(void) {}

	// packet decoding
	bool IsValidConnectEcho(const CBuffer &);
	bool IsValidLoginAck(const CBuffer &);
	bool IsValidLoginNack(const CBuffer &);
	bool IsValidDisconnectPacket(const CBuffer &);
	bool IsValidKeepAlivePacket(const CBuffer &);
	bool IsValidDvHeaderPacket(const CBuffer &, std::unique_ptr<CDvHeaderPacket> &);
	bool IsValidDvFramePacket(const CBuffer &, std::unique_ptr<CDvFramePacket> &);

	// packet encoding
	void EncodeConnectPacket(CBuffer *);
	void EncodeLoginPacket(CBuffer *);
	void EncodeDisconnectPacket(CBuffer *);
	void EncodeKeepAlivePacket(CBuffer *);
	bool EncodeDvHeaderPacket(const CDvHeaderPacket &, CBuffer &) const;
	bool EncodeDvFramePacket(const CDvFramePacket &, CBuffer &) const;

	SDPlusClientMapping *FindMappingByLocal(char localMod);
	SDPlusClientMapping *FindMappingByRemote(const CIp &ip, char remoteMod);
	SDPlusClientMapping *FindMappingByIp(const CIp &ip);

private:
	CCallsign   m_ClientCallsign;

	mutable std::mutex m_MappingMutex;
	std::vector<SDPlusClientMapping> m_Mappings;

	std::atomic<bool> m_ReconnectRequested{false};
};
