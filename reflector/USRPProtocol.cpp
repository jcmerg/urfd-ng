//  Copyright © 2015 Jean-Luc Deltombe (LX3JL). All rights reserved.

// urfd -- The universal reflector
// Copyright © 2023 Thomas A. Early N7TAE
// Copyright © 2023 Doug McLain AD8DP
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

#include "Defines.h"
#include <string.h>
#include "USRPClient.h"
#include "USRPProtocol.h"

#include "Global.h"
#include "Utils.h"
#include "RejectLog.h"

// chan_usrp's struct _chan_usrp_bufhdr (32 bytes): "USRP", then seq, memory,
// keyup, talkgroup, type, mpxid and reserved, each a network order u32; a voice
// packet adds 160 samples s16 little endian (20 ms at 8 kHz)
const uint32_t USRP_TYPE_VOICE = 0;
const uint32_t USRP_TYPE_TEXT  = 2;

const uint32_t USRP_KEYUP_FALSE  = 0;
const uint32_t USRP_KEYUP_TRUE   = 1;

const uint8_t TLV_TAG_SET_INFO = 8;

static uint32_t GetU32BE(const uint8_t *p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static void PutU32BE(uint8_t *p, uint32_t v)
{
	p[0] = (v >> 24) & 0xFFU;
	p[1] = (v >> 16) & 0xFFU;
	p[2] = (v >> 8) & 0xFFU;
	p[3] = v & 0xFFU;
}

static bool IsUSRP(const CBuffer &Buffer)
{
	return ( (Buffer.size() >= 32) && !memcmp(Buffer.data(), "USRP", 4) );
}

////////////////////////////////////////////////////////////////////////////////////////
// operation

bool CUSRPProtocol::Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6)
{
	CBuffer buffer;
	std::ifstream file;
	std::streampos size;

	// base class, create the listing port for the read-write client
	if (! CProtocol::Initialize(type, ptype, port, has_ipv4, has_ipv6))
		return false;

	m_Module = g_Configure.GetAutolinkModule(g_Keys.usrp.module);

	// gain in dB, -40 to +40, before the transcoder and after it
	if (g_Configure.Contains(g_Keys.usrp.rxgain))
	{
		int db = std::stoi(g_Configure.GetString(g_Keys.usrp.rxgain));
		m_RxGainNum = CUtils::GainFromDb(db);
		std::cout << "USRP: RxGain = " << db << " dB" << std::endl;
	}
	if (g_Configure.Contains(g_Keys.usrp.txgain))
	{
		int db = std::stoi(g_Configure.GetString(g_Keys.usrp.txgain));
		m_TxGainNum = CUtils::GainFromDb(db);
		std::cout << "USRP: TxGain = " << db << " dB" << std::endl;
	}

	// create the one special USRP Tx/Rx client
	auto scs = g_Configure.GetString(g_Keys.usrp.callsign);
	if (scs.compare("NONE"))
	{
		m_Callsign.SetCallsign(scs, false);
		CIp ip(AF_INET, uint16_t(g_Configure.GetUnsigned(g_Keys.usrp.txport)), g_Configure.GetString(g_Keys.usrp.ip).c_str());
		auto newclient = std::make_shared<CUSRPClient>(m_Callsign, ip);
		newclient->SetReflectorModule(m_Module);
		g_Reflector.GetClients()->AddClient(newclient);
		g_Reflector.ReleaseClients();
	}

	// now create "listen-only" clients, as many as specified
	if (g_Configure.Contains(g_Keys.usrp.filepath))
		file.open(g_Configure.GetString(g_Keys.usrp.filepath), std::ios::in | std::ios::binary | std::ios::ate);
	if ( file.is_open() )
	{
		// read file
		size = file.tellg();
		if ( size > 0 )
		{
			// read file into buffer
			buffer.resize((int)size+1);
			file.seekg (0, std::ios::beg);
			file.read((char *)buffer.data(), (int)size);

			// close file
			file.close();
		}
	}

	if ( buffer.size() > 0 )
	{
		char *ptr1 = (char *)buffer.data();
		char *ptr2;

		while ( (ptr2 = ::strchr(ptr1, '\n')) != nullptr )
		{
			*ptr2 = 0;
			char *ip;
			char *port;
			char *clientcs;

			if ( ((ip = ::strtok(ptr1, ";")) != nullptr) &&
				((port = ::strtok(nullptr, ";")) != nullptr) &&
				((clientcs = ::strtok(nullptr, ";")) != nullptr) )
			{
				uint16_t ui = atoi(port);
				CIp Ip(AF_INET, ui, ip);
				CCallsign cs;
				cs.SetCallsign(clientcs, false);
				auto newclient = std::make_shared<CUSRPClient>(cs, Ip);
				newclient->SetReflectorModule(m_Module);
				g_Reflector.GetClients()->AddClient(newclient);
				g_Reflector.ReleaseClients();
			}
			ptr1 = ptr2+1;
		}
	}

	// update time
	m_LastKeepaliveTime.start();

	// done
	return true;
}



////////////////////////////////////////////////////////////////////////////////////////
// task

void CUSRPProtocol::Task(void)
{
	CBuffer   Buffer;
	CIp       Ip;
	CCallsign Callsign;
	std::unique_ptr<CDvFramePacket>  Frame;

	// handle incoming packets
#if USRP_IPV6==true
#if USRP_IPV4==true
	if ( ReceiveDS(Buffer, Ip, 20) )
#else
	if ( Receive6(Buffer, Ip, 20) )
#endif
#else
	if ( Receive4(Buffer, Ip, 20) )
#endif
	{
		// crack the packet
		if ( IsValidDvPacket(Buffer, 0, Frame) )
		{
			// the voice opens a stream if the TEXT packet didn't
			auto sid = GetStreamIdFromIp(Ip);
			if ( !sid )
			{
				// the talker SET_INFO announced, else the node itself
				CCallsign my(m_HasTalker ? m_Talker : m_Callsign);
				if ( 0 == my.GetDmrid() )
				{
					g_LDid.Lock();
					auto id = g_LDid.FindDmrid(my.GetKey());
					g_LDid.Unlock();
					my.SetDmrid(id, false);
				}
				sid = OpenStream(my, Ip);
			}
			// nothing after the unkey goes into the ending stream
			if ( sid && !m_EndedStreams.count(sid) )
			{
				// push the packet
				Frame->SetStreamId(sid);
				OnDvFramePacketIn(Frame, &Ip);
			}
		}
		else if ( IsValidDvHeaderPacket(Buffer, Callsign) )
		{
			// DVSwitch announces the talker before the voice
			m_HasTalker = Callsign.IsValid();
			if ( m_HasTalker )
			{
				m_Talker = Callsign;
				if ( !GetStreamIdFromIp(Ip) )
					OpenStream(m_Talker, Ip);
			}
		}
		else if ( IsValidDvLastPacket(Buffer) )
		{
			// the unkey ends the stream with a silent last frame
			m_HasTalker = false;
			auto sid = GetStreamIdFromIp(Ip);
			if ( sid && !m_EndedStreams.count(sid) )
			{
				int16_t silence[160] = {};
				Frame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(silence, sid, true));
				m_EndedStreams.insert(sid);
				OnDvFramePacketIn(Frame, &Ip);
			}
		}
		else
		{
			// invalid packet: the first of an address each hour
			if (RejectLog("USRP: unknown packets", Ip.GetAddress()))
			{
				std::string title("Unknown USRP packet from ");
				title += Ip.GetAddress();
				Buffer.Dump(title);
			}
		}
	}

	// handle end of streaming timeout
	CheckStreamsTimeout();
	for ( auto it=m_EndedStreams.begin(); it!=m_EndedStreams.end(); )
		it = m_Streams.count(*it) ? std::next(it) : m_EndedStreams.erase(it);

	// handle queue from reflector
	HandleQueue();

	// keep client alive
	if ( m_LastKeepaliveTime.time() > USRP_KEEPALIVE_PERIOD )
	{
		//
		HandleKeepalives();

		// update time
		m_LastKeepaliveTime.start();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// streams helpers

void CUSRPProtocol::OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip)
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
		CCallsign rpt1(Header->GetRpt1Callsign());
		CCallsign rpt2(Header->GetRpt2Callsign());

		// find this client
		std::shared_ptr<CClient>client = g_Reflector.GetClients()->FindClient(Ip, EProtocol::usrp);
		if ( client )
		{
			// get client callsign
			rpt1 = client->GetCallsign();
			auto m = client->GetReflectorModule();
			Header->SetRpt2Module(m);
			rpt2.SetCSModule(m);
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
		g_Reflector.GetUsers()->Hearing(my, rpt1, rpt2, "USRP");
		g_Reflector.ReleaseUsers();
	}
}

// the stream the sender at Ip has open, 0 if none
uint16_t CUSRPProtocol::GetStreamIdFromIp(const CIp &Ip)
{
	for ( auto &it : m_Streams )
	{
		auto owner = it.second->GetOwnerIp();
		if ( (owner != nullptr) && (*owner == Ip) )
			return it.first;
	}
	return 0;
}

// opens a stream for the talker my, if the gatekeeper lets it; returns its id or 0
uint16_t CUSRPProtocol::OpenStream(const CCallsign &my, const CIp &Ip)
{
	// callsign muted?
	if ( !g_GateKeeper.MayTransmit(my, Ip, EProtocol::usrp) )
		return 0;

	// a new stream id for every transmission, never 0
	uint16_t sid;
	do {
		sid = static_cast<uint16_t>(::rand());
	} while ( 0 == sid );

	CCallsign rpt1 = m_Callsign;
	CCallsign rpt2 = m_ReflectorCallsign;
	rpt1.SetCSModule(m_Module);
	rpt2.SetCSModule(' ');
	auto header = std::unique_ptr<CDvHeaderPacket>(new CDvHeaderPacket(my, CCallsign("CQCQCQ"), rpt1, rpt2, sid, true));
	OnDvHeaderPacketIn(header, Ip);
	return GetStreamIdFromIp(Ip);
}

////////////////////////////////////////////////////////////////////////////////////////
// queue helper

void CUSRPProtocol::HandleQueue(void)
{
	while (! m_Queue.IsEmpty())
	{
		// get the packet
		auto packet = m_Queue.Pop();

		// get our sender's id
		const auto module = packet->GetPacketModule();
		CBuffer buffer;

		// check if it's header and update cache
		if ( packet->IsDvHeader() )
		{
			// this relies on queue feeder setting valid module id
			// m_StreamsCache[module] will be created if it doesn't exist
			m_StreamsCache[module].m_dvHeader = CDvHeaderPacket((const CDvHeaderPacket &)*packet.get());
			m_StreamsCache[module].m_iSeqCounter = 0;
			EncodeUSRPHeaderPacket(m_StreamsCache[module].m_dvHeader, m_StreamsCache[module].m_iSeqCounter++, buffer);
		}
		else if ( packet->IsDvFrame() )
		{
			EncodeUSRPPacket(m_StreamsCache[module].m_dvHeader, (const CDvFramePacket &)*packet.get(), m_StreamsCache[module].m_iSeqCounter++, buffer, packet->IsLastPacket());
		}

		// send it
		if ( buffer.size() > 0 )
		{
			// and push it to all our clients linked to the module and who are not streaming in
			CClients *clients = g_Reflector.GetClients();
			auto it = clients->begin();
			std::shared_ptr<CClient>client = nullptr;
			while ( (client = clients->FindNextClient(EProtocol::usrp, it)) != nullptr )
			{
				// is this client busy ?
				if ( !client->IsAMaster() && (client->GetReflectorModule() == module) )
				{
					// no, send the packet
					Send(buffer, client->GetIp());
				}
			}
			g_Reflector.ReleaseClients();
		}
	}
}


////////////////////////////////////////////////////////////////////////////////////////
// packet decoding helpers

bool CUSRPProtocol::IsValidDvPacket(const CBuffer &Buffer, uint16_t sid, std::unique_ptr<CDvFramePacket> &frame)
{
	if ( (Buffer.size() == 352) && IsUSRP(Buffer) && (GetU32BE(Buffer.data()+20) == USRP_TYPE_VOICE) && (GetU32BE(Buffer.data()+12) != USRP_KEYUP_FALSE) )
	{
		int16_t pcm[160];
		for(int i = 0; i < 160; ++i){
			pcm[i] = int16_t((Buffer.data()[32+(i*2)+1] << 8) | Buffer.data()[32+(i*2)]);
		}
		CUtils::ApplyGain(pcm, 160, m_RxGainNum);

		frame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(pcm, sid, false));
		return true;
	}
	return false;
}

// TEXT with a TLV SET_INFO as DVSwitch Analog_Bridge sends it: tag, length, then
// dmr id (3), repeater id (4), tg (3), ts (1), cc (1) and the callsign, 0 terminated
bool CUSRPProtocol::IsValidDvHeaderPacket(const CBuffer &Buffer, CCallsign &talker)
{
	if ( (Buffer.size() == 352) && IsUSRP(Buffer) && (GetU32BE(Buffer.data()+20) == USRP_TYPE_TEXT) && (Buffer.data()[32] == TLV_TAG_SET_INFO) )
	{
		const uint8_t *value = Buffer.data() + 34;
		uint32_t len = Buffer.data()[33];
		uint32_t uiSrcId = (uint32_t(value[0]) << 16) | (uint32_t(value[1]) << 8) | uint32_t(value[2]);
		std::string cs;
		for ( uint32_t i = 12; (i < len) && (34 + i < Buffer.size()) && value[i] && (cs.size() < CALLSIGN_LEN); i++ )
		{
			if ( value[i] != ' ' )
				cs.push_back(char(value[i]));
		}
		// the callsign, with its dmr id, else the callsign of the dmr id; invalid without both
		talker = CCallsign(cs, uiSrcId);
		if ( !talker.IsValid() )
			talker = CCallsign("", uiSrcId);
		return true;
	}
	return false;
}

// the unkey: a bare header (or a voice packet) with keyup 0
bool CUSRPProtocol::IsValidDvLastPacket(const CBuffer &Buffer)
{
	return ( ((Buffer.size() == 32) || (Buffer.size() == 352)) && IsUSRP(Buffer) && (GetU32BE(Buffer.data()+20) == USRP_TYPE_VOICE) && (GetU32BE(Buffer.data()+12) == USRP_KEYUP_FALSE) );
}

void CUSRPProtocol::EncodeUSRPHeaderPacket(const CDvHeaderPacket &Header, uint32_t iSeq, CBuffer &Buffer) const
{
	std::string cs = Header.GetMyCallsign().GetCS();
	cs.erase(cs.find_last_not_of(' ') + 1);
	const uint32_t id = Header.GetMyCallsign().GetDmrid();
	Buffer.resize(352);
	memset(Buffer.data(), 0, 352);
	memcpy(Buffer.data(), "USRP", 4);
	PutU32BE(Buffer.data() + 4, iSeq);
	PutU32BE(Buffer.data() + 12, USRP_KEYUP_FALSE);
	PutU32BE(Buffer.data() + 20, USRP_TYPE_TEXT);
	Buffer.data()[32] = TLV_TAG_SET_INFO;
	Buffer.data()[33] = 13 + cs.size();
	Buffer.data()[34] = (id >> 16) & 0xFFU;
	Buffer.data()[35] = (id >> 8) & 0xFFU;
	Buffer.data()[36] = id & 0xFFU;
	memcpy(Buffer.data()+46, cs.c_str(), cs.size());
}

void CUSRPProtocol::EncodeUSRPPacket(const CDvHeaderPacket &Header, const CDvFramePacket &DvFrame, uint32_t iSeq, CBuffer &Buffer, bool islast) const
{
	if(islast)
	{
		Buffer.resize(32);
		memset(Buffer.data(), 0, 32);
		memcpy(Buffer.data(), "USRP", 4);
		PutU32BE(Buffer.data() + 4, iSeq);
		PutU32BE(Buffer.data() + 12, USRP_KEYUP_FALSE);
	}
	else
	{
		Buffer.resize(352);
		memset(Buffer.data(), 0, 352);
		memcpy(Buffer.data(), "USRP", 4);
		PutU32BE(Buffer.data() + 4, iSeq);
		PutU32BE(Buffer.data() + 12, USRP_KEYUP_TRUE);
		int16_t pcm[160];
		memcpy(pcm, DvFrame.GetCodecData(ECodecType::usrp), sizeof(pcm));
		CUtils::ApplyGain(pcm, 160, m_TxGainNum);
		// the samples little endian, whatever the host is
		for (int i = 0; i < 160; i++)
		{
			Buffer.data()[32 + 2*i]     = uint16_t(pcm[i]) & 0xFFU;
			Buffer.data()[32 + 2*i + 1] = (uint16_t(pcm[i]) >> 8) & 0xFFU;
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// keepalive helpers

void CUSRPProtocol::HandleKeepalives(void)
{
	// iterate on clients
	CClients *clients = g_Reflector.GetClients();
	auto it = clients->begin();
	std::shared_ptr<CClient>client = nullptr;
	while ( (client = clients->FindNextClient(EProtocol::usrp, it)) != nullptr )
	{
		// is this client busy ?
		//if ( client->IsAMaster() )
		//{
			// yes, just tickle it
			client->Alive();
		//}
		// check it's still with us
		//else if ( !client->IsAlive() )
		//{
			// no, remove it
			//std::cout << "USRP client " << client->GetCallsign() << " keepalive timeout" << std::endl;
			//clients->RemoveClient(client);
		//}

	}
	g_Reflector.ReleaseClients();
}
