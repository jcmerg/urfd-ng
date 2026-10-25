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


#include <string.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>

#include "Global.h"
#include "Utils.h"
#include "DMRMMDVMProtocol.h"
#include "MMDVMClientProtocol.h"
#include "SvxReflectorProtocol.h"
#include "SvxProtocol.h"
#include "DCSClientProtocol.h"
#include "DExtraClientProtocol.h"
#include "DPlusClientProtocol.h"
#include "YSFClientProtocol.h"
#include "NXDNProtocol.h"
#include "Notifier.h"
#include "Plugins.h"

CReflector::CReflector()
{
	m_StartedAt = std::time(nullptr);
	m_EchoModule = ' ';
	m_EchoHasHeader = false;
}

CReflector::~CReflector()
{
	keep_running = false;
	if ( m_MaintenanceFuture.valid() )
	{
		m_MaintenanceFuture.get();
	}
	if ( m_EchoFuture.valid() )
	{
		m_EchoFuture.get();
	}

	for (auto it=m_Modules.cbegin(); it!=m_Modules.cend(); it++)
	{
		if (m_RouterFuture[*it].valid())
			m_RouterFuture[*it].get();
	}
	m_RouterFuture.clear();
	m_Stream.clear();
}


////////////////////////////////////////////////////////////////////////////////////////
// operation

bool CReflector::Start(void)
{
	// get config stuff
	const auto cs(g_Configure.GetString(g_Keys.names.callsign));
	m_Callsign.SetCallsign(cs, false);
	m_Modules.assign(g_Configure.GetString(g_Keys.modules.modules));
	const auto tcmods(g_Configure.GetString(g_Keys.tc.modules));
	const auto port = g_Configure.GetUnsigned(g_Keys.tc.port);

#ifndef NO_DHT
	// start the dht instance
	refhash = dht::InfoHash::get(cs);
	node.run(17171, dht::crypto::generateIdentity(cs), true, 59973);
	node.bootstrap(g_Configure.GetString(g_Keys.names.bootstrap), "17171");
#endif

	// let's go!
	keep_running = true;

	// init transcoder comms
	if (port)
	{
		g_TCServer.SetPassword(g_Configure.GetString(g_Keys.tc.password));
		if (g_TCServer.Open(g_Configure.GetString(g_Keys.tc.bind), tcmods, port))
			return true;
	}

	// init gate keeper. It can only return true!
	g_GateKeeper.Init();

	// init dmrid directory. No need to check the return value.
	g_LDid.LookupInit();

	// init dmrid directory. No need to check the return value.
	g_LNid.LookupInit();

	// init wiresx node directory. Likewise with the return vale.
	g_LYtr.LookupInit();

	// the logins from a database, before the protocols that look them up
	{
		SCredConfig cc;
		cc.backend = g_Configure.GetString(g_Keys.cred.backend);
		cc.path = g_Configure.GetString(g_Keys.cred.path);
		cc.host = g_Configure.GetString(g_Keys.cred.host);
		cc.port = g_Configure.GetUnsigned(g_Keys.cred.port);
		cc.database = g_Configure.GetString(g_Keys.cred.database);
		cc.user = g_Configure.GetString(g_Keys.cred.user);
		cc.password = g_Configure.GetString(g_Keys.cred.password);
		cc.table = g_Configure.GetString(g_Keys.cred.table);
		cc.timeout = g_Configure.GetUnsigned(g_Keys.cred.timeout);
		cc.cachettl = g_Configure.GetUnsigned(g_Keys.cred.cachettl);
		cc.negativettl = g_Configure.GetUnsigned(g_Keys.cred.negativettl);
		cc.cachesize = g_Configure.GetUnsigned(g_Keys.cred.cachesize);
		if (! g_Credentials.Open(cc))
			return true;
	}

	// create protocols
	if (! m_Protocols.Init())
	{
		m_Protocols.Close();
		return true;
	}

	// echo module
	if (g_Configure.Contains(g_Keys.echo.module)
		&& (!g_Configure.Contains(g_Keys.echo.enable) || g_Configure.GetBoolean(g_Keys.echo.enable)))
	{
		m_EchoModule = g_Configure.GetString(g_Keys.echo.module)[0];
		std::cout << "Echo module: " << m_EchoModule << std::endl;
	}

	// start one thread per reflector module
	for (auto c : m_Modules)
	{
		auto stream = std::make_shared<CPacketStream>(c);
		if (stream)
		{
			// if it's a transcoded module, then we need to initialize the codec stream
			if (port)
			{
				if (std::string::npos != tcmods.find(c))
				{
					if (stream->InitCodecStream())
						return true;
				}
			}
			m_Stream[c] = stream;
		}
		else
		{
			std::cerr << "Could not make a CPacketStream for module '" << c << "'" << std::endl;
			return true;
		}
		try
		{
			m_RouterFuture[c] = std::async(std::launch::async, &CReflector::RouterThread, this, c);
		}
		catch(const std::exception& e)
		{
			std::cerr << "Cannot start module '" << c << "' thread: " << e.what() << '\n';
			keep_running = false;
			return true;
		}
	}

	// start the reporting thread
	try
	{
		m_MaintenanceFuture = std::async(std::launch::async, &CReflector::MaintenanceThread, this);
	}
	catch(const std::exception& e)
	{
		std::cerr << "Cannot start the dashboard data report thread: " << e.what() << '\n';
	}

	// register in the XLX directory, if switched on
	m_CallingHome.Start();

	// start echo thread
	if (m_EchoModule != ' ')
	{
		try
		{
			m_EchoFuture = std::async(std::launch::async, &CReflector::EchoThread, this);
		}
		catch(const std::exception& e)
		{
			std::cerr << "Cannot start echo thread: " << e.what() << '\n';
		}
	}

	// start admin socket
	if (!g_AdminSocket.Start())
	{
		std::cerr << "Admin socket failed to start" << std::endl;
		// non-fatal, continue without admin interface
	}

	// WebSocket for the dashboard; non-fatal as well
	CNotifierManager::Instance().Start();

#ifndef NO_DHT
	PutDHTConfig();
#endif

	return false;
}

void CReflector::ReloadConfig(const std::string &configPath)
{
	std::cout << "Reloading configuration from " << configPath << std::endl;

	// Save current config in case reload fails
	auto savedConfig = g_Configure.GetData();

	try
	{
		// Re-read the configuration file
		if (g_Configure.ReadData(configPath))
		{
			std::cerr << "Config reload: ReadData returned error, restoring previous configuration" << std::endl;
			g_Configure.SetData(savedConfig);
			return;
		}

		// Reload TG mappings (preserve dynamic entries)
		for (auto *mmdvm : m_Protocols.MmdvmClients())
		{
			mmdvm->GetTGMap().ReloadStaticFromConfig(MmdvmClientPrefix(mmdvm->GetInstance()), mmdvm->GetName());
			mmdvm->RequestReconnect();
		}

		auto *svxProto = m_Protocols.FindByType(EProtocol::svxreflector);
		if (svxProto)
		{
			auto *svx = static_cast<CSvxReflectorProtocol *>(svxProto);
			svx->ReloadStaticTGMap();
		}

		auto *svxsProto = m_Protocols.FindByType(EProtocol::svx);
		if (svxsProto)
		{
			auto *svxs = static_cast<CSvxProtocol *>(svxsProto);
			svxs->ReloadStaticTGMap();
		}

		// the plugins have read their sections again
		for (size_t i = 0; i < g_Plugins.Size(); i++)
			g_Plugins.Get(i).Reload(m_Protocols.FindByType(g_Plugins.GetId(i)));

		// Reload whitelist, blacklist, and interlink files
		g_GateKeeper.ReloadLists();

		// logins changed in the database count from now on; the [Credentials]
		// settings themselves only change with a restart
		g_Credentials.Flush();

		// Update transcoder modules string
		m_TCmodules.assign(g_Configure.GetString(g_Keys.tc.modules));

		std::cout << "Configuration reloaded successfully" << std::endl;
	}
	catch (const std::exception &e)
	{
		std::cerr << "Config reload failed: " << e.what() << std::endl;
		std::cerr << "Restoring previous configuration" << std::endl;
		g_Configure.SetData(savedConfig);
	}
}

void CReflector::Stop(void)
{
	// stop admin socket and WebSocket
	g_AdminSocket.Stop();
	CNotifierManager::Instance().Stop();

	// stop & delete all threads
	keep_running = false;

	// end the codec threads before their transcoder sockets close: they poll
	// g_TCServer, and global objects go in reverse order, g_TCServer before
	// g_Reflector, whose streams would end them only then (heap corruption,
	// "malloc_consolidate(): unaligned fastbin chunk" at every stop)
	for (auto &stream : m_Stream)
		stream.second->StopCodecStream();

	// stop transcoder comms
	// if it was never opened, then there is nothing to close;
	g_TCServer.Close();

	// stop & delete report threads
	if ( m_MaintenanceFuture.valid() )
	{
		m_MaintenanceFuture.get();
	}

	m_CallingHome.Stop();

	// stop echo thread
	if ( m_EchoFuture.valid() )
	{
		m_EchoFuture.get();
	}

	// stop & delete all router threads; they wait for packets, so wake them
	for (auto &stream : m_Stream)
		stream.second->Shutdown();
	for (auto c : m_Modules)
	{
		if (m_RouterFuture[c].valid())
			m_RouterFuture[c].get();
	}

	// close protocols
	m_Protocols.Close();
	g_Credentials.Close();

	// close gatekeeper
	g_GateKeeper.Close();

	// close databases
	g_LDid.LookupClose();
	g_LNid.LookupClose();
	g_LYtr.LookupClose();

#ifndef NO_DHT
	// kill the DHT
	node.cancelPut(refhash, toUType(EUrfdValueID::Config));
	node.cancelPut(refhash, toUType(EUrfdValueID::Peers));
	node.cancelPut(refhash, toUType(EUrfdValueID::Clients));
	node.cancelPut(refhash, toUType(EUrfdValueID::Users));
	if (g_Configure.Contains(g_Keys.m17.callsign))
		node.cancelPut(m17hash, toUType(EMrefdValueID::Config));
	node.shutdown({}, true);
	node.join();
#endif
}

////////////////////////////////////////////////////////////////////////////////////////
// stream opening & closing

bool CReflector::IsStreaming(char module)
{
	return false;
}

// clients MUST have bee locked by the caller so we can freely access it within the function
std::shared_ptr<CPacketStream> CReflector::OpenStream(std::unique_ptr<CDvHeaderPacket> &DvHeader, std::shared_ptr<CClient>client)
{
	// check sid is not zero
	if ( 0U == DvHeader->GetStreamId() )
	{
		std::cerr << "StreamId for client " << client->GetCallsign() << " is zero!" << std::endl;
		return nullptr;
	}

	// check if client is valid candidate
	if ( ! m_Clients.IsClient(client) || client->IsAMaster() )
	{
		return nullptr;
	}

	// check if no stream with same streamid already open
	// to prevent loops
	if ( IsStreamOpen(DvHeader) )
	{
		std::cerr << "Detected stream loop on module " << DvHeader->GetRpt2Module() << " for client " << client->GetCallsign() << " with sid " << DvHeader->GetStreamId() << std::endl;
		return nullptr;
	}

	// check if module needs a transcoder and if it's connected
	char module = DvHeader->GetRpt2Module();
	auto tcmods = g_Configure.GetString(g_Keys.tc.modules);
	if (tcmods.find(module) != std::string::npos && g_TCServer.GetFD(module) < 0)
	{
		std::cout << "Module " << module << " waiting for transcoder, rejecting stream from " << client->GetCallsign() << std::endl;
		return nullptr;
	}

	// set the packet module from the header's target module (set by protocol handler)
	char packetMod = DvHeader->GetRpt2Module();
	if (packetMod == ' ') packetMod = client->GetReflectorModule();
	DvHeader->SetPacketModule(packetMod);
	// get the module's queue
	module = DvHeader->GetPacketModule();
	auto stream = GetStream(module);
	if ( stream == nullptr )
	{
		std::cerr << "Can't find module '" << module << "' for Client " << client->GetCallsign() << std::endl;
		return nullptr;
	}

	// is it available ?
	if ( stream->OpenPacketStream(*DvHeader, client) )
	{
		// stream open, mark client as master
		// so that it can't be deleted
		client->SetMasterOfModule(module);

		// update last heard time
		client->Heard();

		// report
		std::cout << std::showbase << std::hex;
		std::cout << "Opening stream on module " << module << " for client " << client->GetCallsign() << " with sid " << ntohs(DvHeader->GetStreamId()) << " by user " << DvHeader->GetMyCallsign() << std::endl;
		std::cout << std::noshowbase << std::dec;

		// and push header packet
		stream->Push(std::move(DvHeader));
		StatusChanged();

		// WebSocket clients see the station on the module; via_peer like Hearing():
		// the peer for a stream from an interlinked reflector, else this one
		CCallsign moduleCs;
		moduleCs.SetCSModule(module);
		GetUsers()->StationStarted(stream->GetUserCallsign(), client->GetCallsign(), moduleCs, client->IsPeer() ? client->GetCallsign() : m_Callsign, client->GetProtocolName());
		ReleaseUsers();
	}
	return stream;
}

void CReflector::CloseStream(std::shared_ptr<CPacketStream> stream)
{
	if ( stream != nullptr )
	{
		// Wait for transcoder pipeline to finish (input queue + pending responses)
		// Without this, the end-of-TX frame gets lost because ClosePacketStream
		// sets m_IsOpen=false and CodecStream discards pending transcoder responses
		{
			int waitMs = 0;
			while ((!stream->IsEmpty() || !stream->IsCodecPipelineEmpty()) && waitMs < 1000)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				waitMs += 10;
			}
		}

		// Wait for output queue to fully drain (router distributes to protocols)
		while (!stream->IsEmpty())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		GetClients();	// lock clients

		// get and check the master
		std::shared_ptr<CClient>client = stream->GetOwnerClient();
		if ( client != nullptr )
		{
			// client no longer a master
			client->NotAMaster();

			std::cout << "Closing stream of module " << GetStreamModule(stream) << std::endl;
		}

		// also without an owner, or the module stays on air in the dashboards
		GetUsers()->StationStopped(GetStreamModule(stream));
		ReleaseUsers();

		// release clients
		ReleaseClients();

		// and stop the queue
		stream->ClosePacketStream();
		StatusChanged();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// router threads

void CReflector::RouterThread(const char ThisModule)
{
	auto pitem = m_Stream.find(ThisModule);
	if (m_Stream.end() == pitem)
	{
		std::cerr << "Module '" << ThisModule << " CPacketStream doesn't exist! aborting RouterThread()" << std::endl;
		return;
	}
	const auto streamIn = pitem->second;
	while (keep_running)
	{
		// wait until something shows up, nullptr when shutting down
		auto packet = streamIn->PopWait();
		if (!packet)
			break;

		packet->SetPacketModule(ThisModule);

		// Refresh dynamic TG timers on all protocols when a new transmission starts
		if (packet->IsDvHeader())
		{
			m_Protocols.Lock();
			for (auto *cli : m_Protocols.MmdvmClients())
				cli->GetTGMap().RefreshActivityByModule(ThisModule);
			m_Protocols.Unlock();
			auto *svxProto = m_Protocols.FindByType(EProtocol::svxreflector);
			if (svxProto)
				static_cast<CSvxReflectorProtocol *>(svxProto)->RefreshActivityByModule(ThisModule);
			auto *svxsProto = m_Protocols.FindByType(EProtocol::svx);
			if (svxsProto)
				static_cast<CSvxProtocol *>(svxsProto)->RefreshActivityByModule(ThisModule);
		}

		// get source protocol type from stream owner, and the instance that made
		// the client where several of one protocol run (MMDVMClient)
		EProtocol srcProto = EProtocol::none;
		auto ownerClient = streamIn->GetOwnerClient();
		const CProtocol *srcInstance = nullptr;
		if (ownerClient)
		{
			srcProto = ownerClient->GetProtocol();
			srcInstance = ownerClient->GetOwnerProtocol();
		}

		// iterate on all protocols
		m_Protocols.Lock();
		for ( auto it=m_Protocols.begin(); it!=m_Protocols.end(); it++ )
		{
			// bidirectional block: skip if target blocks source or vice versa
			EProtocol dstProto = (*it)->GetProtocolType();
			if ((*it)->IsSourceBlocked(srcProto))
			{
				if (packet->IsDvHeader() && srcProto != dstProto)
					std::cout << "Router: " << g_GateKeeper.ProtocolName(srcProto) << " -> " << g_GateKeeper.ProtocolName(dstProto) << " blocked on module " << ThisModule << std::endl;
				continue;
			}
			if (ownerClient && (*it)->IsSourceClientBlocked(*ownerClient))
			{
				// only worth a line when the other MMDVMClient connection serves the module
				const bool serves = (dstProto != EProtocol::mmdvmclient) || static_cast<CMMDVMClientProtocol *>((*it).get())->GetTGMap().IsModuleInUse(ThisModule);
				if (packet->IsDvHeader() && (*it).get() != srcInstance && serves)
				{
					const std::string dstName = (dstProto == EProtocol::mmdvmclient) ? static_cast<CMMDVMClientProtocol *>((*it).get())->GetName() : g_GateKeeper.ProtocolName(dstProto);
					const auto *srcCli = dynamic_cast<const CMMDVMClientProtocol *>(srcInstance);
					std::cout << "Router: " << (srcCli ? srcCli->GetName() : g_GateKeeper.ProtocolName(srcProto)) << " -> " << dstName << " blocked on module " << ThisModule << std::endl;
				}
				continue;
			}
			// the other way round: the source blocks the destination, by type or,
			// for one MMDVMClient connection, by its name
			bool reverseBlocked = false;
			const unsigned dstConn = (*it)->GetMmdvmConnection();
			auto blocksDst = [&](const CProtocol *p) { return p->IsSourceBlocked(dstProto) || (dstConn && p->IsMmdvmConnectionBlocked(dstConn)); };
			if (srcInstance)
				reverseBlocked = blocksDst(srcInstance);	// only the instance the call came from
			else
			{
				for (auto sit=m_Protocols.begin(); sit!=m_Protocols.end(); sit++)
				{
					if ((*sit)->GetProtocolType() == srcProto && blocksDst((*sit).get()))
					{ reverseBlocked = true; break; }
				}
			}
			if (reverseBlocked)
			{
				if (packet->IsDvHeader())
					std::cout << "Router: " << g_GateKeeper.ProtocolName(srcProto) << " -> " << g_GateKeeper.ProtocolName(dstProto) << " blocked on module " << ThisModule << std::endl;
				continue;
			}

			auto copy = packet->Copy();

			// if packet is header, update RPT2 according to protocol
			if ( copy->IsDvHeader() )
			{
				// make the protocol-patched reflector callsign
				CCallsign csRPT = (*it)->GetReflectorCallsign();
				csRPT.SetCSModule(ThisModule);
				// and put it in the copy
				(dynamic_cast<CDvHeaderPacket *>(copy.get()))->SetRpt2Callsign(csRPT);
			}

			(*it)->Push(std::move(copy));
		}
		m_Protocols.Unlock();

		// echo: buffer packets for echo module (skip echo playback packets)
		if (ThisModule == m_EchoModule && (packet->GetStreamId() < 0xEC00 || packet->GetStreamId() > 0xECFF))
		{
			std::lock_guard<std::mutex> lock(m_EchoMutex);
			if (packet->IsDvHeader())
			{
				m_EchoBuffer.clear();
				m_EchoHeader = *dynamic_cast<CDvHeaderPacket *>(packet.get());
				m_EchoHasHeader = true;
			}
			if (packet->IsDvFrame())
			{
				m_EchoBuffer.push_back(packet->Copy());
				m_EchoLastFrame = std::chrono::steady_clock::now();
			}
		}
	}
}

// Maintenance thread hands xml and/or json update,
// and also keeps the transcoder TCP port(s) connected
#define XML_UPDATE_PERIOD 10

void CReflector::MaintenanceThread()
{
	std::string xmlpath, jsonpath;
	if (g_Configure.Contains(g_Keys.files.xml))
		xmlpath.assign(g_Configure.GetString(g_Keys.files.xml));
	if (g_Configure.Contains(g_Keys.files.json))
		jsonpath.assign(g_Configure.GetString(g_Keys.files.json));
	auto tcport = g_Configure.GetUnsigned(g_Keys.tc.port);

	if (xmlpath.empty() && jsonpath.empty())
		return;	// nothing to do

	while (keep_running)
	{
		// report to xml file, written aside and renamed over the old one so the
		// dashboard never reads it empty or half written
		if (! xmlpath.empty())
		{
			const std::string tmppath(xmlpath + ".tmp");
			std::ofstream xmlFile;
			xmlFile.open(tmppath, std::ios::out | std::ios::trunc);
			if ( xmlFile.is_open() )
			{
				// write xml file
				WriteXmlFile(xmlFile);

				// and close file
				xmlFile.close();
				if (0 != std::rename(tmppath.c_str(), xmlpath.c_str()))
					std::cout << "Failed to rename " << tmppath << " to " << xmlpath << std::endl;
			}
			else
			{
				std::cout << "Failed to open " << tmppath  << std::endl;
			}
		}

		// bridges for the WebSocket clients, when they changed
		NotifyMappings();

		// json report
		if (!  jsonpath.empty())
		{
			nlohmann::json jreport;
			JsonReport(jreport);
			const std::string tmppath(jsonpath + ".tmp");
			std::ofstream jsonFile;
			jsonFile.open(tmppath, std::ios::out | std::ios::trunc);
			if (jsonFile.is_open())
			{
				// callsigns come from packets and need not be valid UTF-8, which dump()
				// would throw on, ending urfd
				jsonFile << jreport.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
				jsonFile.close();
				std::rename(tmppath.c_str(), jsonpath.c_str());
			}
		}


		// and wait a bit and do something useful at the same time; a speaker change
		// cuts the wait short, so the dashboard shows it within a tenth of a second
		for (int i=0; i< XML_UPDATE_PERIOD*10 && keep_running && !m_StatusChanged.exchange(false); i++)
		{
			if (tcport)
			{
				g_TCServer.TryAccept(100); // probe dead sockets + non-blocking accept
			}
			else
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// modules & queues

std::shared_ptr<CPacketStream> CReflector::GetStream(char module)
{
	auto it=m_Stream.find(module);
	if (it!=m_Stream.end())
		return it->second;

	return nullptr;
}

bool CReflector::IsStreamOpen(const std::unique_ptr<CDvHeaderPacket> &DvHeader)
{
	for (auto it=m_Stream.begin(); it!=m_Stream.end(); it++)
	{
		if ( (it->second->GetStreamId() == DvHeader->GetStreamId()) && (it->second->IsOpen()) )
			return true;
	}
	return false;
}

char CReflector::GetStreamModule(std::shared_ptr<CPacketStream> stream)
{
	for (auto it=m_Stream.begin(); it!=m_Stream.end(); it++)
	{
		if ( it->second == stream )
			return it->first;
	}
	return ' ';
}

////////////////////////////////////////////////////////////////////////////////////////
// report helpers

void CReflector::JsonReport(nlohmann::json &report)
{
	for (auto &item : g_Configure.GetData().items())
	{
		if (isupper(item.key().at(0)))
			report["Configure"][item.key()] = item.value();
	}

	report["Peers"] = nlohmann::json::array();
	auto peers = GetPeers();
	for (auto pit=peers->cbegin(); pit!=peers->cend(); pit++)
		(*pit)->JsonReport(report);
	ReleasePeers();

	report["Clients"] = nlohmann::json::array();
	auto clients = GetClients();
	for (auto cit=clients->cbegin(); cit!=clients->cend(); cit++)
		(*cit)->JsonReport(report);
	ReleaseClients();

	report["Users"] = nlohmann::json::array();
	auto users = GetUsers();
	for (auto uid=users->begin(); uid!=users->end(); uid++)
		(*uid).JsonReport(report);
	ReleaseUsers();

	// Dynamic TG mappings (not in config)
	report["DynamicTGs"] = nlohmann::json::array();
	for (auto *mmdvm : m_Protocols.MmdvmClients())
	{
		for (const auto &mi : mmdvm->GetTGMap().GetAllMappings())
		{
			if (mi.is_static) continue;
			report["DynamicTGs"].push_back({
				{"protocol", "mmdvm"}, {"client", mmdvm->GetName()}, {"tg", mi.tg}, {"module", std::string(1, mi.module)},
				{"ts", mi.timeslot}, {"remaining", mi.remainingSeconds}
			});
		}
	}
	auto *svxProto = m_Protocols.FindByType(EProtocol::svxreflector);
	if (svxProto)
	{
		auto *svx = static_cast<CSvxReflectorProtocol *>(svxProto);
		for (const auto &mi : svx->GetTGMappings())
		{
			if (mi.is_static) continue;
			report["DynamicTGs"].push_back({
				{"protocol", "svxreflector"}, {"tg", mi.tg}, {"module", std::string(1, mi.module)},
				{"remaining", mi.remainingSeconds}
			});
		}
	}
	auto *svxsProto = m_Protocols.FindByType(EProtocol::svx);
	if (svxsProto)
	{
		auto *svxs = static_cast<CSvxProtocol *>(svxsProto);
		for (const auto &mi : svxs->GetTGMappings())
		{
			if (mi.is_static) continue;
			report["DynamicTGs"].push_back({
				{"protocol", "svx"}, {"tg", mi.tg}, {"module", std::string(1, mi.module)},
				{"remaining", mi.remainingSeconds}
			});
		}
	}
}

#define XML_PROTO_ENABLED(key) (!g_Configure.Contains(key) || g_Configure.GetBoolean(key))

// the same bridges, a dynamic TG's expiry moving by a few seconds aside: it is
// taken anew on every call, while a TG whose timer was refreshed moves by more
static bool SameMappings(const nlohmann::json &a, const nlohmann::json &b)
{
	if (a.size() != b.size())
		return false;
	for (auto it = a.begin(); it != a.end(); ++it)
	{
		if (!b.contains(it.key()) || it.value().size() != b[it.key()].size())
			return false;
		const auto &la = it.value(), &lb = b[it.key()];
		for (size_t i = 0; i < la.size(); i++)
		{
			auto ea = la[i], eb = lb[i];
			const long long xa = ea.value("expires", 0LL), xb = eb.value("expires", 0LL);
			ea.erase("expires");
			eb.erase("expires");
			if (ea != eb || std::llabs(xa - xb) > 5)
				return false;
		}
	}
	return true;
}

// module_mappings to the WebSocket clients when a bridge changed: a TG added,
// removed or refreshed, a connector up or down
void CReflector::NotifyMappings(void)
{
	auto &notifier = CNotifierManager::Instance();
	if (!notifier.IsRunning())
		return;
	nlohmann::json all = nlohmann::json::object();
	for (char m : m_Modules)
		all[std::string(1, m)] = GetModuleMappings(m);
	if (!m_SentMappings.is_null() && SameMappings(m_SentMappings, all))
		return;
	m_SentMappings = all;
	notifier.Dispatch("module_mappings", [&]() -> nlohmann::json { return { { "modules", all } }; });
}

// The bridges of one module: autolinks, TG mappings (static and dynamic, with
// the time a dynamic one expires) and the connectors to other reflectors. For
// the XML file and the WebSocket; only protocols that are enabled.
nlohmann::json CReflector::GetModuleMappings(char m)
{
	const long long now = std::time(nullptr);
	nlohmann::json list = nlohmann::json::array();
	auto add = [&](const char *protocol, const char *type) -> nlohmann::json & {
		list.push_back({ { "protocol", protocol }, { "type", type } });
		return list.back();
	};
	auto addTG = [&](const char *protocol, const CTGModuleMap::STGInfo &mi, bool timeslot) -> nlohmann::json & {
		auto &e = add(protocol, "TG");
		e["id"] = mi.tg;
		if (timeslot)
			e["timeslot"] = (int)mi.timeslot;
		if (!mi.is_static)
			e["expires"] = now + mi.remainingSeconds;
		return e;
	};
	auto enabled = [](const std::string &key) { return g_Configure.Contains(key) && g_Configure.GetBoolean(key); };
	auto isModule = [&](const std::string &key) { return g_Configure.Contains(key) && g_Configure.GetString(key)[0] == m; };

	if (XML_PROTO_ENABLED(g_Keys.ysf.enable) && isModule(g_Keys.ysf.autolinkmod))
	{
		auto &e = add("YSF", "AutoLink");
		if (g_Configure.Contains(g_Keys.ysf.ysfreflectordb.id))
			e["id"] = g_Configure.GetUnsigned(g_Keys.ysf.ysfreflectordb.id);
		if (g_Configure.Contains(g_Keys.ysf.ysfreflectordb.name))
			e["remote_name"] = g_Configure.GetString(g_Keys.ysf.ysfreflectordb.name);
	}
	if (XML_PROTO_ENABLED(g_Keys.nxdn.enable) && isModule(g_Keys.nxdn.autolinkmod))
	{
		auto &e = add("NXDN", "AutoLink");
		if (g_Configure.Contains(g_Keys.nxdn.reflectorid))
			e["id"] = g_Configure.GetUnsigned(g_Keys.nxdn.reflectorid);
	}
	if (XML_PROTO_ENABLED(g_Keys.p25.enable) && isModule(g_Keys.p25.autolinkmod))
	{
		auto &e = add("P25", "AutoLink");
		if (g_Configure.Contains(g_Keys.p25.reflectorid))
			e["id"] = g_Configure.GetUnsigned(g_Keys.p25.reflectorid);
	}
	if (enabled(g_Keys.usrp.enable) && isModule(g_Keys.usrp.module))
	{
		auto &e = add("USRP", "Bridge");
		if (g_Configure.Contains(g_Keys.usrp.callsign))
			e["remote_name"] = g_Configure.GetString(g_Keys.usrp.callsign);
	}
	if (enabled(g_Keys.mmdvm.enable))
	{
		if (auto *proto = m_Protocols.FindByType(EProtocol::dmrmmdvm))
			for (const auto &mi : static_cast<CDmrmmdvmProtocol *>(proto)->GetTGMap().GetAllMappings())
				if (mi.module == m)
					addTG("MMDVM", mi, true);
	}
	// every MMDVMClient connection under its own name, MMDVMClient2 ...
	for (auto *cli : m_Protocols.MmdvmClients())
	{
		for (const auto &mi : cli->GetTGMap().GetAllMappings())
			if (mi.module == m)
				addTG(cli->GetName().c_str(), mi, true)["network"] = cli->GetNetwork();
	}
	if (enabled(g_Keys.svx.enable))
	{
		if (auto *proto = m_Protocols.FindByType(EProtocol::svxreflector))
			for (const auto &mi : static_cast<CSvxReflectorProtocol *>(proto)->GetTGMappings())
				if (mi.module == m)
				{
					auto &e = addTG("SvxReflector", mi, false);
					if (g_Configure.Contains(g_Keys.svx.host))
						e["remote_name"] = g_Configure.GetString(g_Keys.svx.host);
				}
	}
	if (enabled(g_Keys.svxs.enable))
	{
		if (auto *proto = m_Protocols.FindByType(EProtocol::svx))
			for (const auto &mi : static_cast<CSvxProtocol *>(proto)->GetTGMappings())
				if (mi.module == m)
					addTG("SVX", mi, false);
	}
	// connectors to other reflectors: the remote module (or DG-ID) and whether it is up
	auto addConnector = [&](const char *protocol, const char *type, const std::string &host, uint16_t port, bool connected) -> nlohmann::json & {
		auto &e = add(protocol, type);
		e["remote_name"] = host + ":" + std::to_string(port);
		e["connected"] = connected;
		return e;
	};
	if (enabled(g_Keys.dcsclient.enable))
	{
		if (auto *proto = m_Protocols.FindByType(EProtocol::dcsclient))
			for (const auto &mi : static_cast<CDcsClientProtocol *>(proto)->GetMappings())
				if (mi.localModule == m)
					addConnector("DCSClient", "Module", mi.host, mi.port, mi.connected)["id"] = std::string(1, mi.remoteModule);
	}
	if (enabled(g_Keys.dextraclient.enable))
	{
		if (auto *proto = m_Protocols.FindByType(EProtocol::dextraclient))
			for (const auto &mi : static_cast<CDExtraClientProtocol *>(proto)->GetMappings())
				if (mi.localModule == m)
					addConnector("DExtraClient", "Module", mi.host, mi.port, mi.connected)["id"] = std::string(1, mi.remoteModule);
	}
	if (enabled(g_Keys.dplusclient.enable))
	{
		if (auto *proto = m_Protocols.FindByType(EProtocol::dplusclient))
			for (const auto &mi : static_cast<CDPlusClientProtocol *>(proto)->GetMappings())
				if (mi.localModule == m)
					addConnector("DPlusClient", "Module", mi.host, mi.port, mi.connected)["id"] = std::string(1, mi.remoteModule);
	}
	if (enabled(g_Keys.ysfclient.enable))
	{
		if (auto *proto = m_Protocols.FindByType(EProtocol::ysfclient))
			for (const auto &mi : static_cast<CYsfClientProtocol *>(proto)->GetMappings())
				if (mi.localModule == m)
				{
					auto &e = addConnector("YSFClient", "DG-ID", mi.host, mi.port, mi.connected);
					if (mi.dgid > 0)
						e["id"] = (unsigned)mi.dgid;
				}
	}
	return list;
}

void CReflector::WriteXmlFile(std::ofstream &xmlFile)
{
	// write header
	xmlFile << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" << std::endl;

	// software version
	xmlFile << "<Version>" << g_Version << "</Version>" << std::endl;

	// reflector metadata
	xmlFile << "<Reflector>" << std::endl;
	xmlFile << "\t<Callsign>" << CUtils::XmlEscape(g_Configure.GetString(g_Keys.names.callsign)) << "</Callsign>" << std::endl;
	if (g_Configure.Contains(g_Keys.names.country))
		xmlFile << "\t<Country>" << CUtils::XmlEscape(g_Configure.GetString(g_Keys.names.country)) << "</Country>" << std::endl;
	if (g_Configure.Contains(g_Keys.names.sponsor))
		xmlFile << "\t<Sponsor>" << CUtils::XmlEscape(g_Configure.GetString(g_Keys.names.sponsor)) << "</Sponsor>" << std::endl;
	if (g_Configure.Contains(g_Keys.names.url))
		xmlFile << "\t<DashboardURL>" << CUtils::XmlEscape(g_Configure.GetString(g_Keys.names.url)) << "</DashboardURL>" << std::endl;
	if (g_Configure.Contains(g_Keys.names.email))
		xmlFile << "\t<Email>" << CUtils::XmlEscape(g_Configure.GetString(g_Keys.names.email)) << "</Email>" << std::endl;
	xmlFile << "</Reflector>" << std::endl;

	// configured modules
	xmlFile << "<Modules>" << std::endl;
	auto modules = g_Configure.GetString(g_Keys.modules.modules);
	for (char m : modules)
	{
		if (m < 'A' || m > 'Z') continue;
		xmlFile << "<Module>" << std::endl;
		xmlFile << "\t<Name>" << m << "</Name>" << std::endl;
		auto descKey = g_Keys.modules.descriptor[m - 'A'];
		if (g_Configure.Contains(descKey))
			xmlFile << "\t<Description>" << CUtils::XmlEscape(g_Configure.GetString(descKey)) << "</Description>" << std::endl;
		// auto-set Echo description
		if (g_Configure.Contains(g_Keys.echo.module) && g_Configure.GetString(g_Keys.echo.module)[0] == m)
			xmlFile << "\t<Echo>true</Echo>" << std::endl;
		// count connected nodes on this module
		int nodeCount = 0;
		CClients *modClients = GetClients();
		for (auto cit = modClients->cbegin(); cit != modClients->cend(); cit++)
		{
			if ((*cit)->IsNode() && !(*cit)->IsLink() && (*cit)->IsLinkedTo(m))
				nodeCount++;
		}
		ReleaseClients();
		xmlFile << "\t<LinkedNodes>" << nodeCount << "</LinkedNodes>" << std::endl;
		// who is transmitting on this module right now, for the dashboard
		auto stream = GetStream(m);
		if (stream && stream->IsOpen())
			xmlFile << "\t<OnAir>" << CUtils::XmlEscape(stream->GetUserCallsign().GetCS()) << "</OnAir>" << std::endl;
		// check if transcoded
		auto tcmods = g_Configure.GetString(g_Keys.tc.modules);
		bool transcoded = (tcmods.find(m) != std::string::npos);
		xmlFile << "\t<Transcoded>" << (transcoded ? "true" : "false") << "</Transcoded>" << std::endl;
		// standard protocol IDs for this module
		int modIdx = m - 'A';
		if (XML_PROTO_ENABLED(g_Keys.dmrplus.enable))
			xmlFile << "\t<DMRplus>" << (4001 + modIdx) << "</DMRplus>" << std::endl;
		if (XML_PROTO_ENABLED(g_Keys.ysf.enable))
			xmlFile << "\t<YSFDGID>" << (10 + modIdx) << "</YSFDGID>" << std::endl;
		// NXDN RAN and destination ID for this module
		if (XML_PROTO_ENABLED(g_Keys.nxdn.enable))
		{
			uint8_t ran = CNXDNProtocol::ModuleToRAN(m);
			if (ran != 0)
				xmlFile << "\t<NXDNRAN>" << (int)ran << "</NXDNRAN>" << std::endl;
			uint16_t dstid = CNXDNProtocol::ModuleToDstId(m);
			if (dstid != 0)
				xmlFile << "\t<NXDNDstId>" << dstid << "</NXDNDstId>" << std::endl;
		}
		// the bridges of this module, shared with the WebSocket
		for (const auto &mp : GetModuleMappings(m))
		{
			xmlFile << "\t<Mapping><Protocol>" << mp["protocol"].get<std::string>() << "</Protocol><Type>" << mp["type"].get<std::string>() << "</Type>";
			if (mp.contains("id"))
				xmlFile << "<ID>" << (mp["id"].is_string() ? mp["id"].get<std::string>() : std::to_string(mp["id"].get<unsigned>())) << "</ID>";
			if (mp.contains("timeslot"))
				xmlFile << "<Timeslot>TS" << mp["timeslot"].get<int>() << "</Timeslot>";
			if (mp.contains("remote_name"))
				xmlFile << "<RemoteName>" << CUtils::XmlEscape(mp["remote_name"].get<std::string>()) << "</RemoteName>";
			if (mp.contains("expires"))
				xmlFile << "<Dynamic>true</Dynamic><Remaining>" << std::max<long long>(0, mp["expires"].get<long long>() - (long long)std::time(nullptr)) << "</Remaining>";
			if (mp.value("connected", false))
				xmlFile << "<Connected>true</Connected>";
			xmlFile << "</Mapping>" << std::endl;
		}
		xmlFile << "</Module>" << std::endl;
	}
	xmlFile << "</Modules>" << std::endl;

	// enabled protocols (only show if enabled; default=true for backwards compat)
	xmlFile << "<Protocols>" << std::endl;
	if (XML_PROTO_ENABLED(g_Keys.dextra.enable))
		xmlFile << "<Protocol><Name>DExtra</Name><Port>" << g_Configure.GetUnsigned(g_Keys.dextra.port) << "</Port></Protocol>" << std::endl;
	if (XML_PROTO_ENABLED(g_Keys.dplus.enable))
		xmlFile << "<Protocol><Name>DPlus</Name><Port>" << g_Configure.GetUnsigned(g_Keys.dplus.port) << "</Port></Protocol>" << std::endl;
	if (XML_PROTO_ENABLED(g_Keys.dcs.enable))
		xmlFile << "<Protocol><Name>DCS</Name><Port>" << g_Configure.GetUnsigned(g_Keys.dcs.port) << "</Port></Protocol>" << std::endl;
	if (XML_PROTO_ENABLED(g_Keys.mmdvm.enable))
		xmlFile << "<Protocol><Name>MMDVM</Name><Port>" << g_Configure.GetUnsigned(g_Keys.mmdvm.port) << "</Port></Protocol>" << std::endl;
	if (XML_PROTO_ENABLED(g_Keys.dmrplus.enable))
		xmlFile << "<Protocol><Name>DMRPlus</Name><Port>" << g_Configure.GetUnsigned(g_Keys.dmrplus.port) << "</Port></Protocol>" << std::endl;
	if (XML_PROTO_ENABLED(g_Keys.m17.enable))
		xmlFile << "<Protocol><Name>M17</Name><Port>" << g_Configure.GetUnsigned(g_Keys.m17.port) << "</Port></Protocol>" << std::endl;
	if (XML_PROTO_ENABLED(g_Keys.ysf.enable))
		xmlFile << "<Protocol><Name>YSF</Name><Port>" << g_Configure.GetUnsigned(g_Keys.ysf.port) << "</Port></Protocol>" << std::endl;
	if (XML_PROTO_ENABLED(g_Keys.p25.enable))
		xmlFile << "<Protocol><Name>P25</Name><Port>" << g_Configure.GetUnsigned(g_Keys.p25.port) << "</Port></Protocol>" << std::endl;
	if (XML_PROTO_ENABLED(g_Keys.nxdn.enable))
		xmlFile << "<Protocol><Name>NXDN</Name><Port>" << g_Configure.GetUnsigned(g_Keys.nxdn.port) << "</Port></Protocol>" << std::endl;
	if (XML_PROTO_ENABLED(g_Keys.urf.enable))
		xmlFile << "<Protocol><Name>URF</Name><Port>" << g_Configure.GetUnsigned(g_Keys.urf.port) << "</Port></Protocol>" << std::endl;
	if (g_Configure.Contains(g_Keys.xlxpeer.enable) && g_Configure.GetBoolean(g_Keys.xlxpeer.enable))
		xmlFile << "<Protocol><Name>XLXPeer</Name><Port>" << g_Configure.GetUnsigned(g_Keys.xlxpeer.port) << "</Port></Protocol>" << std::endl;
	if (g_Configure.Contains(g_Keys.usrp.enable) && g_Configure.GetBoolean(g_Keys.usrp.enable))
		xmlFile << "<Protocol><Name>USRP</Name><Port>" << g_Configure.GetUnsigned(g_Keys.usrp.rxport) << "</Port></Protocol>" << std::endl;
	if (g_Configure.Contains(g_Keys.svx.enable) && g_Configure.GetBoolean(g_Keys.svx.enable))
		xmlFile << "<Protocol><Name>SvxReflector</Name><Port>" << g_Configure.GetUnsigned(g_Keys.svx.port) << "</Port></Protocol>" << std::endl;
	if (g_Configure.Contains(g_Keys.svxs.enable) && g_Configure.GetBoolean(g_Keys.svxs.enable))
		xmlFile << "<Protocol><Name>SVX</Name><Port>" << g_Configure.GetUnsigned(g_Keys.svxs.port) << "</Port></Protocol>" << std::endl;
	for (size_t i = 0; i < g_Plugins.Size(); i++)
		if (g_Plugins.Get(i).IsEnabled())
			xmlFile << "<Protocol><Name>" << CUtils::XmlEscape(g_Plugins.Get(i).GetName()) << "</Name><Port>" << g_Plugins.Get(i).GetPort() << "</Port><Description>" << CUtils::XmlEscape(g_Plugins.Get(i).GetDescription()) << "</Description></Protocol>" << std::endl;
	if (g_Configure.Contains(g_Keys.g3.enable) && g_Configure.GetBoolean(g_Keys.g3.enable))
		xmlFile << "<Protocol><Name>G3</Name><Port>40000</Port></Protocol>" << std::endl;
	for (unsigned n = 1; n <= MMDVMCLI_MAX_INSTANCES; n++)
	{
		const auto ck = MmdvmClientKeys(n);
		if (g_Configure.Contains(ck.enable) && g_Configure.GetBoolean(ck.enable))
			xmlFile << "<Protocol><Name>" << MmdvmClientName(n) << "</Name><Port>" << (g_Configure.Contains(ck.localport) ? g_Configure.GetUnsigned(ck.localport) : 0) << "</Port></Protocol>" << std::endl;
	}
	if (g_Configure.Contains(g_Keys.dcsclient.enable) && g_Configure.GetBoolean(g_Keys.dcsclient.enable))
		xmlFile << "<Protocol><Name>DCSClient</Name><Port>0</Port></Protocol>" << std::endl;
	if (g_Configure.Contains(g_Keys.dextraclient.enable) && g_Configure.GetBoolean(g_Keys.dextraclient.enable))
		xmlFile << "<Protocol><Name>DExtraClient</Name><Port>0</Port></Protocol>" << std::endl;
	if (g_Configure.Contains(g_Keys.dplusclient.enable) && g_Configure.GetBoolean(g_Keys.dplusclient.enable))
		xmlFile << "<Protocol><Name>DPlusClient</Name><Port>0</Port></Protocol>" << std::endl;
	if (g_Configure.Contains(g_Keys.ysfclient.enable) && g_Configure.GetBoolean(g_Keys.ysfclient.enable))
		xmlFile << "<Protocol><Name>YSFClient</Name><Port>0</Port></Protocol>" << std::endl;
	xmlFile << "</Protocols>" << std::endl;

	CCallsign cs = m_Callsign;
	cs.PatchCallsign(0, "XLX", 3);

	// linked peers
	xmlFile << "<" << (const char *)cs << "linked peers>" << std::endl;
	// lock
	CPeers *peers = GetPeers();
	// iterate on peers
	for ( auto pit=peers->cbegin(); pit!=peers->cend(); pit++ )
	{
		(*pit)->WriteXml(xmlFile);
	}
	// unlock
	ReleasePeers();
	xmlFile << "</" << (const char *)cs << "linked peers>" << std::endl;

	// linked nodes
	xmlFile << "<" << (const char *)cs << "linked nodes>" << std::endl;
	// lock
	CClients *clients = GetClients();
	// iterate on clients
	for ( auto cit=clients->cbegin(); cit!=clients->cend(); cit++ )
	{
		if ( (*cit)->IsNode() && !(*cit)->IsLink() )
		{
			(*cit)->WriteXml(xmlFile);
		}
	}
	// unlock
	ReleaseClients();
	xmlFile << "</" << (const char *)cs << "linked nodes>" << std::endl;

	// last heard users
	xmlFile << "<" << (const char *)cs << "heard users>" << std::endl;
	// a copy, so the name lookups never hold up the users lock while the ID database reloads
	CUsers *users = GetUsers();
	std::list<CUser> heard(users->begin(), users->end());
	ReleaseUsers();
	for ( auto &user : heard )
	{
		user.WriteXml(xmlFile);
	}
	xmlFile << "</" << (const char *)cs << "heard users>" << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////
// echo thread

void CReflector::EchoThread(void)
{
	while (keep_running)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		std::vector<std::unique_ptr<CPacket>> playback;
		CDvHeaderPacket header;
		bool doPlayback = false;

		{
			std::lock_guard<std::mutex> lock(m_EchoMutex);
			if (m_EchoHasHeader && !m_EchoBuffer.empty())
			{
				auto elapsed = std::chrono::steady_clock::now() - m_EchoLastFrame;
				if (elapsed > std::chrono::seconds(3))
				{
					header = m_EchoHeader;
					playback = std::move(m_EchoBuffer);
					m_EchoBuffer.clear();
					m_EchoHasHeader = false;
					doPlayback = true;
				}
			}
		}

		if (doPlayback && !playback.empty())
		{
			std::cout << "Echo: playing back " << playback.size() << " frames on module " << m_EchoModule << std::endl;

			// get the stream for the echo module
			auto streamIt = m_Stream.find(m_EchoModule);
			if (streamIt == m_Stream.end())
				continue;
			auto stream = streamIt->second;

			// wait for any current stream to finish
			while (stream->IsOpen() && keep_running)
				std::this_thread::sleep_for(std::chrono::milliseconds(50));

			if (!keep_running) break;

			// generate a new stream ID
			static uint16_t echoStreamId = 0xEC00;
			echoStreamId++;
			if (echoStreamId == 0) echoStreamId = 0xEC00;

			// create echo header - swap MY callsign to show "ECHO"
			CCallsign echoCs;
			echoCs.SetCallsign("ECHO    ", false);
			CCallsign rpt1 = header.GetRpt1Callsign();
			CCallsign rpt2 = header.GetRpt2Callsign();
			auto echoHeader = std::unique_ptr<CDvHeaderPacket>(
				new CDvHeaderPacket(echoCs, header.GetMyCallsign(), rpt1, rpt2, echoStreamId, (uint8_t)0)
			);
			echoHeader->SetPacketModule(m_EchoModule);

			// push header to stream
			stream->Push(std::move(echoHeader));

			// push voice frames with 20ms spacing
			for (size_t i = 0; i < playback.size() && keep_running; i++)
			{
				playback[i]->SetStreamId(echoStreamId);
				playback[i]->SetPacketModule(m_EchoModule);
				if (i == playback.size() - 1)
					playback[i]->SetLastPacket(true);
				stream->Push(std::move(playback[i]));
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}

			std::cout << "Echo: playback complete" << std::endl;
		}
	}

	std::cout << "Echo thread stopped" << std::endl;
}

#ifndef NO_DHT
// DHT put() and get()
void CReflector::PutDHTConfig()
{
	const std::string cs(g_Configure.GetString(g_Keys.names.callsign));
	SUrfdConfig1 cfg;
	time(&cfg.timestamp);
	cfg.callsign.assign(cs);
	cfg.ipv4addr.assign(g_Configure.GetString(g_Keys.ip.ipv4address));
	cfg.ipv6addr.assign(g_Configure.GetString(g_Keys.ip.ipv6address));
	cfg.modules.assign(g_Configure.GetString(g_Keys.modules.modules));
	if (g_Configure.GetUnsigned(g_Keys.tc.port))
		cfg.transcodedmods.assign(g_Configure.GetString(g_Keys.tc.modules));
	cfg.url.assign(g_Configure.GetString(g_Keys.names.url));
	cfg.email.assign(g_Configure.GetString(g_Keys.names.email));
	cfg.country.assign(g_Configure.GetString(g_Keys.names.country));
	cfg.sponsor.assign(g_Configure.GetString(g_Keys.names.sponsor));
	std::ostringstream ss;
	ss << g_Version;
	cfg.version.assign(ss.str());
	cfg.almod[toUType(EUrfdAlMod::nxdn)]   = g_Configure.GetAutolinkModule(g_Keys.nxdn.autolinkmod);
	cfg.almod[toUType(EUrfdAlMod::p25)]    = g_Configure.GetAutolinkModule(g_Keys.p25.autolinkmod);
	cfg.almod[toUType(EUrfdAlMod::ysf)]    = g_Configure.GetAutolinkModule(g_Keys.ysf.autolinkmod);
	cfg.ysffreq[toUType(EUrfdTxRx::rx)]    = g_Configure.GetUnsigned(g_Keys.ysf.defaultrxfreq);
	cfg.ysffreq[toUType(EUrfdTxRx::tx)]    = g_Configure.GetUnsigned(g_Keys.ysf.defaulttxfreq);
	cfg.refid[toUType(EUrfdRefId::nxdn)]   = g_Configure.GetUnsigned(g_Keys.nxdn.reflectorid);
	cfg.refid[toUType(EUrfdRefId::p25)]    = g_Configure.GetUnsigned(g_Keys.p25.reflectorid);
	cfg.port[toUType(EUrfdPorts::dcs)]     = (uint16_t)g_Configure.GetUnsigned(g_Keys.dcs.port);
	cfg.port[toUType(EUrfdPorts::dextra)]  = (uint16_t)g_Configure.GetUnsigned(g_Keys.dextra.port);
	cfg.port[toUType(EUrfdPorts::dmrplus)] = (uint16_t)g_Configure.GetUnsigned(g_Keys.dmrplus.port);
	cfg.port[toUType(EUrfdPorts::dplus)]   = (uint16_t)g_Configure.GetUnsigned(g_Keys.dplus.port);
	cfg.port[toUType(EUrfdPorts::m17)]     = (uint16_t)g_Configure.GetUnsigned(g_Keys.m17.port);
	// [MMDVM] is commented out in the template, so the port may be missing
	cfg.port[toUType(EUrfdPorts::mmdvm)]   = g_Configure.Contains(g_Keys.mmdvm.port) ? (uint16_t)g_Configure.GetUnsigned(g_Keys.mmdvm.port) : 0;
	cfg.port[toUType(EUrfdPorts::nxdn)]    = (uint16_t)g_Configure.GetUnsigned(g_Keys.nxdn.port);
	cfg.port[toUType(EUrfdPorts::p25)]     = (uint16_t)g_Configure.GetUnsigned(g_Keys.p25.port);
	cfg.port[toUType(EUrfdPorts::urf)]     = (uint16_t)g_Configure.GetUnsigned(g_Keys.urf.port);
	cfg.port[toUType(EUrfdPorts::ysf)]     = (uint16_t)g_Configure.GetUnsigned(g_Keys.ysf.port);
	cfg.g3enabled = g_Configure.GetBoolean(g_Keys.g3.enable);
	for (const auto m : cfg.modules)
		cfg.description[m] = g_Configure.GetString(g_Keys.modules.descriptor[m-'A']);

	auto nv = std::make_shared<dht::Value>(cfg);
	nv->user_type.assign(URFD_CONFIG_1);
	nv->id = toUType(EUrfdValueID::Config);

	node.putSigned(
		refhash,
		nv,
#ifdef DEBUG
		[](bool success){ std::cout << "PutDHTConfig() " << (success ? "successful" : "unsuccessful") << std::endl; },
#else
		[](bool success){ if(! success) std::cout << "PutDHTConfig() unsuccessful" << std::endl; },
#endif
		true
	);

	// mrefd looks an interlinked reflector up by its M17-XXX designator
	if (g_Configure.Contains(g_Keys.m17.callsign))
	{
		const std::string m17cs(g_Configure.GetString(g_Keys.m17.callsign));
		SMrefdConfig1 m17cfg;
		m17cfg.timestamp = cfg.timestamp;
		m17cfg.callsign.assign(m17cs);
		m17cfg.ipv4addr.assign(cfg.ipv4addr);
		m17cfg.ipv6addr.assign(cfg.ipv6addr);
		m17cfg.modules.assign(cfg.modules);
		m17cfg.url.assign(cfg.url);
		m17cfg.email.assign(cfg.email);
		m17cfg.sponsor.assign(cfg.sponsor);
		m17cfg.country.assign(cfg.country);
		m17cfg.version.assign(cfg.version);
		m17cfg.port = cfg.port[toUType(EUrfdPorts::m17)];

		auto mv = std::make_shared<dht::Value>(m17cfg);
		mv->user_type.assign(MREFD_CONFIG_1);
		mv->id = toUType(EMrefdValueID::Config);

		m17hash = dht::InfoHash::get(m17cs);
		node.putSigned(
			m17hash,
			mv,
#ifdef DEBUG
			[](bool success){ std::cout << "PutDHTConfig() M17 " << (success ? "successful" : "unsuccessful") << std::endl; },
#else
			[](bool success){ if(! success) std::cout << "PutDHTConfig() M17 unsuccessful" << std::endl; },
#endif
			true
		);
	}
}

// an mrefd publishes its M17-XXX document with the mrefd-config-1 layout
void CReflector::GetDHTMrefdConfig(const std::string &cs)
{
	static SMrefdConfig1 cfg;
	cfg.timestamp = 0;	// every time this is called, zero the timestamp

	std::cout << "Getting " << cs << " connection info..." << std::endl;

	dht::Where w;
	w.id(toUType(EMrefdValueID::Config));

	node.get(
		dht::InfoHash::get(cs),
		[](const std::shared_ptr<dht::Value> &v) {
			if (0 == v->user_type.compare(MREFD_CONFIG_1))
			{
				auto rdat = dht::Value::unpack<SMrefdConfig1>(*v);
				if (rdat.timestamp > cfg.timestamp)
					cfg = rdat;
			}
			else
			{
				std::cerr << "Get() returned unknown user_type: '" << v->user_type << "'" << std::endl;
			}
			return true;	// check all the values returned
		},
		[](bool success) {
			if (success)
			{
				if (cfg.timestamp)
				{
					auto ilmap = g_GateKeeper.GetInterlinkMap();
					ilmap->Update(cfg.callsign, cfg.modules, cfg.ipv4addr, cfg.ipv6addr, cfg.port, "");
					// like mrefd, a version 0.x is a legacy mrefd
					auto item = ilmap->FindMapItem(cfg.callsign);
					if (item)
						item->SetProtocol((cfg.version.size() && '0' == cfg.version[0]) ? "M17L" : "M17");
					g_GateKeeper.ReleaseInterlinkMap();
				}
				else
				{
					std::cerr << "node.Get() was successful, but the timestamp was zero" << std::endl;
				}
			}
			else
			{
				std::cout << "Get() was unsuccessful" << std::endl;
			}
		},
		{}, // empty filter
		w	// just the configuration section
	);
}

void CReflector::GetDHTConfig(const std::string &cs)
{
	if (0 == cs.compare(0, 4, "M17-"))
	{
		GetDHTMrefdConfig(cs);
		return;
	}

	static SUrfdConfig1 cfg;
	cfg.timestamp = 0;	// every time this is called, zero the timestamp

	std::cout << "Getting " << cs << " connection info..." << std::endl;

	// we only want the configuration section of the reflector's document
	dht::Where w;
	w.id(toUType(EUrfdValueID::Config));

	node.get(
		dht::InfoHash::get(cs),
		[](const std::shared_ptr<dht::Value> &v) {
			if (0 == v->user_type.compare(URFD_CONFIG_1))
			{
				auto rdat = dht::Value::unpack<SUrfdConfig1>(*v);
				if (rdat.timestamp > cfg.timestamp)
				{
					// the time stamp is the newest so far, so put it in the static cfg struct
					cfg = dht::Value::unpack<SUrfdConfig1>(*v);
				}
			}
			else
			{
				std::cerr << "Get() returned unknown user_type: '" << v->user_type << "'" << std::endl;
			}
			return true;	// check all the values returned
		},
		[](bool success) {
			if (success)
			{
				if (cfg.timestamp)
				{
					// if the get() call was successful and there is a nonzero timestamp, then do the update
					g_GateKeeper.GetInterlinkMap()->Update(cfg.callsign, cfg.modules, cfg.ipv4addr, cfg.ipv6addr, cfg.port[toUType(EUrfdPorts::urf)], cfg.transcodedmods);
					g_GateKeeper.ReleaseInterlinkMap();
				}
				else
				{
					std::cerr << "node.Get() was successful, but the timestamp was zero" << std::endl;
				}
			}
			else
			{
				std::cout << "Get() was unsuccessful" << std::endl;
			}
		},
		{}, // empty filter
		w	// just the configuration section
	);
}

#endif
