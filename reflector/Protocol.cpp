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

#include "Defines.h"
#include "Global.h"
#include "Protocol.h"
#include "Clients.h"
#include "ProtocolNames.h"
#include "MmdvmClientConfig.h"
#include "ThreadName.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iostream>
#include <poll.h>
#include <unistd.h>

////////////////////////////////////////////////////////////////////////////////////////
// constructor


CProtocol::CProtocol() : keep_running(true)
{
	if (!m_QueueWake.Open())
		std::cerr << "Protocol: no wake descriptor (" << strerror(errno) << "), its queue waits for the timeout" << std::endl;
}


////////////////////////////////////////////////////////////////////////////////////////
// destructor

CProtocol::~CProtocol()
{
	// kill threads
	Close();

	// empty queue
	m_Queue.Clear();
}

void CProtocol::Push(std::unique_ptr<CPacket> p)
{
	m_Queue.Push(std::move(p));
	// the packet goes out at once
	Wake();
}

void CProtocol::Wake(void)
{
	m_QueueWake.Signal();
}

////////////////////////////////////////////////////////////////////////////////////////
// initialization

bool CProtocol::Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6)
{
	m_Port = port;
	m_ProtocolType = ptype;
	// init reflector apparent callsign
	m_ReflectorCallsign = g_Reflector.GetCallsign();

	// reset stop flag
	keep_running = true;

	// update the reflector callsign
	if (type)
		m_ReflectorCallsign.PatchCallsign(0, type, 3);

	// create our sockets
	if (has_ipv4)
	{
		const std::string ipv4binding(g_Configure.GetString(g_Keys.ip.ipv4bind));
		CIp ip4(AF_INET, port, ipv4binding.c_str());
		if ( ip4.IsSet() )
		{
			if (! m_Socket4.Open(ip4))
				return false;
		}
		std::cout << "Listening on " << ip4 << std::endl;
	}

	if (g_Configure.IsString(g_Keys.ip.ipv6bind))
	{
		if (has_ipv6)
		{
			const std::string ipv6binding(g_Configure.GetString(g_Keys.ip.ipv6bind));
			CIp ip6(AF_INET6, port, ipv6binding.c_str());
			if ( ip6.IsSet() )
			{
				if (! m_Socket6.Open(ip6))
				{
					m_Socket4.Close();
					return false;
				}
				std::cout << "Listening on " << ip6 << std::endl;
			}
		}
	}

	try
	{
		m_Future = std::async(std::launch::async, &CProtocol::Thread, this);
	}
	catch (const std::exception &e)
	{
		std::cerr << "Could not start protocol on port " << port << ": " << e.what() << std::endl;
		m_Socket4.Close();
		m_Socket6.Close();
		return false;
	}

	return true;
}

void CProtocol::Thread()
{
	// urfd-dextra, urfd-mmdvm, ...: the name of BlockProtocols; a client
	// (urfd links out) is <protocol>-cl, which keeps it within the 15
	// characters of a thread name
	std::string name;
	switch (m_ProtocolType)
	{
		case EProtocol::dextraclient: name = "dextra-cl"; break;
		case EProtocol::dplusclient:  name = "dplus-cl";  break;
		case EProtocol::dcsclient:    name = "dcs-cl";    break;
		case EProtocol::ysfclient:    name = "ysf-cl";    break;
		case EProtocol::mmdvmclient:  name = "mmdvm-cl";  break;
		case EProtocol::svxreflector: name = "svx-cl";    break;
		default:
			// a plugin's protocol, too: its name, cut at 15 characters
			name = "proto";
			for (const auto &n : ProtocolNames())
				if (n.second == m_ProtocolType)
				{
					name.clear();
					for (char c : n.first)
						name += char(std::tolower((unsigned char)c));
					break;
				}
	}
	SetThreadName("urfd-" + name);
	while (keep_running)
	{
		Task();
	}
}

void CProtocol::StopTask(void)
{
	keep_running = false;
	Wake();
	if ( m_Future.valid() )
	{
		m_Future.get();
	}
}

void CProtocol::Close(void)
{
	StopTask();
	m_Socket4.Close();
	m_Socket6.Close();
}

////////////////////////////////////////////////////////////////////////////////////////
// streams helpers

void CProtocol::OnDvFramePacketIn(std::unique_ptr<CDvFramePacket> &Frame, const CIp *Ip)
{
	// find the stream
	auto stream = GetStream(Frame->GetStreamId(), Ip);
	if ( stream )
	{
		// set the packet module, the transcoder needs this; the stream's, as a
		// client may be linked to more than one (a DMR hotspot, one per slot)
		Frame->SetPacketModule(stream->GetModule());
		// D-Star positions (DPRS) in the slow data go to the last heard list
		double lat, lon;
		if ( (ECodecType::dstar == Frame->GetCodecIn()) && stream->DprsIn(*Frame, lat, lon) )
		{
			g_Reflector.GetUsers()->Location(stream->GetUserCallsign(), lat, lon);
			g_Reflector.ReleaseUsers();
		}
		// and push
		stream->Push(std::move(Frame));
	}
	else
	{
		// once per stream id and peer
		const uint16_t thisId = Frame->GetStreamId();
		if (!Ip || LogOnce(0, *Ip, thisId))
		{
			std::cout << std::showbase << std::hex;
			std::cout << "Orphaned Frame with ID " << ntohs(thisId) << std::noshowbase << std::dec << " on " << (Ip ? *Ip : CIp()) << std::endl;
		}
		Frame.reset();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// stream handle helpers

// the one way a stream leaves m_Streams: closed, taken out, its id handed on
// (StreamClosed); returns the next one
CProtocol::StreamMap::iterator CProtocol::CloseStreamAt(StreamMap::iterator it)
{
	g_Reflector.CloseStream(it->second);
	const uint16_t streamid = it->first;
	it = m_Streams.erase(it);
	StreamClosed(streamid);
	return it;
}

bool CProtocol::LogOnce(int kind, const CIp &ip, uint16_t id)
{
	const uint64_t key = (uint64_t(kind) << 48) | (uint64_t(ip.GetAddr()) << 16) | ip.GetPort();
	const auto it = m_Logged.find(key);
	if (it != m_Logged.end() && it->second == id)
		return false;
	if (it == m_Logged.end() && m_Logged.size() >= 64)
		m_Logged.clear();	// peers that went
	m_Logged[key] = id;
	return true;
}

void CProtocol::StreamClosed(uint16_t streamid)
{
	m_EndedStreams.erase(streamid);
	// the next transmission that opens gets another id (NewStreamId)
	m_StreamIdSeed++;
}

std::shared_ptr<CPacketStream> CProtocol::OpenStreamOf(const CIp &ip, uint16_t *sid) const
{
	for (const auto &it : m_Streams)
	{
		const CIp *owner = it.second->GetOwnerIp();
		if (owner && (*owner == ip))
		{
			if (sid)
				*sid = it.first;
			return it.second;
		}
	}
	return nullptr;
}

// an id a protocol may give a new stream: not 0, not in use here, no stream open
// elsewhere with it (a loop to the reflector) and none closed lately (its late
// transcoder answers and reports)
bool CProtocol::StreamIdTaken(uint16_t id) const
{
	return 0 == id || m_Streams.count(id) || g_Reflector.IsStreamIdInUse(id);
}

bool CProtocol::StreamIdLive(uint16_t id) const
{
	return m_Streams.count(id) || g_Reflector.IsStreamIdRecent(id);
}

uint16_t CProtocol::NewStreamId(const CIp &ip)
{
	// from the peer's address and port and the seed: a talker refused gets the
	// same one with every packet until a stream of this protocol closes. Only
	// the taken ones are in the way, so it ends after a few tries
	const uint32_t base = (ip.GetAddr() ^ (uint32_t(ip.GetPort()) * 0x10001u)) + uint32_t(m_StreamIdSeed) * 0x9E3779B1u;
	for (uint32_t k = 0; ; k++)
	{
		const uint32_t h = base + k * 0x85EBCA6Bu;
		const uint16_t id = uint16_t(h ^ (h >> 16));
		if (!StreamIdTaken(id))
			return id;
	}
}

std::shared_ptr<CPacketStream> CProtocol::GetStream(uint16_t uiStreamId, const CIp *Ip)
{
	auto it = m_Streams.find(uiStreamId);
	if (it == m_Streams.end())
		return nullptr;

	if (Ip != nullptr && it->second->GetOwnerIp() != nullptr)
	{
		if (*Ip == *it->second->GetOwnerIp())
		{
			return it->second;
		}
	}

	return nullptr;
}

void CProtocol::CheckStreamsTimeout(void)
{
	for ( auto it=m_Streams.begin(); it!=m_Streams.end(); )
	{
		// its last packet has gone out to the protocols, or it timed out (end lost)?
		if ( it->second->IsEnded() || it->second->IsExpired() )
		{
			// the end out first (a lost one is made up), and sent by this protocol
			// while the talker is still the master: it doesn't get it back
			g_Reflector.EndStream(it->second);
			HandleQueue();
			// and close it, and remove it from the m_Streams map
			it = CloseStreamAt(it);
		}
		else
		{
			it++;
		}
	}
}

// a client that leaves while it talks: a master can't be removed, so its
// stream is closed first. Call it without the clients lock.
void CProtocol::CloseStreamsOf(const CIp &Ip)
{
	for ( auto it=m_Streams.begin(); it!=m_Streams.end(); )
	{
		const CIp *owner = it->second->GetOwnerIp();
		if ( owner && (*owner == Ip) )
			it = CloseStreamAt(it);
		else
			it++;
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// syntax helper

bool CProtocol::IsNumber(char c) const
{
	return ((c >= '0') && (c <= '9'));
}

bool CProtocol::IsLetter(char c) const
{
	return ((c >= 'A') && (c <= 'Z'));
}

bool CProtocol::IsSpace(char c) const
{
	return (c == ' ');
}

CCallsign CProtocol::RemoteReflectorCallsign(const std::string &announced, const std::string &host, const CCallsign &own, char module)
{
	CCallsign cs;
	if (! announced.empty())
		cs.SetCallsign(announced, false);
	else
	{
		std::string label(host.substr(0, host.find('.')));
		for (auto &c : label)
			c = toupper(c);
		cs.SetCallsign(label, false);
		if (label.size() < 3 || label.size() >= CALLSIGN_LEN || ! cs.IsValid())
			cs = own;
	}
	cs.SetCSModule(module);
	return cs;
}

////////////////////////////////////////////////////////////////////////////////////////
// DestId to Module helper

char CProtocol::DmrDstIdToModule(uint32_t tg) const
{
	return ((char)((tg % 26)-1) + 'A');
}

uint32_t CProtocol::ModuleToDmrDestId(char m) const
{
	return (uint32_t)(m - 'A')+1;
}

////////////////////////////////////////////////////////////////////////////////////////
// Receivers

// a stream open: its timeout is checked every PROTOCOL_TICK; else only timers
// of seconds are due
int CProtocol::WaitTime(void) const
{
	return m_Streams.empty() ? PROTOCOL_IDLE_WAIT : PROTOCOL_TICK;
}

// poll() on fds[0] to fds[n - 1] and the queue's wake (CWakeFd), which goes into fds[n]
// (the caller's room for it) and whose wake-up is cleared here
int CProtocol::PollWithQueue(struct pollfd *fds, nfds_t n, int time_ms)
{
	fds[n] = { m_QueueWake.Fd(), POLLIN, 0 };	// poll leaves out a -1
	const auto rval = poll(fds, n + 1, time_ms);
	if (rval < 0 && errno != EINTR)
		std::cerr << "Receive poll error: " << strerror(errno) << std::endl;
	if (rval > 0 && (fds[n].revents & POLLIN))
		m_QueueWake.Clear();
	return rval;
}

bool CProtocol::WaitFor(std::vector<struct pollfd> &fds, int time_ms)
{
	const auto n = fds.size();
	fds.emplace_back();
	const auto rval = PollWithQueue(fds.data(), n, time_ms);
	fds.pop_back();
	return rval > 0 && std::any_of(fds.begin(), fds.end(), [](const struct pollfd &f) { return f.revents != 0; });
}

// Waits up to time_ms for a packet on the sockets (fd < 0: none), and returns
// early, without a packet, when something was pushed into the queue: the Task
// loops handle the queue after the receive, so a packet of the router does not
// wait for the next incoming one (same-protocol listeners got them in pairs)
bool CProtocol::Wait(int fd4, int fd6, int time_ms, bool &ready4, bool &ready6)
{
	ready4 = ready6 = false;
	struct pollfd fds[3];
	nfds_t n = 0;
	int i4 = -1, i6 = -1;
	if (fd4 >= 0) { fds[n] = { fd4, POLLIN, 0 }; i4 = n++; }
	if (fd6 >= 0) { fds[n] = { fd6, POLLIN, 0 }; i6 = n++; }

	// without a socket (none bound) on the queue alone, the task would spin else
	if (PollWithQueue(fds, n, time_ms) <= 0)
		return false;
	ready4 = (i4 >= 0 && (fds[i4].revents & POLLIN));
	ready6 = (i6 >= 0 && (fds[i6].revents & POLLIN));
	return ready4 || ready6;
}

bool CProtocol::Receive6(CBuffer &buf, CIp &ip, int time_ms)
{
	bool r4, r6;
	if (Wait(-1, m_Socket6.GetSocket(), time_ms, r4, r6))
		return m_Socket6.ReceiveFrom(buf, ip);
	return false;
}

bool CProtocol::Receive4(CBuffer &buf, CIp &ip, int time_ms)
{
	bool r4, r6;
	if (Wait(m_Socket4.GetSocket(), -1, time_ms, r4, r6))
		return m_Socket4.ReceiveFrom(buf, ip);
	return false;
}

bool CProtocol::ReceiveDS(CBuffer &buf, CIp &ip, int time_ms)
{
	bool r4, r6;
	if (! Wait(m_Socket4.GetSocket(), m_Socket6.GetSocket(), time_ms, r4, r6))
		return false;
	if (r4)
		return m_Socket4.ReceiveFrom(buf, ip);
	return m_Socket6.ReceiveFrom(buf, ip);
}

////////////////////////////////////////////////////////////////////////////////////////
// dual stack senders

void CProtocol::Send(const CBuffer &buf, const CIp &Ip) const
{
	switch (Ip.GetFamily())
	{
	case AF_INET:
		m_Socket4.Send(buf, Ip);
		break;
	case AF_INET6:
		m_Socket6.Send(buf, Ip);
		break;
	default:
		std::cerr << "Wrong family: " << Ip.GetFamily() << std::endl;
		break;
	}
}

void CProtocol::Send(const char *buf, const CIp &Ip) const
{
	switch (Ip.GetFamily())
	{
	case AF_INET:
		m_Socket4.Send(buf, Ip);
		break;
	case AF_INET6:
		m_Socket6.Send(buf, Ip);
		break;
	default:
		std::cerr << "ERROR: wrong family: " << Ip.GetFamily() << std::endl;
		break;
	}
}

void CProtocol::Send(const CBuffer &buf, const CIp &Ip, uint16_t port) const
{
	switch (Ip.GetFamily())
	{
	case AF_INET:
		m_Socket4.Send(buf, Ip, port);
		break;
	case AF_INET6:
		m_Socket6.Send(buf, Ip, port);
		break;
	default:
		std::cerr << "Wrong family: " << Ip.GetFamily() << " on port " << port << std::endl;
		break;
	}
}

void CProtocol::Send(const char *buf, const CIp &Ip, uint16_t port) const
{
	switch (Ip.GetFamily())
	{
	case AF_INET:
		m_Socket4.Send(buf, Ip, port);
		break;
	case AF_INET6:
		m_Socket6.Send(buf, Ip, port);
		break;
	default:
		std::cerr << "Wrong family: " << Ip.GetFamily() << " on port " << port << std::endl;
		break;
	}
}

void CProtocol::Send(const SM17Frame &frame, const CIp &Ip) const
{
	switch (Ip.GetFamily())
	{
	case AF_INET:
		m_Socket4.Send(frame.magic, sizeof(SM17Frame), Ip);
		break;
	case AF_INET6:
		m_Socket6.Send(frame.magic, sizeof(SM17Frame), Ip);
		break;
	default:
		std::cerr << "WrongFamily: " << Ip.GetFamily() << std::endl;
		break;
	}
}

#ifdef DEBUG
void CProtocol::Dump(const char *title, const uint8_t *data, int length)
{
	std::cout << title << std::endl;

	unsigned int offset = 0U;

	while (length > 0) {

		unsigned int bytes = (length > 16) ? 16U : length;

		for (unsigned i = 0U; i < bytes; i++) {
			if (i)
				std::cout << " ";
			std::cout << std::hex << std::setw(2) << std::right << std::setfill('0') << int(data[offset + i]);
		}

		for (unsigned int i = bytes; i < 16U; i++)
			std::cout << "   ";

		std::cout << "   *";

		for (unsigned i = 0U; i < bytes; i++) {
			unsigned char c = data[offset + i];

			if (::isprint(c))
				std::cout << c;
			else
				std::cout << '.';
		}

		std::cout << '*' << std::endl;

		offset += 16U;

		if (length >= 16)
			length -= 16;
		else
			length = 0;
	}
	std::cout << std::dec << std::setfill(' ');
}
#endif

////////////////////////////////////////////////////////////////////////////////////////
// blocking

void CProtocol::ParseBlockProtocols(const std::string &key, const std::string &label)
{
	if (!g_Configure.Contains(key))
		return;
	std::istringstream ss(g_Configure.GetString(key));
	std::string token;
	while (std::getline(ss, token, ','))
	{
		token.erase(0, token.find_first_not_of(" \t"));
		token.erase(token.find_last_not_of(" \t") + 1);
		if (token.empty())
			continue;
		if (const unsigned n = MmdvmClientConnection(token))
		{
			if (n == GetMmdvmConnection())
				continue;	// itself: it never takes its own calls back anyway
			SetMmdvmConnectionBlocked(n);
			std::cout << label << ": blocking " << token << std::endl;
			continue;
		}
		const auto it = ProtocolNames().find(token);
		if (it == ProtocolNames().end())
		{
			std::cerr << label << ": unknown protocol in BlockProtocols: " << token << std::endl;
			continue;
		}
		SetSourceBlocked(it->second);
		std::cout << label << ": blocking protocol " << token << std::endl;
	}
}

bool CProtocol::IsSourceClientBlocked(const CClient &client) const
{
	const CProtocol *owner = client.GetOwnerProtocol();
	return owner && owner->GetMmdvmConnection() && IsMmdvmConnectionBlocked(owner->GetMmdvmConnection());
}
