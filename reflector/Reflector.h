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

#include <unordered_map>
#include <chrono>
#include <vector>

#include "Users.h"
#include "Clients.h"
#include "Peers.h"
#include "Protocols.h"
#include "PacketStream.h"
#include "CallingHome.h"

#ifndef NO_DHT
#include "dht-values.h"
#endif


////////////////////////////////////////////////////////////////////////////////////////
// define

// event defines
#define EVENT_NONE      0
#define EVENT_CLIENTS   1
#define EVENT_USERS     2

////////////////////////////////////////////////////////////////////////////////////////
// class

class CReflector
{
public:
	CReflector();
	// destructor
	~CReflector();

	// operation
	bool Start(void);
	void Stop(void);
	void ReloadConfig(const std::string &configPath);

	// clients
	CClients *GetClients(void)                      { m_Clients.Lock(); return &m_Clients; }
	void      ReleaseClients(void)                  { m_Clients.Unlock(); }

	// peers
	CPeers   *GetPeers(void)                        { m_Peers.Lock(); return &m_Peers; }
	void      ReleasePeers(void)                    { m_Peers.Unlock(); }

	// stream opening & closing
	std::shared_ptr<CPacketStream> OpenStream(std::unique_ptr<CDvHeaderPacket> &, std::shared_ptr<CClient>);
	bool IsStreaming(char);
	// a stream opened or closed or a station was heard: write the status file now
	void StatusChanged(void) { m_StatusChanged = true; }
	void EndStream(std::shared_ptr<CPacketStream>);
	void CloseStream(std::shared_ptr<CPacketStream>);

	// get
	const CCallsign &GetCallsign(void) const        { return m_Callsign; }
	nlohmann::json GetModuleMappings(char module);	// the bridges of a module, for XML and WebSocket
	std::time_t GetStartedAt(void) const             { return m_StartedAt; }
	CUsers  *GetUsers(void)                         { m_Users.Lock(); return &m_Users; }
	void    ReleaseUsers(void)                      { m_Users.Unlock(); }

	// check
	bool IsValidModule(char c) const                { return m_Modules.npos!=m_Modules.find(c); }
	char GetEchoModule(void) const                  { return m_EchoModule; }
	// the stream id of the (last) echo playback
	bool IsEchoStream(uint16_t sid) const           { return (0 != sid) && (sid == m_EchoStreamId); }

	// protocol access (for admin socket)
	CProtocols &GetProtocols(void)                  { return m_Protocols; }
	const std::string &GetModules(void) const       { return m_Modules; }
	const std::string &GetTCModules(void) const     { return m_TCmodules; }	// configured; with Modules = auto all of them
	// transcoded now: a configured module, with Modules = auto only while tcd is connected for it
	bool IsTranscoded(char module) const;
	std::string GetTranscodedModules(void) const;
	const std::string &GetConfigPath(void) const    { return m_ConfigPath; }
	void SetConfigPath(const std::string &p)        { m_ConfigPath = p; }

	// notifications

#ifndef NO_DHT
	void GetDHTConfig(const std::string &cs);
#endif

	// stream access
	std::shared_ptr<CPacketStream> GetStream(char);

protected:
#ifndef NO_DHT
	// Publish DHT
	void PutDHTConfig();
	void GetDHTMrefdConfig(const std::string &cs);
#endif

	// threads
	void RouterThread(const char);
	void MaintenanceThread(void);
	bool IsStreamOpen(const std::unique_ptr<CDvHeaderPacket> &);
	char GetStreamModule(std::shared_ptr<CPacketStream>);

	// xml helpers
	void WriteXmlFile(std::ofstream &);
	void JsonReport(nlohmann::json &report);
	void NotifyMappings(void);

	// echo module
	void EchoThread(void);
	char m_EchoModule;
	std::mutex m_EchoMutex;
	std::vector<std::unique_ptr<CPacket>> m_EchoBuffer;
	CDvHeaderPacket m_EchoHeader;
	bool m_EchoHasHeader;
	std::chrono::steady_clock::time_point m_EchoLastFrame;
	std::future<void> m_EchoFuture;
	std::atomic<uint16_t> m_EchoStreamId { 0 };

	// identity
	CCallsign   m_Callsign;
	std::time_t m_StartedAt = 0;
	nlohmann::json m_SentMappings;	// last module_mappings sent, MaintenanceThread only
	std::string m_Modules, m_TCmodules;
	std::string m_ConfigPath;

	// objects
	CUsers     m_Users;            // sorted list of lastheard stations
	CCallingHome m_CallingHome;    // XLX directory registration, if switched on
	CClients   m_Clients;          // list of linked repeaters/nodes/peers's modules
	CPeers     m_Peers;            // list of linked peers
	CProtocols m_Protocols;        // list of supported protocol handlers

	// queues
	std::unordered_map<char, std::shared_ptr<CPacketStream>> m_Stream;

	// threads
	std::atomic<bool> keep_running;
	std::atomic<bool> m_StatusChanged { false };
	std::unordered_map<char, std::future<void>> m_RouterFuture;
	std::future<void> m_MaintenanceFuture;

#ifndef NO_DHT
	// Distributed Hash Table
	dht::DhtRunner node;
	dht::InfoHash refhash;
	dht::InfoHash m17hash;	// our M17-XXX designator, for mrefd interlinks
#endif
};
