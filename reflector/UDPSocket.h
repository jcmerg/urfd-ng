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

#include <cstdint>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <mutex>
#include <string>
#include <unordered_map>

#include "IP.h"
#include "Buffer.h"

#define UDP_BUFFER_LENMAX       1024

class CUdpSocket
{
public:
	// constructor
	CUdpSocket();

	// destructor
	~CUdpSocket();

	// open & close
	bool Open(const CIp &Ip);
	void Close(void);
	int  GetSocket(void) const
	{
		return m_fd;
	}

	// read
	bool Receive(CBuffer &, CIp &, int);
	bool ReceiveFrom(CBuffer &buf, CIp &ip);

	// write
	void Send(const CBuffer &, const CIp &) const;
	void Send(const char    *, const CIp &) const;
	void Send(const CBuffer &, const CIp &, uint16_t) const;
	void Send(const char    *, const CIp &, uint16_t) const;
	void Send(const u_int8_t *, size_t size, const CIp &) const;

	// the source address for a peer that has not sent us anything yet (a link
	// we open), when the socket listens on the wildcard address: the address
	// the reflector's name points to. Without it the kernel picks one, on IPv6
	// often a temporary or SLAAC address that the far side does not know
	void SetDefaultSource(const CIp &);
	// the source for one peer before it sent anything (the address a TCP
	// connection to the same host leaves from)
	void SetSourceFor(const CIp &peer, const CIp &local);
	// the address a TCP connection to a peer of this socket should leave
	// from, so both use the same one: the bound address, else the default
	// source (unset: the kernel's pick)
	CIp GetSourceAddress(void) const;

protected:
	void SendTo(const void *data, size_t size, const CIp &Ip) const;
	void Remember(const CIp &peer, const void *local, size_t len);

	// data
	int m_fd;
	CIp m_addr;
	bool m_Wildcard = false;	// bound to 0.0.0.0 or ::: replies need their source set

	// the local address each peer sent to, the source of our answers to it, so
	// a client hears us from the address it called on a host with several
	// (a fixed one, SLAAC and privacy addresses, secondary IPv4 addresses)
	mutable std::mutex m_LocalMutex;
	mutable std::unordered_map<std::string, std::string> m_Local;
	mutable std::string m_DefaultLocal;
};
