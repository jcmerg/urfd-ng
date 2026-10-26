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
#include <sstream>
#include <iostream>
#include <poll.h>
#include <unistd.h>
#include <sys/eventfd.h>

////////////////////////////////////////////////////////////////////////////////////////
// constructor


CProtocol::CProtocol() : keep_running(true)
{
	m_QueueFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}


////////////////////////////////////////////////////////////////////////////////////////
// destructor

CProtocol::~CProtocol()
{
	// kill threads
	Close();

	// empty queue
	while ( !m_Queue.IsEmpty() )
	{
		m_Queue.Pop();
	}

	if (m_QueueFd >= 0)
		close(m_QueueFd);
}

void CProtocol::Push(std::unique_ptr<CPacket> p)
{
	m_Queue.Push(std::move(p));
	// wake the receive wait, the packet goes out at once
	if (m_QueueFd >= 0)
	{
		const uint64_t one = 1;
		if (write(m_QueueFd, &one, sizeof(one)) < 0) {}
	}
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
	while (keep_running)
	{
		Task();
	}
}

void CProtocol::Close(void)
{
	keep_running = false;
	if ( m_Future.valid() )
	{
		m_Future.get();
	}
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
		// Rate-limit: only log once per stream ID to avoid flooding
		static uint16_t lastOrphanId = 0;
		uint16_t thisId = Frame->GetStreamId();
		if (thisId != lastOrphanId)
		{
			lastOrphanId = thisId;
			std::cout << std::showbase << std::hex;
			std::cout << "Orphaned Frame with ID " << ntohs(thisId) << std::noshowbase << std::dec << " on " << *Ip << std::endl;
		}
		Frame.reset();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// stream handle helpers

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
		// time out ?
		if ( it->second->IsExpired() )
		{
			// yes, close it
			g_Reflector.CloseStream(it->second);
			// and remove it from the m_Streams map
			it = m_Streams.erase(it);
		}
		else
		{
			it++;
		}
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

// Waits up to time_ms for a packet on the sockets (fd < 0: none), and returns
// early, without a packet, when something was pushed into the queue: the Task
// loops handle the queue after the receive, so a packet of the router does not
// wait for the next incoming one (same-protocol listeners got them in pairs)
bool CProtocol::Wait(int fd4, int fd6, int time_ms, bool &ready4, bool &ready6)
{
	ready4 = ready6 = false;
	struct pollfd fds[3];
	nfds_t n = 0;
	int i4 = -1, i6 = -1, iq = -1;
	if (fd4 >= 0) { fds[n] = { fd4, POLLIN, 0 }; i4 = n++; }
	if (fd6 >= 0) { fds[n] = { fd6, POLLIN, 0 }; i6 = n++; }
	if (0 == n)
		return false;
	if (m_QueueFd >= 0) { fds[n] = { m_QueueFd, POLLIN, 0 }; iq = n++; }

	auto rval = poll(fds, n, time_ms);
	if (rval <= 0)
	{
		if (rval < 0 && errno != EINTR)
			std::cerr << "Receive poll error: " << strerror(errno) << std::endl;
		return false;
	}
	if (iq >= 0 && (fds[iq].revents & POLLIN))
	{
		uint64_t count;
		if (read(m_QueueFd, &count, sizeof(count)) < 0) {}
	}
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
