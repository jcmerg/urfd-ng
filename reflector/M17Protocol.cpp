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
#include "M17Client.h"
#include "M17Peer.h"
#include "M17Protocol.h"
#include "M17Packet.h"
#include "Global.h"

////////////////////////////////////////////////////////////////////////////////////////
// operation

bool CM17Protocol::Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6)
{
	// base class
	if (! CProtocol::Initialize(type, ptype, port, has_ipv4, has_ipv6))
		return false;

	// update time
	m_LastKeepaliveTime.start();
	m_LastPeersLinkTime.start();

	// the M17 designator for interlinks with mrefd reflectors, none disables them
	if ( g_Configure.Contains(g_Keys.m17.callsign) )
	{
		m_InterlinkCallsign.SetCallsign(g_Configure.GetString(g_Keys.m17.callsign), false);
		m_bInterlink = true;
		std::cout << "M17 interlinks as " << m_InterlinkCallsign << std::endl;
	}

	// done
	return true;
}



////////////////////////////////////////////////////////////////////////////////////////
// task

void CM17Protocol::Task(void)
{
	CBuffer   Buffer;
	CIp       Ip;
	CCallsign Callsign;
	char      ToLinkModule;
	char      Modules[27];
	std::unique_ptr<CDvHeaderPacket> Header;
	std::unique_ptr<CDvFramePacket>  Frame;

	// handle incoming packets
#if M17_IPV6==true
#if M17_IPV4==true
	if ( ReceiveDS(Buffer, Ip, 20) )
#else
	if ( Receive6(Buffer, Ip, 20) )
#endif
#else
	if ( Receive4(Buffer, Ip, 20) )
#endif
	{
		// data from an interlinked mrefd has one more byte, telling the module
		const char PeerModule = StripPeerModule(Buffer, Ip);

		// crack the packet
		if ( PEER_DROP == PeerModule )
		{
			// from an interlinked mrefd, but for no module it shares
		}
		else if ( IsValidDvPacket(Buffer, Ip, PeerModule, Header, Frame) )
		{
			// listen-only clients may not transmit
			if ( (' ' == PeerModule) && IsListenOnlyClient(Ip) )
			{
				if ( Frame->IsLastPacket() )
					std::cout << "M17 listen-only client at " << Ip << " transmitted as " << Header->GetMyCallsign() << ", ignored" << std::endl;
			}
			// callsign muted?
			else if ( g_GateKeeper.MayTransmit(Header->GetMyCallsign(), Ip, EProtocol::m17, Header->GetRpt2Module()) )
			{
				// GNSS position, checked once a second (25 frames); look before
				// OnDvHeaderPacketIn, an opened stream takes over the header
				const bool gnss = (EM17Meta::gnss == Header->GetM17Meta()) && (0 == (0x7FFFU & Frame->M17FrameNumber()) % 25);
				const CCallsign my(Header->GetMyCallsign());

				OnDvHeaderPacketIn(Header, Ip, PeerModule);

				if ( gnss )
					OnGnssIn(my, Frame->GetNonce());

				// xrf needs a voice frame every 20 ms and an M17 frame is 40 ms, so we need a duplicate
				auto secondFrame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(*Frame.get()));

				// This is not a second packet, so clear the last packet status, since the real last packet it the secondFrame
				if (Frame->IsLastPacket())
					Frame->SetLastPacket(false);

				// push the "first" packet
				OnDvFramePacketIn(Frame, &Ip);
				// push the "second" packet
				OnDvFramePacketIn(secondFrame, &Ip); // push two packet because we need a packet every 20 ms
			}
		}
		else if ( IsValidConnectPacket(Buffer, Callsign, ToLinkModule) || IsValidListenPacket(Buffer, Callsign, ToLinkModule) )
		{
			bool isListen = (0 == Buffer.Compare((uint8_t *)"LSTN", 4));
			std::cout << "M17 " << (isListen ? "listen " : "") << "connect packet for module " << ToLinkModule << " from " << Callsign << " at " << Ip << std::endl;

			// callsign authorized?
			if ( g_GateKeeper.MayLink(Callsign, Ip, EProtocol::m17) && g_Reflector.IsValidModule(ToLinkModule) )
			{
				// valid module ?
				if ( g_Reflector.IsValidModule(ToLinkModule) )
				{
					// acknowledge the request
					Send("ACKN", Ip);

					// a new link replaces any earlier one from this node, e.g. on another module
					CClients *clients = g_Reflector.GetClients();
					std::vector<std::shared_ptr<CClient>> old;
					auto it = clients->begin();
					std::shared_ptr<CClient>client = nullptr;
					while ( (client = clients->FindNextClient(Callsign, Ip, EProtocol::m17, it)) != nullptr )
						old.push_back(client);
					for ( auto &c : old )
						clients->RemoveClient(c);

					// create the client and append
					clients->AddClient(std::make_shared<CM17Client>(Callsign, Ip, ToLinkModule, isListen ? EM17ClientType::listenonly : EM17ClientType::simple));
					g_Reflector.ReleaseClients();
				}
				else
				{
					std::cout << "M17 node " << Callsign << " connect attempt on non-existing module" << std::endl;

					// deny the request
					Send("NACK", Ip);
				}
			}
			else
			{
				// deny the request
				Send("NACK", Ip);
			}

		}
		else if ( IsValidInterlinkPacket(Buffer, "CONN", Callsign, Modules) )
		{
			OnInterlinkConnectIn(Callsign, Modules, Ip);
		}
		else if ( IsValidInterlinkPacket(Buffer, "ACKN", Callsign, Modules) )
		{
			OnInterlinkAckIn(Callsign, Modules, Ip);
		}
		else if ( (10 == Buffer.size()) && (0 == Buffer.Compare((uint8_t *)"NACK", 4)) )
		{
			// an mrefd refusing our interlink
			Callsign.CodeIn(Buffer.data() + 4);
			std::cout << "M17 NACK packet from " << Callsign << " at " << Ip << std::endl;
		}
		else if ( IsValidDisconnectPacket(Buffer, Callsign) )
		{
			if ( IsPeer(Ip) )
			{
				// an interlinked mrefd unlinks
				std::cout << "M17 disconnect packet from peer " << Callsign << " at " << Ip << std::endl;
				RemovePeer(Ip);
			}
			else
			{
				std::cout << "M17 disconnect packet from " << Callsign << " at " << Ip << std::endl;

				// find client
				CClients *clients = g_Reflector.GetClients();
				std::shared_ptr<CClient>client = clients->FindClient(Ip, EProtocol::m17);
				if ( client != nullptr )
				{
					// remove it
					clients->RemoveClient(client);
					// and acknowledge the disconnect
					Send("DISC", Ip);
				}
				g_Reflector.ReleaseClients();
			}
		}
		else if ( IsValidKeepAlivePacket(Buffer, Callsign) )
		{
			// find all clients with that callsign & ip and keep them alive
			CClients *clients = g_Reflector.GetClients();
			auto it = clients->begin();
			std::shared_ptr<CClient>client = nullptr;
			while ( (client = clients->FindNextClient(Callsign, Ip, EProtocol::m17, it)) != nullptr )
			{
				client->Alive();
			}
			g_Reflector.ReleaseClients();

			// and an interlinked reflector as a whole
			CPeers *peers = g_Reflector.GetPeers();
			std::shared_ptr<CPeer>peer = peers->FindPeer(Ip, EProtocol::m17);
			if ( peer != nullptr )
				peer->Alive();
			g_Reflector.ReleasePeers();
		}
		else if ( IsValidPacketModePacket(Buffer) )
		{
			OnPacketModePacketIn(Buffer, Ip, PeerModule);
		}
		else if ( (sizeof(SM17Frame) == Buffer.size()) && (0 == Buffer.Compare((uint8_t *)"M17 ", 4)) )
		{
			// a stream frame not taken: not linked, encrypted or no callsign as source
		}
		else
		{
			// invalid packet
			std::string title("Unknown M17 packet from ");
			title += Ip.GetAddress();
			Buffer.Dump(title);
		}
	}

	// handle end of streaming timeout
	CheckStreamsTimeout();

	// handle queue from reflector
	HandleQueue();

	// keep client alive
	if ( m_LastKeepaliveTime.time() > M17_KEEPALIVE_PERIOD )
	{
		//
		HandleKeepalives();

		// update time
		m_LastKeepaliveTime.start();
	}

	// link the mrefd reflectors listed in the interlink file
	if ( m_LastPeersLinkTime.time() > M17_RECONNECT_PERIOD )
	{
		HandlePeerLinks();
		m_LastPeersLinkTime.start();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// streams helpers

void CM17Protocol::OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip, char peerModule)
{
	// find the stream
	auto stream = GetStream(Header->GetStreamId(), &Ip);
	if ( stream )
	{
		// stream already open
		// skip packet, but tickle the stream
		stream->Tickle();
	}
	else
	{
		// no stream open yet, open a new one
		CCallsign my(Header->GetMyCallsign());
		my.SetSuffix("M17");
		CCallsign rpt1(Header->GetRpt1Callsign());
		CCallsign rpt2(Header->GetRpt2Callsign());

		// find this client, for an interlinked reflector the one of the module
		std::shared_ptr<CClient>client = (' ' == peerModule)
			? g_Reflector.GetClients()->FindClient(Ip, EProtocol::m17)
			: g_Reflector.GetClients()->FindClient(Ip, EProtocol::m17, peerModule);
		if ( client )
		{
			// get client callsign
			rpt1 = client->GetCallsign();
			// and try to open the stream
			if ( (stream = g_Reflector.OpenStream(Header, client)) != nullptr )
			{
				// keep the handle
				m_Streams[stream->GetStreamId()] = stream;
			}
		}
		// release
		g_Reflector.ReleaseClients();

		// update last heard
		g_Reflector.GetUsers()->Hearing(my, rpt1, rpt2, "M17");
		g_Reflector.ReleaseUsers();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// queue helper

void CM17Protocol::HandleQueue(void)
{
	while (! m_Queue.IsEmpty())
	{
		// get the packet
		auto packet = m_Queue.Pop();

		// get our sender's id
		const auto module = packet->GetPacketModule();

		// check if it's header and update cache
		if ( packet->IsDvHeader() )
		{
			// this relies on queue feeder setting valid module id
			// m_StreamsCache[module] will be created if it doesn't exist
			m_StreamsCache[module].m_dvHeader = CDvHeaderPacket((const CDvHeaderPacket &)*packet.get());
			m_StreamsCache[module].m_iSeqCounter = 0;
			m_StreamsCache[module].m_bFromPeer = IsPeerMastering(module);
		}
		else if (packet->IsDvFrame())
		{
			if ((1 == m_StreamsCache[module].m_iSeqCounter % 2) || packet->IsLastPacket())
			{
				// encode it
				SM17Frame frame;

				EncodeM17Packet(frame, m_StreamsCache[module].m_dvHeader, (CDvFramePacket *)packet.get(), m_StreamsCache[module].m_iSeqCounter);

				// push it to all our clients linked to the module and who are not streaming in
				CClients *clients = g_Reflector.GetClients();
				auto it = clients->begin();
				std::shared_ptr<CClient>client = nullptr;
				while ( (client = clients->FindNextClient(EProtocol::m17, it)) != nullptr )
				{
					// an interlinked reflector gets streams from here, but none from another one (as mrefd)
					if ( std::static_pointer_cast<CM17Client>(client)->IsReflector() )
					{
						if ( !client->IsAMaster() && (client->GetReflectorModule() == module) && !m_StreamsCache[module].m_bFromPeer )
							SendToPeerClient(frame, *client, module);
					}
					// is this client busy ?
					else if ( !client->IsAMaster() && ((client->GetReflectorModule() == module) || IsEchoPlaybackFor(*client, module)) )
					{
						// to @ALL, as mrefd does
						memset(frame.lich.addr_dst, 0xFF, 6);
						frame.crc = htons(m17crc.CalcCRC(frame.magic, sizeof(SM17Frame)-2));
						Send(frame, client->GetIp());
					}
				}
				g_Reflector.ReleaseClients();
			}
			m_StreamsCache[module].m_iSeqCounter++;
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// mrefd interlink helpers
//
// Interlinked mrefd reflectors (M17-XXX in the interlink file) link with a 37 byte
// CONN/ACKN carrying the shared modules. Their stream and packet data carry one more
// byte: an mrefd >= 1.0 appends the module, a legacy one appends 1 and addresses the
// module in the destination ("M17-XXX B").

// the M17-XXX designator of a reflector, empty for anything else; GetBase() would
// stop at the '-'
std::string CM17Protocol::Designator(const CCallsign &cs)
{
	std::string s = cs.GetCS().substr(0, CALLSIGN_LEN - 1);
	s.erase(s.find_last_not_of(' ') + 1);
	return (0 == s.compare(0, 4, "M17-")) ? s : std::string();
}

// for data from an interlinked reflector: drop the extra byte and return the module,
// or PEER_DROP for peer data that can't be placed on a shared module
char CM17Protocol::StripPeerModule(CBuffer &Buffer, const CIp &Ip)
{
	const bool voice = (0 == Buffer.Compare((uint8_t *)"M17 ", 4));
	const bool data = (0 == Buffer.Compare((uint8_t *)"M17P", 4));
	if ( !m_bInterlink || !(voice || data) )
		return ' ';

	char module = ' ';
	CPeers *peers = g_Reflector.GetPeers();
	auto peer = peers->FindPeer(Ip, EProtocol::m17);
	if ( peer != nullptr )
	{
		module = PEER_DROP;
		if ( (voice && (sizeof(SM17Frame) + 1 != Buffer.size())) || (data && (Buffer.size() < 38)) )
		{
			g_Reflector.ReleasePeers();
			return module;
		}

		if ( std::static_pointer_cast<CM17Peer>(peer)->IsLegacy() )
		{
			// legacy reflectors send no packet mode data
			if ( voice )
			{
				CCallsign dst;
				dst.CodeIn(Buffer.data() + 6);
				module = dst.GetCSModule();
			}
		}
		else
		{
			module = char(Buffer.data()[Buffer.size() - 1]);
		}

		// only a module this peer shares
		bool shared = false;
		for ( auto it = peer->cbegin(); it != peer->cend(); it++ )
			shared = shared || ((*it)->GetReflectorModule() == module);
		if ( shared )
		{
			Buffer.resize(Buffer.size() - 1);
			// the destination is @ALL or M17-XXX x, neither makes a valid header
			// callsign, so address the frame to this reflector's module instead
			if ( voice )
			{
				CCallsign dst(g_Reflector.GetCallsign());
				dst.SetCSModule(module);
				dst.CodeOut(Buffer.data() + 6);
			}
		}
		else
			module = PEER_DROP;
	}
	g_Reflector.ReleasePeers();
	return module;
}

// listed in the interlink file as an M17 reflector with this IP and exactly these modules?
bool CM17Protocol::IsInterlinkListed(const CCallsign &cs, const CIp &Ip, const char *modules, bool &legacy) const
{
	bool ok = false;
	auto ilmap = g_GateKeeper.GetInterlinkMap();
	auto item = ilmap->FindMapItem(Designator(cs));
	if ( item && ((item->GetProtocol() == "M17") || (item->GetProtocol() == "M17L")) )
	{
		legacy = (item->GetProtocol() == "M17L");
		std::string a(item->GetModules()), b(modules);
		std::sort(a.begin(), a.end());
		std::sort(b.begin(), b.end());
		if ( !item->HasSameIp(Ip) )
			std::cout << "M17 interlink from " << cs << " at " << Ip << " is not from the IP in the interlink file" << std::endl;
		else if ( a != b )
			std::cout << "M17 interlink from " << cs << " is for modules '" << modules << "' but the interlink file says '" << item->GetModules() << "'" << std::endl;
		else
			ok = true;
	}
	else
	{
		std::cout << "M17 interlink from " << cs << " at " << Ip << " is not in the interlink file" << std::endl;
	}
	g_GateKeeper.ReleaseInterlinkMap();
	return ok;
}

void CM17Protocol::OnInterlinkConnectIn(const CCallsign &cs, const char *modules, const CIp &Ip)
{
	std::cout << "M17 interlink connect from " << cs << " at " << Ip << " for modules " << modules << std::endl;

	bool legacy;
	CBuffer reply;
	if ( m_bInterlink && IsInterlinkListed(cs, Ip, modules, legacy) )
		EncodeInterlinkPacket(reply, "ACKN", modules);
	else
		EncodeInterlinkShortPacket(reply, "NACK");
	Send(reply, Ip);
}

void CM17Protocol::OnInterlinkAckIn(const CCallsign &cs, const char *modules, const CIp &Ip)
{
	bool legacy;
	if ( !m_bInterlink || !IsInterlinkListed(cs, Ip, modules, legacy) )
		return;

	for ( const char *m = modules; *m; m++ )
	{
		if ( !g_Reflector.IsValidModule(*m) )
		{
			std::cerr << "This reflector has no module '" << *m << "', so it can't interlink with " << cs << std::endl;
			return;
		}
	}

	CPeers *peers = g_Reflector.GetPeers();
	if ( peers->FindPeer(cs, Ip, EProtocol::m17) == nullptr )
	{
		std::cout << "M17 interlink with " << cs << " at " << Ip << " on modules " << modules << (legacy ? " (legacy)" : "") << std::endl;
		peers->AddPeer(std::make_shared<CM17Peer>(cs, Ip, modules, legacy));
	}
	g_Reflector.ReleasePeers();
}

// is the stream on this module coming in from an interlinked mrefd?
bool CM17Protocol::IsPeerMastering(char module)
{
	bool fromPeer = false;
	CClients *clients = g_Reflector.GetClients();
	auto it = clients->begin();
	std::shared_ptr<CClient>client = nullptr;
	while ( (client = clients->FindNextClient(EProtocol::m17, it)) != nullptr )
	{
		if ( client->IsAMaster() && (client->GetReflectorModule() == module) && std::static_pointer_cast<CM17Client>(client)->IsReflector() )
			fromPeer = true;
	}
	g_Reflector.ReleaseClients();
	return fromPeer;
}

bool CM17Protocol::IsPeer(const CIp &Ip)
{
	const bool found = (nullptr != g_Reflector.GetPeers()->FindPeer(Ip, EProtocol::m17));
	g_Reflector.ReleasePeers();
	return found;
}

// CPeers keeps a peer while it is talking, true only if it is gone
bool CM17Protocol::RemovePeer(const CIp &Ip)
{
	CPeers *peers = g_Reflector.GetPeers();
	auto peer = peers->FindPeer(Ip, EProtocol::m17);
	if ( peer != nullptr )
		peers->RemovePeer(peer);
	const bool removed = (nullptr == peers->FindPeer(Ip, EProtocol::m17));
	g_Reflector.ReleasePeers();
	if ( !removed )
		std::cout << "M17 peer at " << Ip << " is talking, it is removed once its stream ends" << std::endl;
	return removed;
}

// connect the M17 reflectors of the interlink file, drop those no longer listed
void CM17Protocol::HandlePeerLinks(void)
{
	if ( !m_bInterlink )
		return;

	CBuffer buffer;
	auto ilmap = g_GateKeeper.GetInterlinkMap();
	CPeers *peers = g_Reflector.GetPeers();

	auto pit = peers->begin();
	std::shared_ptr<CPeer>peer = nullptr;
	while ( (peer = peers->FindNextPeer(EProtocol::m17, pit)) != nullptr )
	{
		// no longer listed, or listed with other modules, address or legacy flag
		auto item = ilmap->FindMapItem(Designator(peer->GetCallsign()));
		bool changed = (nullptr == item);
		if ( item )
		{
			std::string a(item->GetModules()), b(peer->GetReflectorModules());
			std::sort(a.begin(), a.end());
			std::sort(b.begin(), b.end());
			changed = (a != b) || !item->HasSameIp(peer->GetIp())
				|| ((item->GetProtocol() == "M17L") != std::static_pointer_cast<CM17Peer>(peer)->IsLegacy());
		}
		if ( changed && !peer->IsAMaster() )
		{
			EncodeInterlinkShortPacket(buffer, "DISC");
			Send(buffer, peer->GetIp());
			std::cout << "Sending disconnect packet to M17 peer " << peer->GetCallsign() << ", its interlink entry changed" << std::endl;
			peers->RemovePeer(peer);
		}
	}

	for ( auto it = ilmap->begin(); it != ilmap->end(); it++ )
	{
		if ( (it->second.GetProtocol() != "M17") && (it->second.GetProtocol() != "M17L") )
			continue;
		CCallsign cs;
		cs.SetCallsign(it->first, false);
		if ( nullptr != peers->FindPeer(cs, EProtocol::m17) )
			continue;
#ifndef NO_DHT
		// a line with only the modules takes the address from the mrefd's DHT document
		it->second.UpdateIP(g_Configure.GetString(g_Keys.ip.ipv6address).empty());
		if ( it->second.UsesDHT() && !it->second.GetIp().IsSet() )
		{
			g_Reflector.GetDHTConfig(it->first);
			continue;
		}
		if ( it->second.UsesDHT() )
		{
			bool ok = true;
			for ( const auto c : it->second.GetModules() )
			{
				if ( std::string::npos == it->second.GetCMods().find(c) )
				{
					ok = false;
					std::cerr << it->first << " has no module '" << c << "'" << std::endl;
				}
			}
			if ( !ok )
				continue;
		}
#endif
		if ( it->second.GetIp().IsSet() )
		{
			EncodeInterlinkPacket(buffer, "CONN", it->second.GetModules().c_str());
			Send(buffer, it->second.GetIp());
			std::cout << "Sent connect packet to M17 peer " << it->first << " @ " << it->second.GetIp() << " for modules " << it->second.GetModules() << std::endl;
		}
	}

	g_Reflector.ReleasePeers();
	g_GateKeeper.ReleaseInterlinkMap();
}

// stream data to an interlinked reflector, see above for the extra byte
void CM17Protocol::SendToPeerClient(const SM17Frame &frame, const CClient &client, char module)
{
	SM17Frame peerFrame(frame);
	if ( EM17ClientType::legacy == static_cast<const CM17Client &>(client).GetType() )
	{
		CCallsign dst(client.GetCallsign());
		dst.SetCSModule(module);
		dst.CodeOut(peerFrame.lich.addr_dst);
	}
	else
	{
		// broadcast, @ALL
		memset(peerFrame.lich.addr_dst, 0xFF, 6);
	}
	peerFrame.crc = htons(m17crc.CalcCRC(peerFrame.magic, sizeof(SM17Frame)-2));

	CBuffer buffer((uint8_t *)&peerFrame, sizeof(SM17Frame));
	buffer.Append(uint8_t((EM17ClientType::legacy == static_cast<const CM17Client &>(client).GetType()) ? 1 : module));
	Send(buffer, client.GetIp());
}

////////////////////////////////////////////////////////////////////////////////////////
// client helpers

bool CM17Protocol::IsListenOnlyClient(const CIp &Ip)
{
	std::shared_ptr<CClient>client = g_Reflector.GetClients()->FindClient(Ip, EProtocol::m17);
	const bool listenOnly = client && std::static_pointer_cast<CM17Client>(client)->IsListenOnly();
	g_Reflector.ReleaseClients();
	return listenOnly;
}

////////////////////////////////////////////////////////////////////////////////////////
// position helpers

void CM17Protocol::OnGnssIn(const CCallsign &my, const uint8_t *meta)
{
	double lat, lon;
	if ( !CM17Packet::GnssPosition(meta, lat, lon) )
		return;

	g_Reflector.GetUsers()->Location(my, lat, lon);
	g_Reflector.ReleaseUsers();
}

////////////////////////////////////////////////////////////////////////////////////////
// echo helpers

// the echo module plays a recording back to its module only, so a caller
// linked elsewhere also gets the playback of their own recording
bool CM17Protocol::IsEchoPlaybackFor(const CClient &client, char module) const
{
	if ( (module != g_Reflector.GetEchoModule()) || (client.GetReflectorModule() == module) )
		return false;

	auto item = m_StreamsCache.find(module);
	if ( item == m_StreamsCache.end() )
		return false;

	// the echo thread sends with stream ids 0xEC00-0xECFF and the caller as UR
	const CDvHeaderPacket &header = item->second.m_dvHeader;
	return (header.GetStreamId() >= 0xEC00) && (header.GetStreamId() <= 0xECFF) && (header.GetUrCallsign().GetBase() == client.GetCallsign().GetBase());
}

////////////////////////////////////////////////////////////////////////////////////////
// packet mode helpers

void CM17Protocol::OnPacketModePacketIn(const CBuffer &Buffer, const CIp &Ip, char peerModule)
{
	CCallsign src, rpt1, rpt2;
	src.CodeIn(Buffer.data() + 10);
	bool heard = false;

	CClients *clients = g_Reflector.GetClients();
	std::shared_ptr<CClient>sender = (' ' == peerModule) ? clients->FindClient(Ip, EProtocol::m17) : clients->FindClient(Ip, EProtocol::m17, peerModule);
	if ( !IsM17ClientCallsign(Buffer.data() + 10) )
	{
		std::cout << "M17 packet mode data from " << Ip << " dropped, source '" << M17Address(Buffer.data() + 10) << "' doesn't look like a callsign" << std::endl;
	}
	else if ( (sender != nullptr) && std::static_pointer_cast<CM17Client>(sender)->IsListenOnly() )
	{
		std::cout << "M17 listen-only client " << sender->GetCallsign() << " sent packet mode data, ignored" << std::endl;
	}
	else if ( sender != nullptr )
	{
		const char module = sender->GetReflectorModule();
		// the raw address, @ALL would not decode as a callsign
		auto dstBase = M17Address(Buffer.data() + 4);
		dstBase.erase(std::min(dstBase.find(' '), dstBase.size()));
		if ( !g_GateKeeper.MayTransmit(src, Ip, EProtocol::m17, module) )
		{
			// muted
		}
		// PARROT (or ECHO) gets the packet straight back, to @ALL, as on mrefd
		else if ( (' ' == peerModule) && ((0 == dstBase.compare("PARROT")) || (0 == dstBase.compare("ECHO"))) )
		{
			CBuffer reply(Buffer);
			memset(reply.data() + 4, 0xFF, 6);
			const uint16_t crc = m17crc.CalcCRC(reply.data() + 4, 28);
			reply.data()[32] = uint8_t(crc >> 8);
			reply.data()[33] = uint8_t(crc);
			Send(reply, Ip);
			std::cout << "M17 parrot packet from " << src << " played back to " << sender->GetCallsign() << " at " << Ip << std::endl;
		}
		else
		{
			// last heard once the clients are released, the reflector module as RPT2 like a stream
			heard = true;
			rpt1 = sender->GetCallsign();
			rpt2 = g_Reflector.GetCallsign();
			rpt2.SetCSModule(module);

			std::cout << "M17 packet mode data (type " << unsigned(Buffer.data()[34]) << ") on module " << module << " from " << src;
			// SMS: print the text, up to the terminating zero
			if ( 0x05U == Buffer.data()[34] )
			{
				std::string text;
				for ( auto p = Buffer.data() + 35; p < Buffer.data() + Buffer.size() - 2 && *p; p++ )
					text += isprint(*p) ? char(*p) : '.';
				std::cout << ": \"" << text << "\"";
			}
			std::cout << std::endl;

			// forward it unchanged to all other M17 clients linked to the module, and from
			// a local client also to interlinked reflectors (not legacy ones), as mrefd does
			const bool fromPeer = std::static_pointer_cast<CM17Client>(sender)->IsReflector();
			auto it = clients->begin();
			std::shared_ptr<CClient>client = nullptr;
			while ( (client = clients->FindNextClient(EProtocol::m17, it)) != nullptr )
			{
				if ( (client == sender) || client->IsAMaster() || (client->GetReflectorModule() != module) )
					continue;
				const auto type = std::static_pointer_cast<CM17Client>(client)->GetType();
				if ( EM17ClientType::reflector == type )
				{
					if ( !fromPeer )
					{
						CBuffer withModule(Buffer);
						withModule.Append(uint8_t(module));
						Send(withModule, client->GetIp());
					}
				}
				else if ( EM17ClientType::legacy != type )
				{
					Send(Buffer, client->GetIp());
				}
			}
		}
	}
	g_Reflector.ReleaseClients();

	if ( heard )
	{
		src.SetSuffix("M17");
		g_Reflector.GetUsers()->Hearing(src, rpt1, rpt2, "M17");
		g_Reflector.ReleaseUsers();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// keepalive helpers

void CM17Protocol::HandleKeepalives(void)
{
	// M17 protocol sends and monitors keepalives packets
	// event if the client is currently streaming
	// so, send keepalives to all
	CBuffer keepalive;
	EncodeKeepAlivePacket(keepalive);

	// iterate on clients
	CClients *clients = g_Reflector.GetClients();
	auto it = clients->begin();
	std::shared_ptr<CClient>client = nullptr;
	while ( (client = clients->FindNextClient(EProtocol::m17, it)) != nullptr )
	{
		// interlinked reflectors are handled per peer below
		if ( std::static_pointer_cast<CM17Client>(client)->IsReflector() )
			continue;

		// send keepalive
		Send(keepalive, client->GetIp());

		// is this client busy ?
		if ( client->IsAMaster() )
		{
			// yes, just tickle it
			client->Alive();
		}
		// check it's still with us
		else if ( !client->IsAlive() )
		{
			// no, disconnect
			Send("DISC", client->GetIp());

			// remove it
			std::cout << "M17 client " << client->GetCallsign() << " keepalive timeout" << std::endl;
			clients->RemoveClient(client);
		}
		// blacklisted since it linked? as the mrefd packet description says, DISC with our callsign
		else if ( !g_GateKeeper.MayLink(client->GetCallsign(), client->GetIp(), EProtocol::m17) )
		{
			CBuffer disconnect;
			disconnect.Set("DISC");
			disconnect.resize(10);
			g_Reflector.GetCallsign().CodeOut(disconnect.data() + 4);
			Send(disconnect, client->GetIp());
			std::cout << "M17 client " << client->GetCallsign() << " is no longer allowed to link, disconnected" << std::endl;
			clients->RemoveClient(client);
		}

	}
	g_Reflector.ReleaseClients();

	// interlinked reflectors, keepalive with our M17 designator
	if ( m_bInterlink )
	{
		CBuffer peerKeepalive;
		EncodeInterlinkShortPacket(peerKeepalive, "PING");

		CPeers *peers = g_Reflector.GetPeers();
		auto pit = peers->begin();
		std::shared_ptr<CPeer>peer = nullptr;
		while ( (peer = peers->FindNextPeer(EProtocol::m17, pit)) != nullptr )
		{
			Send(peerKeepalive, peer->GetIp());
			if ( peer->IsAMaster() )
			{
				peer->Alive();
			}
			else if ( !peer->IsAlive() )
			{
				CBuffer disconnect;
				EncodeInterlinkShortPacket(disconnect, "DISC");
				Send(disconnect, peer->GetIp());
				std::cout << "M17 peer " << peer->GetCallsign() << " keepalive timeout" << std::endl;
				peers->RemovePeer(peer);
			}
		}
		g_Reflector.ReleasePeers();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// packet decoding helpers

// the address as mrefd decodes it, before urfd pads it into callsign and module
std::string CM17Protocol::M17Address(const uint8_t *in)
{
	uint64_t coded = 0;
	for (int i = 0; i < 6; i++)
		coded = (coded << 8) | in[i];
	if (coded > 0xee6b27ffffffu)
		return std::string();	// @ALL, # addresses and invalid ones
	const std::string alphabet(" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.");
	std::string s;
	for ( ; coded; coded /= 40 )
		s += alphabet[coded % 40];
	return s;
}

bool CM17Protocol::IsM17ClientCallsign(const uint8_t *in) const
{
	return std::regex_match(M17Address(in), m_ClientRegEx);
}

bool CM17Protocol::IsValidConnectPacket(const CBuffer &Buffer, CCallsign &callsign, char &mod)
{
	uint8_t tag[] = { 'C', 'O', 'N', 'N' };
	bool valid = false;
	if (11 == Buffer.size() && 0 == Buffer.Compare(tag, 4))
	{
		callsign.CodeIn(Buffer.data() + 4);
		mod = Buffer.data()[10];
		valid = (callsign.IsValid() && IsLetter(mod));
		if ( valid && !IsM17ClientCallsign(Buffer.data() + 4) )
		{
			valid = false;
			// WPSD retries with its own name, as mrefd don't log that
			if ( 0 != M17Address(Buffer.data() + 4).compare(0, 4, "WPSD") )
				std::cout << "M17 CONN packet from " << callsign << " rejected, the callsign doesn't look like one" << std::endl;
		}
	}
	return valid;
}

bool CM17Protocol::IsValidListenPacket(const CBuffer &Buffer, CCallsign &callsign, char &mod)
{
	uint8_t tag[] = { 'L', 'S', 'T', 'N' };
	bool valid = false;
	if (11 == Buffer.size() && 0 == Buffer.Compare(tag, 4))
	{
		callsign.CodeIn(Buffer.data() + 4);
		mod = Buffer.data()[10];
		valid = (callsign.IsValid() && IsLetter(mod));
		if ( valid && !std::regex_match(M17Address(Buffer.data() + 4), m_ListenRegEx) )
		{
			valid = false;
			std::cout << "M17 LSTN packet from " << callsign << " rejected, the callsign doesn't look like one" << std::endl;
		}
	}
	return valid;
}

bool CM17Protocol::IsValidDisconnectPacket(const CBuffer &Buffer, CCallsign &callsign)
{
	uint8_t tag[] = { 'D', 'I', 'S', 'C' };
	bool valid = false;
	if ((Buffer.size() == 10) && (0 == Buffer.Compare(tag, 4)))
	{
		callsign.CodeIn(Buffer.data() + 4);
		// or an interlinked mrefd, whose M17-XXX is no valid callsign
		valid = callsign.IsValid() || !Designator(callsign).empty();
	}
	return valid;
}

bool CM17Protocol::IsValidKeepAlivePacket(const CBuffer &Buffer, CCallsign &callsign)
{
	bool valid = false;
	// clients answer with PONG, interlinked reflectors send PING
	if ( (Buffer.size() == 10) && ((0 == Buffer.Compare((uint8_t *)"PONG", 4)) || (0 == Buffer.Compare((uint8_t *)"PING", 4))) )
	{
		callsign.CodeIn(Buffer.data() + 4);
		// or an interlinked mrefd, whose M17-XXX is no valid callsign
		valid = callsign.IsValid() || !Designator(callsign).empty();
	}
	return valid;
}

bool CM17Protocol::IsValidDvPacket(const CBuffer &Buffer, const CIp &Ip, char peerModule, std::unique_ptr<CDvHeaderPacket> &header, std::unique_ptr<CDvFramePacket> &frame)
{
	uint8_t tag[] = { 'M', '1', '7', ' ' };

	if ( (Buffer.size() == sizeof(SM17Frame)) && (0 == Buffer.Compare(tag, sizeof(tag))) )
	{
		// Make the M17 header
		CM17Packet m17(Buffer.data());

		// only unencrypted Codec2 voice, in either TYPE layout
		if ( (ECodecType::none == m17.GetCodecType()) || m17.IsEncrypted() )
			return false;

		// as mrefd, only from a source that looks like a callsign
		if ( !IsM17ClientCallsign(Buffer.data() + 12) )
		{
			if ( 0x8000U & m17.GetFrameNumber() )
				std::cout << "M17 stream from " << Ip << " dropped, source '" << M17Address(Buffer.data() + 12) << "' doesn't look like a callsign" << std::endl;
			return false;
		}

		// get the header
		header = std::unique_ptr<CDvHeaderPacket>(new CDvHeaderPacket(m17));

		// as on mrefd, a client talks on the module it is linked on, whatever the
		// destination (an interlinked mrefd's frames are readdressed in StripPeerModule)
		if ( ' ' == peerModule )
		{
			char module = ' ';
			auto client = g_Reflector.GetClients()->FindClient(Ip, EProtocol::m17);
			if ( client )
				module = client->GetReflectorModule();
			g_Reflector.ReleaseClients();
			if ( ' ' == module )
				return false;

			// except a test to PARROT (as on mrefd) or ECHO, that goes to the echo module
			const char echo = g_Reflector.GetEchoModule();
			const auto dst = m17.IsBroadcast() ? std::string() : header->GetRpt2Callsign().GetBase();
			if ( (' ' != echo) && ((0 == dst.compare("PARROT")) || (0 == dst.compare("ECHO"))) )
				module = echo;

			CCallsign rpt(g_Reflector.GetCallsign());
			rpt.SetCSModule(module);
			header->SetRpt2Callsign(rpt);
			rpt.SetCSModule('G');
			header->SetRpt1Callsign(rpt);
		}

		// get the frame
		frame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(m17));

		// check validity of packets
		if ( header && header->IsValid() && frame && frame->IsValid() )
			return true;
	}
	return false;
}


bool CM17Protocol::IsValidPacketModePacket(const CBuffer &Buffer) const
{
	uint8_t tag[] = { 'M', '1', '7', 'P' };

	// tag + LSF (28) + LSF CRC (2) + payload (type, data, CRC: 3..825)
	if ( (Buffer.size() < 37) || (Buffer.size() > 859) || (0 != Buffer.Compare(tag, 4)) )
		return false;

	const uint8_t *p = Buffer.data();
	const size_t len = Buffer.size();
	return (m17crc.CalcCRC(p + 4, 28) == ((p[32] << 8) | p[33])) &&
	       (m17crc.CalcCRC(p + 34, len - 36) == ((p[len - 2] << 8) | p[len - 1]));
}

// CONN or ACKN between reflectors: tag, M17-XXX designator, null terminated modules
bool CM17Protocol::IsValidInterlinkPacket(const CBuffer &Buffer, const char *tag, CCallsign &cs, char *modules) const
{
	if ( (37 != Buffer.size()) || (0 != Buffer.Compare((uint8_t *)tag, 4)) )
		return false;
	cs.CodeIn(Buffer.data() + 4);
	memcpy(modules, Buffer.data() + 10, 27);
	return (0 == modules[26]) && (0 != modules[0]) && !Designator(cs).empty();
}

////////////////////////////////////////////////////////////////////////////////////////
// packet encoding helpers

// CONN or ACKN to an mrefd: tag, our M17 designator and the null terminated modules
void CM17Protocol::EncodeInterlinkPacket(CBuffer &Buffer, const char *tag, const char *modules) const
{
	Buffer.Set(tag);
	Buffer.resize(37);
	memset(Buffer.data() + 4, 0, 33);
	m_InterlinkCallsign.CodeOut(Buffer.data() + 4);
	memcpy(Buffer.data() + 10, modules, std::min(strlen(modules), size_t(26)));
}

// NACK, PING or DISC to an mrefd: tag and our M17 designator
void CM17Protocol::EncodeInterlinkShortPacket(CBuffer &Buffer, const char *tag) const
{
	Buffer.Set(tag);
	Buffer.resize(10);
	m_InterlinkCallsign.CodeOut(Buffer.data() + 4);
}

void CM17Protocol::EncodeKeepAlivePacket(CBuffer &Buffer)
{
	Buffer.resize(10);
	memcpy(Buffer.data(), "PING", 4);
	g_Reflector.GetCallsign().CodeOut(Buffer.data() + 4);
}

void CM17Protocol::EncodeM17Packet(SM17Frame &frame, const CDvHeaderPacket &Header, const CDvFramePacket *DvFrame, uint32_t iSeq) const
{
	ECodecType codec_in = Header.GetCodecIn();  // We'll need this


	// do the lich structure first
	// first, the src callsign (the lich.dest is set in HandleQueue)
	CCallsign from = Header.GetMyCallsign();
	from.CodeOut(frame.lich.addr_src);
	// then the frame type, if the incoming frame is M17 1600, then it will be Voice+Data only, otherwise Voice-Only,
	// with the META subtype and CAN of an M17 source, so GNSS is not taken for text
	uint16_t frametype = (ECodecType::c2_1600==codec_in) ? 0x7U : 0x5U;
	switch (Header.GetM17Meta())
	{
		case EM17Meta::gnss:
			frametype |= 0x1U << 5;
			break;
		case EM17Meta::ecd:
			frametype |= 0x2U << 5;
			break;
		default:
			break;
	}
	frametype |= (0xFU & Header.GetM17Can()) << 7;
	frame.lich.frametype = htons(frametype);
	memcpy(frame.lich.nonce, DvFrame->GetNonce(), 14);

	// now the main part of the packet
	memcpy(frame.magic, "M17 ", 4);
	// the frame number comes from the stream sequence counter
	uint16_t fn = (iSeq / 2) % 0x8000U;
	if (DvFrame->IsLastPacket())
		fn |= 0x8000U;
	frame.framenumber = htons(fn);
	memcpy(frame.payload, DvFrame->GetCodecData(ECodecType::c2_3200), 16);
	frame.streamid = Header.GetStreamId();	// no host<--->network byte swapping since we never do any math on this value
	// the CRC will be set in HandleQueue, after lich.dest is set
}
