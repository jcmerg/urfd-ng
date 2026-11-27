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
#include <errno.h>
#include <algorithm>

#include "UDPSocket.h"

// the peer's address without the port: the key of the local address it uses
static std::string PeerKey(const CIp &ip)
{
	const struct sockaddr *sa = ip.GetCPointer();
	if (AF_INET6 == sa->sa_family)
	{
		const auto *a = (const struct sockaddr_in6 *)sa;
		return std::string("6") + std::string((const char *)&a->sin6_addr, sizeof(a->sin6_addr));
	}
	const auto *a = (const struct sockaddr_in *)sa;
	return std::string("4") + std::string((const char *)&a->sin_addr, sizeof(a->sin_addr));
}

// peers we keep the local address of: more come only from spoofed packets or
// a very busy reflector; then they start again, the next packet of each puts
// it back
#define UDP_LOCAL_MAX 4096


////////////////////////////////////////////////////////////////////////////////////////
// constructor

CUdpSocket::CUdpSocket() : m_fd(-1) {}

////////////////////////////////////////////////////////////////////////////////////////
// destructor

CUdpSocket::~CUdpSocket()
{
	Close();
}

////////////////////////////////////////////////////////////////////////////////////////
// open & close

// returns true on error
bool CUdpSocket::Open(const CIp &Ip)
{
	// check for a valid family
	if (AF_UNSPEC == Ip.GetFamily())
		return true;

	// create socket
	m_fd = socket(Ip.GetFamily(), SOCK_DGRAM, 0);
	if ( m_fd < 0 )
	{
		std::cerr << "Unable to open socket on " << Ip << ", " << strerror(errno) << std::endl;
		return false;
	}
	// initialize sockaddr struct
	m_addr = Ip;

	int reuse = 1;
	if ( 0 > setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int)))
	{
		std::cerr << "Cannot set the UDP socket option on " << m_addr << ", " << strerror(errno) << std::endl;
		Close();
		return false;
	}

	// the IPv6 socket for IPv6 alone: IPv4 has a socket of its own on the
	// port, IPv4 clients would show up as ::ffff:a.b.c.d on this one else
	int on = 1;
	if (AF_INET6 == Ip.GetFamily() && 0 > setsockopt(m_fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)))
	{
		std::cerr << "Cannot set IPV6_V6ONLY on " << m_addr << ", " << strerror(errno) << std::endl;
		Close();
		return false;
	}

	// on the wildcard address each packet tells the local address it went to,
	// the source of the answer (Send)
	m_Wildcard = m_addr.AddressIsZero();
	if (m_Wildcard)
	{
		const int rc = (AF_INET6 == Ip.GetFamily())
			? setsockopt(m_fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on))
			: setsockopt(m_fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on));
		if (rc < 0)
		{
			std::cerr << "Cannot ask for the local address of packets on " << m_addr << ", " << strerror(errno) << ", answers leave from the address the kernel picks" << std::endl;
			m_Wildcard = false;
		}
	}

	if (fcntl(m_fd, F_SETFL, O_NONBLOCK))
	{
		std::cerr << "fcntl set non-blocking failed on " << m_addr << ", " << strerror(errno) << std::endl;
		Close();
		return false;
	}

	if ( bind(m_fd, m_addr.GetCPointer(), m_addr.GetSize()) )
	{
		std::cerr << "bind failed on " << m_addr << ", " << strerror(errno) << std::endl;
		Close();
		return false;
	}

	if (0 == m_addr.GetPort())  	// get the assigned port for an ephemeral port request
	{
		CIp a;
		socklen_t len = sizeof(struct sockaddr_storage);
		if (getsockname(m_fd, a.GetPointer(), &len))
		{
			std::cerr << "getsockname error " << m_addr << ", " << strerror(errno) << std::endl;
			Close();
			return false;
		}
		m_addr.SetPort(a.GetPort());
	}

	// done
	return true;
}

void CUdpSocket::Close(void)
{
	if ( m_fd >= 0 )
	{
		close(m_fd);
		m_fd = -1;
	}
	std::lock_guard<std::mutex> lock(m_LocalMutex);
	m_Local.clear();
}

void CUdpSocket::SetDefaultSource(const CIp &ip)
{
	std::lock_guard<std::mutex> lock(m_LocalMutex);
	m_DefaultLocal.clear();
	if (!ip.IsSet() || ip.GetFamily() != m_addr.GetFamily() || ip.AddressIsZero())
		return;
	const std::string key(PeerKey(ip));
	m_DefaultLocal = key.substr(1);
}

CIp CUdpSocket::GetSourceAddress(void) const
{
	if (m_fd < 0)
		return CIp();
	if (!m_Wildcard)
	{
		CIp ip(m_addr);
		ip.SetPort(0);
		return ip;
	}
	std::lock_guard<std::mutex> lock(m_LocalMutex);
	if (m_DefaultLocal.size() < 4)
		return CIp();
	char str[INET6_ADDRSTRLEN];
	if (AF_INET6 == m_addr.GetFamily() && m_DefaultLocal.size() >= 16 && inet_ntop(AF_INET6, m_DefaultLocal.data(), str, sizeof(str)))
		return CIp(AF_INET6, 0, str);
	if (AF_INET == m_addr.GetFamily() && inet_ntop(AF_INET, m_DefaultLocal.data(), str, sizeof(str)))
		return CIp(AF_INET, 0, str);
	return CIp();
}

void CUdpSocket::SetSourceFor(const CIp &peer, const CIp &local)
{
	if (!m_Wildcard || local.GetFamily() != m_addr.GetFamily() || local.AddressIsZero() || peer.GetFamily() != m_addr.GetFamily())
		return;
	const std::string key(PeerKey(local));
	if (AF_INET6 == local.GetFamily())
	{
		struct in6_pktinfo pi;
		memset(&pi, 0, sizeof(pi));
		memcpy(&pi.ipi6_addr, key.data() + 1, sizeof(pi.ipi6_addr));
		Remember(peer, &pi, sizeof(pi));
	}
	else
		Remember(peer, key.data() + 1, key.size() - 1);
}

void CUdpSocket::Remember(const CIp &peer, const void *local, size_t len)
{
	const std::string key(PeerKey(peer));
	std::string value((const char *)local, len);
	std::lock_guard<std::mutex> lock(m_LocalMutex);
	auto it = m_Local.find(key);
	if (it != m_Local.end())
	{
		if (it->second != value)
			it->second = std::move(value);
		return;
	}
	if (m_Local.size() >= UDP_LOCAL_MAX)
		m_Local.clear();
	m_Local.emplace(key, std::move(value));
}

////////////////////////////////////////////////////////////////////////////////////////
// read

bool CUdpSocket::Receive(CBuffer &Buffer, CIp &Ip, int timeout)
{
	// socket valid ?
	if ( 0 > m_fd )
		return false;

	// control socket
	fd_set FdSet;
	FD_ZERO(&FdSet);
	FD_SET(m_fd, &FdSet);
	struct timeval tv;
	tv.tv_sec = timeout / 1000;
	tv.tv_usec = (timeout % 1000) * 1000;

	auto rval = select(m_fd + 1, &FdSet, 0, 0, &tv);
	if (rval > 0)
		return ReceiveFrom(Buffer, Ip);

	if (rval < 0)
		std::cerr << "select error on UPD port " << m_addr << ": " << strerror(errno) << std::endl;

	return false;
}

bool CUdpSocket::ReceiveFrom(CBuffer &Buffer, CIp &ip)
{
	// read
	uint8_t buf[UDP_BUFFER_LENMAX];
	if (!m_Wildcard)
	{
		unsigned int fromsize = sizeof(struct sockaddr_storage);
		auto iRecvLen = recvfrom(m_fd, buf, UDP_BUFFER_LENMAX, 0, ip.GetPointer(), &fromsize);
		if (0 >= iRecvLen)
			return false;
		Buffer.Set(buf, iRecvLen);
		return true;
	}

	// with the local address the packet went to
	struct iovec iov = { buf, UDP_BUFFER_LENMAX };
	union
	{
		struct cmsghdr align;
		uint8_t space[CMSG_SPACE(sizeof(struct in6_pktinfo)) + CMSG_SPACE(sizeof(struct in_pktinfo))];
	} control;
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = ip.GetPointer();
	msg.msg_namelen = sizeof(struct sockaddr_storage);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control.space;
	msg.msg_controllen = sizeof(control.space);
	auto iRecvLen = recvmsg(m_fd, &msg, 0);
	if (0 >= iRecvLen)
		return false;
	Buffer.Set(buf, iRecvLen);

	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
	{
		if (IPPROTO_IPV6 == cm->cmsg_level && IPV6_PKTINFO == cm->cmsg_type)
		{
			struct in6_pktinfo pi;
			memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
			// the interface only for a link-local address, which needs it: a
			// global one leaves on whatever the route says, also after its
			// interface came back with another index (PPPoE, a tunnel)
			if (!IN6_IS_ADDR_LINKLOCAL(&pi.ipi6_addr))
				pi.ipi6_ifindex = 0;
			Remember(ip, &pi, sizeof(pi));
			break;
		}
		if (IPPROTO_IP == cm->cmsg_level && IP_PKTINFO == cm->cmsg_type)
		{
			struct in_pktinfo pi;
			memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
			// the local address to answer from (for a packet to a broadcast
			// address the interface's own, not the broadcast address)
			Remember(ip, &pi.ipi_spec_dst, sizeof(pi.ipi_spec_dst));
			break;
		}
	}
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////
// write

// from the local address the peer sent to (Remember), else the default source,
// else the one the kernel picks
void CUdpSocket::SendTo(const void *data, size_t size, const CIp &Ip) const
{
	std::string local;
	bool fromDefault = false;
	if (m_Wildcard)
	{
		std::lock_guard<std::mutex> lock(m_LocalMutex);
		auto it = m_Local.find(PeerKey(Ip));
		if (it != m_Local.end())
			local = it->second;
		else
		{
			local = m_DefaultLocal;
			fromDefault = true;
		}
	}
	if (local.empty())
	{
		sendto(m_fd, data, size, 0, Ip.GetCPointer(), Ip.GetSize());
		return;
	}

	struct iovec iov = { const_cast<void *>(data), size };
	union
	{
		struct cmsghdr align;
		uint8_t space[CMSG_SPACE(sizeof(struct in6_pktinfo))];
	} control;
	memset(&control, 0, sizeof(control));
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = const_cast<struct sockaddr *>(Ip.GetCPointer());
	msg.msg_namelen = Ip.GetSize();
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control.space;
	struct cmsghdr *cm = (struct cmsghdr *)control.space;
	if (AF_INET6 == Ip.GetFamily())
	{
		struct in6_pktinfo pi;
		memset(&pi, 0, sizeof(pi));
		memcpy(&pi, local.data(), std::min(local.size(), sizeof(pi)));
		msg.msg_controllen = CMSG_SPACE(sizeof(pi));
		cm->cmsg_level = IPPROTO_IPV6;
		cm->cmsg_type = IPV6_PKTINFO;
		cm->cmsg_len = CMSG_LEN(sizeof(pi));
		memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
	}
	else
	{
		struct in_pktinfo pi;
		memset(&pi, 0, sizeof(pi));
		memcpy(&pi.ipi_spec_dst, local.data(), std::min(local.size(), sizeof(pi.ipi_spec_dst)));
		msg.msg_controllen = CMSG_SPACE(sizeof(pi));
		cm->cmsg_level = IPPROTO_IP;
		cm->cmsg_type = IP_PKTINFO;
		cm->cmsg_len = CMSG_LEN(sizeof(pi));
		memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
	}
	if (0 <= sendmsg(m_fd, &msg, 0))
		return;

	// the address is gone (a SLAAC or privacy address that expired, an address
	// taken off the interface) or the way from it (an interface that went, no
	// route): forgotten, the kernel picks one. A full send buffer is no reason
	if (EAGAIN != errno && EWOULDBLOCK != errno && ENOBUFS != errno && EINTR != errno)
	{
		{
			std::lock_guard<std::mutex> lock(m_LocalMutex);
			if (fromDefault)
			{
				// not an address of this host (IPv6External set to another)
				if (m_DefaultLocal == local)
				{
					std::cerr << "The default source address on " << m_addr << " is not one of this host, the kernel picks one" << std::endl;
					m_DefaultLocal.clear();
				}
			}
			else
			{
				auto it = m_Local.find(PeerKey(Ip));
				if (it != m_Local.end() && it->second == local)
					m_Local.erase(it);
			}
		}
		sendto(m_fd, data, size, 0, Ip.GetCPointer(), Ip.GetSize());
	}
}

void CUdpSocket::Send(const CBuffer &Buffer, const CIp &Ip) const
{
	SendTo(Buffer.data(), Buffer.size(), Ip);
}

void CUdpSocket::Send(const char *Buffer, const CIp &Ip) const
{
	SendTo(Buffer, ::strlen(Buffer), Ip);
}

void CUdpSocket::Send(const CBuffer &Buffer, const CIp &Ip, uint16_t destport) const
{
	CIp temp(Ip);
	temp.SetPort(destport);
	SendTo(Buffer.data(), Buffer.size(), temp);
}

void CUdpSocket::Send(const char *Buffer, const CIp &Ip, uint16_t destport) const
{
	CIp temp(Ip);
	temp.SetPort(destport);
	SendTo(Buffer, ::strlen(Buffer), temp);
}

void CUdpSocket::Send(const uint8_t *data, size_t size, const CIp &Ip) const
{
	SendTo(data, size, Ip);
}
