// urfd -- The universal reflector
// Copyright © 2024 Thomas A. Early N7TAE
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

#include <iostream>
#include <sstream>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <csignal>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <cctype>
#include <cstring>
#include <fstream>

#include "TCSocket.h"
#include "RejectLog.h"
#include "SHA256.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Authentication of a transcoder connection, when both sides have a Password
// (urfd 4.16.0, tcd 0.8.0): after the module byte the reflector sends the magic
// "TCA1" and a 32 byte random challenge, tcd answers with
// HMAC-SHA256(Password, challenge + module byte), and the reflector confirms
// with 'A' or closes the connection. Only an authenticated connection replaces
// the one a module already has.
#define TC_AUTH_MAGIC     "TCA1"
#define TC_AUTH_MAGIC_LEN 4
#define TC_AUTH_NONCE_LEN 32
#define TC_AUTH_OK        'A'
#define TC_PENDING_MAX     8	// connections in their handshake at a time
#define TC_PENDING_TIMEOUT 2	// seconds for the whole handshake

static void hmac_sha256(const std::string &key, const uint8_t *msg, size_t len, uint8_t *out)
{
	CSHA256 sha;
	uint8_t k[64] = { 0 };
	if (key.size() > sizeof(k))
		sha.buffer((const uint8_t *)key.data(), key.size(), k);
	else
		memcpy(k, key.data(), key.size());

	std::vector<uint8_t> inner(64 + len), outer(64 + 32);
	for (unsigned i = 0; i < 64; i++)
	{
		inner[i] = k[i] ^ 0x36;
		outer[i] = k[i] ^ 0x5c;
	}
	memcpy(inner.data() + 64, msg, len);
	sha.buffer(inner.data(), inner.size(), outer.data() + 64);
	sha.buffer(outer.data(), outer.size(), out);
}

static void auth_digest(const std::string &password, const uint8_t *nonce, char module, uint8_t *out)
{
	uint8_t msg[TC_AUTH_NONCE_LEN + 1];
	memcpy(msg, nonce, TC_AUTH_NONCE_LEN);
	msg[TC_AUTH_NONCE_LEN] = (uint8_t)module;
	hmac_sha256(password, msg, sizeof(msg), out);
}

void CTCSocket::Close()
{
	for (auto &item : m_Pfd)
	{
		if (item.fd >= 0)
		{
			Close(item.fd);
		}
	}
	// the entries stay, each now -1: codec threads still running at shutdown look
	// them up by module and must find a closed socket, not a vector emptied under them
}

void CTCSocket::Close(char mod)
{
	auto pos = m_Modules.find(mod);
	if (std::string::npos == pos)
	{
		std::cerr << "Could not find module '" << mod << "'" << std::endl;
		return;
	}
	if (m_Pfd[pos].fd < 0)
	{
		std::cerr << "Close(" << mod << ") is already closed" << std::endl;
		return;
	}
	Close(m_Pfd[pos].fd);
	m_Pfd[pos].fd = -1;
}

void CTCSocket::Close(int fd)
{
	if (fd < 0)
		return;

	for (auto &p : m_Pfd)
	{
		if (fd == p.fd)
		{
			shutdown(p.fd, SHUT_RDWR); // may fail if already disconnected, that's OK
			close(p.fd);
			p.fd = -1;
			return;
		}
	}
}

bool CTCSocket::IsModuleConnected(char module)
{
	auto pos = m_Modules.find(module);
	if (std::string::npos == pos || m_Pfd[pos].fd < 0)
		return false;

	// Quick non-blocking poll to detect dead sockets (POLLHUP/POLLERR from keepalive)
	struct pollfd pfd = { m_Pfd[pos].fd, POLLIN, 0 };
	int rv = poll(&pfd, 1, 0);
	if (rv > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
	{
		std::cerr << "Transcoder module '" << module << "' connection lost" << std::endl;
		Close(m_Pfd[pos].fd);
		return false;
	}
	return true;
}

int CTCSocket::GetFD(char module) const
{
	auto pos = m_Modules.find(module);
	if (std::string::npos == pos)
		return -1;
	return m_Pfd[pos].fd;
}

char CTCSocket::GetMod(int fd) const
{
	for (unsigned i=0; i<m_Pfd.size(); i++)
	{
		if (fd == m_Pfd[i].fd)
		{
			return m_Modules[i];
		}
	}
	return '?';
}

bool CTCServer::AnyAreClosed()
{
	for (unsigned i = 0; i < m_Pfd.size(); i++)
	{
		if (m_Pfd[i].fd < 0)
			return true;

		// Actively probe for dead connections (e.g. half-open after network outage)
		struct pollfd pfd = { m_Pfd[i].fd, POLLIN, 0 };
		int rv = poll(&pfd, 1, 0);
		if (rv > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
		{
			std::cerr << "Transcoder module '" << m_Modules[i] << "' connection lost (maintenance probe)" << std::endl;
			Close(m_Pfd[i].fd);
			return true;
		}
	}
	return false;
}

bool CTCSocket::Send(const STCPacket *packet)
{
	auto pos = m_Modules.find(packet->module);
	if (pos == std::string::npos)
	{
		if(packet->codec_in == ECodecType::ping)
		{
			pos = 0; // There is at least one transcoding module, use it to send the ping
		}
		else
		{
			std::cerr << "Can't Send() this packet to unconfigured module '" << packet->module << "'" << std::endl;
			return true;
		}
	}
	unsigned count = 0;
	auto data = (const unsigned char *)packet;
	do {
		auto n = send(m_Pfd[pos].fd, data+count, sizeof(STCPacket)-count, 0);
		if (n <= 0)
		{
			if (0 == n)
			{
				std::cerr << "CTCSocket::Send: socket on module '" << packet->module << "' has been closed!" << std::endl;
			}
			else
			{
				perror("CTCSocket::Send");
			}
			Close(packet->module);
			return true;
		}
		count += n;
	} while (count < sizeof(STCPacket));
	return false;
}

bool CTCSocket::receive(int fd, STCPacket *packet)
{
	auto n = recv(fd, packet, sizeof(STCPacket), MSG_WAITALL);
	if (n < 0)
	{
		return true;
	}

	if (0 == n)
	{
		return true;
	}

	if (n != sizeof(STCPacket))
		std::cout << "receive() only read " << n << " bytes of the transcoder packet from module '" << GetMod(fd) << "'" << std::endl;
	return false;
}

// returns true if there is data to return
bool CTCServer::Receive(char module, STCPacket *packet, int ms)
{
	bool rv = false;
	const auto pos = m_Modules.find(module);
	if (pos == std::string::npos)
	{
		std::cerr << "Can't receive on unconfigured module '" << module << "'" << std::endl;
		return rv;
	}

	auto pfds = &m_Pfd[pos];
	if (pfds->fd < 0)
	{
		return rv;
	}

	auto n = poll(pfds, 1, ms);
	if (n < 0)
	{
		perror("Recieve poll");
		Close(pfds->fd);
		return rv;
	}

	if (0 == n)
		return rv;	// timeout

	if (pfds->revents & POLLIN)
	{
		rv = receive(pfds->fd, packet);
	}

	// It's possible that even if we read the data, the socket can have an error after the read...
	// So we'll check...
	if (pfds->revents & POLLERR || pfds->revents & POLLHUP)
	{
		if (pfds->revents & POLLERR)
			std::cerr << "POLLERR received on module '" << module << "', closing socket" << std::endl;
		if (pfds->revents & POLLHUP)
			std::cerr << "POLLHUP received on module '" << module << "', closing socket" << std::endl;
		Close(pfds->fd);
	}
	if (pfds->revents & POLLNVAL)
	{
		// the descriptor is no longer open (closed at shutdown): forget it, or the
		// codec thread polls it again at once, forever
		std::cerr << "POLLNVAL received on module '" << module << "', dropping the socket" << std::endl;
		pfds->fd = -1;
	}

	if (rv)
		Close(pfds->fd);

	// only an error or hangup, nothing read: there is no packet
	if (!(pfds->revents & POLLIN))
		return false;

	if(packet->codec_in == ECodecType::ping)
		return false; 
	else 
		return !rv;
}

bool CTCServer::ReceiveNoPoll(char module, STCPacket *packet)
{
	const auto pos = m_Modules.find(module);
	if (pos == std::string::npos || m_Pfd[pos].fd < 0)
		return false;

	int fd = m_Pfd[pos].fd;
	if (receive(fd, packet))
	{
		Close(fd);
		return false;
	}
	if (packet->codec_in == ECodecType::ping)
		return false;
	return true;
}

bool CTCServer::Open(const std::string &address, const std::string &modules, uint16_t port)
{
	m_Modules.assign(modules);

	m_Ip = CIp(address.c_str(), AF_UNSPEC, SOCK_STREAM, port);

	m_Pfd.resize(m_Modules.size());
	for (auto &pf : m_Pfd)
	{
		pf.fd = -1;
		pf.events = POLLIN;
		pf.revents = 0;
	}

	// Create persistent listen socket
	m_ListenFd = socket(m_Ip.GetFamily(), SOCK_STREAM, 0);
	if (m_ListenFd < 0)
	{
		perror("Open socket");
		return true;
	}

	int yes = 1;
	if (setsockopt(m_ListenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0)
	{
		close(m_ListenFd);
		m_ListenFd = -1;
		perror("Open setsockopt");
		return true;
	}

	if (bind(m_ListenFd, m_Ip.GetCPointer(), m_Ip.GetSize()) < 0)
	{
		close(m_ListenFd);
		m_ListenFd = -1;
		perror("Open bind");
		return true;
	}

	if (listen(m_ListenFd, 3) < 0)
	{
		perror("Open listen");
		close(m_ListenFd);
		m_ListenFd = -1;
		return true;
	}

	// Set listen socket non-blocking for use with TryAccept
	int flags = fcntl(m_ListenFd, F_GETFL, 0);
	fcntl(m_ListenFd, F_SETFL, flags | O_NONBLOCK);

	std::cout << "Waiting at " << m_Ip << " for transcoder connections for modules " << m_Modules << (m_Password.empty() ? "" : ", password required") << "..." << std::endl;

	// Non-blocking: don't wait for TCD here, maintenance thread handles it
	return false;
}

// A new connection waits in m_Pending, non-blocking, until it has sent its module
// byte and, with a Password, answered the challenge. TryAccept() moves each one on
// as far as its data allows and never waits on one, so a peer that sends nothing
// holds up neither this status thread nor the next connection.
void CTCServer::TryAccept(int ms)
{
	if (m_ListenFd < 0)
		return;

	// Probe existing connections for dead sockets first
	AnyAreClosed();

	// Poll the listen socket and the connections in their handshake
	std::vector<struct pollfd> pfds(1 + m_Pending.size());
	pfds[0] = { m_ListenFd, POLLIN, 0 };
	for (unsigned i = 0; i < m_Pending.size(); i++)
		pfds[i + 1] = { m_Pending[i].fd, POLLIN, 0 };
	if (poll(pfds.data(), pfds.size(), ms) < 0)
		return;

	const auto now = std::chrono::steady_clock::now();
	std::vector<SPending> waiting;
	for (unsigned i = 0; i < m_Pending.size(); i++)
	{
		auto &p = m_Pending[i];
		bool done = pfds[i + 1].revents ? advance(p) : false;
		if (! done && now > p.deadline)
		{
			std::cerr << "Transcoder connection from " << p.peer << " dropped: " << (p.module ? "no answer to the password challenge" : "no module byte") << std::endl;
			close(p.fd);
			done = true;
		}
		if (! done)
			waiting.push_back(p);
	}
	m_Pending.swap(waiting);

	if (pfds[0].revents & POLLIN)
		acceptnew();
}

void CTCServer::acceptnew()
{
	// a bounded number per call, so a flood of connections can't keep the status thread here
	for (unsigned i = 0; i < TC_PENDING_MAX; i++)
	{
		SPending p {};
		socklen_t sin_size = sizeof(struct sockaddr_storage);
		p.fd = accept(m_ListenFd, p.peer.GetPointer(), &sin_size);
		if (p.fd < 0)
		{
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				perror("Accept accept");
			return;
		}

		// non-blocking for the handshake; BSD hands the listen socket's O_NONBLOCK on, Linux does not
		fcntl(p.fd, F_SETFL, fcntl(p.fd, F_GETFL, 0) | O_NONBLOCK);
		p.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(TC_PENDING_TIMEOUT);

		// the oldest makes room: a tcd answers within a round trip, a flood would have to
		// outpace that to push it out
		if (m_Pending.size() >= TC_PENDING_MAX)
		{
			std::cerr << "Too many transcoder connections in their handshake, dropping the one from " << m_Pending.front().peer << std::endl;
			close(m_Pending.front().fd);
			m_Pending.erase(m_Pending.begin());
		}
		m_Pending.push_back(p);
	}
}

// returns true when the connection has left the handshake, established or closed
bool CTCServer::advance(SPending &p)
{
	if (0 == p.module)
	{
		char mod;
		auto n = recv(p.fd, &mod, 1, 0);
		if (n < 0 && (EAGAIN == errno || EWOULDBLOCK == errno))
			return false;
		if (n != 1)
		{
			if (RejectLog("Transcoder: refused connections", p.peer.GetAddress()))
				std::cerr << "Transcoder connection from " << p.peer << " closed without a module byte" << std::endl;
			close(p.fd);
			return true;
		}
		if (0 == mod || std::string::npos == m_Modules.find(mod))
		{
			if (RejectLog("Transcoder: refused connections", p.peer.GetAddress()))
			{
				// scanners send anything: a byte that is not printable as hex, so the log stays one line
				char shown[8];
				if (std::isprint((unsigned char)mod))
					snprintf(shown, sizeof(shown), "'%c'", mod);
				else
					snprintf(shown, sizeof(shown), "0x%02x", (unsigned char)mod);
				std::cerr << "New connection for module " << shown << " from " << p.peer << ", but it's not configured!" << std::endl;
			}
			close(p.fd);
			return true;
		}
		p.module = mod;

		if (m_Password.empty())
		{
			establish(p);
			return true;
		}

		uint8_t challenge[TC_AUTH_MAGIC_LEN + TC_AUTH_NONCE_LEN];
		memcpy(challenge, TC_AUTH_MAGIC, TC_AUTH_MAGIC_LEN);
		std::ifstream urandom("/dev/urandom", std::ios::binary);
		if (! urandom.read((char *)challenge + TC_AUTH_MAGIC_LEN, TC_AUTH_NONCE_LEN))
		{
			std::cerr << "Transcoder authentication: could not read /dev/urandom" << std::endl;
			close(p.fd);
			return true;
		}
		memcpy(p.nonce, challenge + TC_AUTH_MAGIC_LEN, TC_AUTH_NONCE_LEN);
		// nothing was sent on this socket yet, so its buffer takes the 36 bytes at once
		if ((ssize_t)sizeof(challenge) != send(p.fd, challenge, sizeof(challenge), MSG_NOSIGNAL))
		{
			std::cerr << "Transcoder authentication: could not send the challenge for module '" << p.module << "' to " << p.peer << std::endl;
			close(p.fd);
			return true;
		}
		return false;
	}

	auto n = recv(p.fd, p.answer + p.got, sizeof(p.answer) - p.got, 0);
	if (n < 0 && (EAGAIN == errno || EWOULDBLOCK == errno))
		return false;
	if (n <= 0)
	{
		if (RejectLog("Transcoder: refused connections", p.peer.GetAddress()))
			std::cerr << "Transcoder connection for module '" << p.module << "' from " << p.peer << " refused: no answer to the password challenge" << std::endl;
		close(p.fd);
		return true;
	}
	p.got += n;
	if (p.got < sizeof(p.answer))
		return false;

	uint8_t expected[32];
	auth_digest(m_Password, p.nonce, p.module, expected);
	uint8_t diff = 0;
	for (unsigned i = 0; i < sizeof(expected); i++)
		diff |= p.answer[i] ^ expected[i];
	if (diff)
	{
		if (RejectLog("Transcoder: refused connections", p.peer.GetAddress()))
			std::cerr << "Transcoder connection for module '" << p.module << "' from " << p.peer << " refused: wrong password" << std::endl;
		close(p.fd);
		return true;
	}

	const char ok = TC_AUTH_OK;
	if (1 != send(p.fd, &ok, 1, MSG_NOSIGNAL))
	{
		close(p.fd);
		return true;
	}
	establish(p);
	return true;
}

void CTCServer::establish(SPending &p)
{
	const auto pos = m_Modules.find(p.module);

	// blocking again for normal operation
	fcntl(p.fd, F_SETFL, fcntl(p.fd, F_GETFL, 0) & ~O_NONBLOCK);

	// Close old stale connection if present
	if (m_Pfd[pos].fd >= 0)
	{
		std::cerr << "Replacing stale connection on module '" << p.module << "'" << std::endl;
		shutdown(m_Pfd[pos].fd, SHUT_RDWR);
		close(m_Pfd[pos].fd);
	}

	// Enable TCP keepalive to detect dead connections
	int keepalive = 1;
	setsockopt(p.fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
	int idle = 10;      // start probes after 10s idle
	int interval = 5;   // probe every 5s
	int count = 3;      // 3 failed probes = dead
	setsockopt(p.fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	setsockopt(p.fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
	setsockopt(p.fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));

	// Send every frame at once: with Nagle, a frame waits for the ACK of the
	// previous one, which the peer delays until it has data of its own to send
	int nodelay = 1;
	setsockopt(p.fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

	{ std::ostringstream s; s << "File descriptor " << p.fd << " opened TCP port for module '" << p.module << "' on " << p.peer; std::cout << s.str() << std::endl; }

	m_Pfd[pos].fd = p.fd;
}

bool CTCClient::Open(const std::string &address, const std::string &modules, uint16_t port)
{
	m_Address.assign(address);
	m_Modules.assign(modules);
	m_Port = port;

	m_Pfd.resize(m_Modules.size());
	for (auto &pf : m_Pfd)
	{
		pf.fd = -1;
		pf.events = POLLIN;
	}

	std::cout << "Connecting to the TCP server..." << std::endl;

	// a module the reflector does not take now (not in its [Transcoder] Modules,
	// a wrong password, the reflector down or not started yet) is tried again by
	// ReConnect(), so tcd starts completely (monitor included) and keeps running
	std::string missing;
	for (char c : modules)
		if (Connect(c, false) || GetFD(c) < 0)
			missing += c;
	if (!missing.empty())
		std::cerr << "Module(s) " << missing << " not connected yet (reflector not reachable or refused them), tried again every few seconds" << std::endl;
	return false;
}

bool CTCClient::Connect(char module, bool blocking)
{
	const auto pos = m_Modules.find(module);
	if (pos == std::string::npos)
	{
		std::cerr << "CTCClient::Connect: could not find module '" << module << "' in configured modules!" << std::endl;
		return true;
	}
	CIp ip(m_Address.c_str(), AF_UNSPEC, SOCK_STREAM, m_Port);

	auto fd = socket(ip.GetFamily(), SOCK_STREAM, 0);
	if (fd < 0)
	{
		std::cerr << "Could not open socket for module '" << module << "'" << std::endl;
		perror("TC client socket");
		return true;
	}

	int yes = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)))
	{
		std::cerr << "Moudule " << module << " error:";
		perror("setsockopt");
		close(fd);
		return true;
	}

	if (blocking)
	{
		// Initial startup: block until reflector is available
		unsigned count = 0;
		while (connect(fd, ip.GetCPointer(), ip.GetSize()))
		{
			if (ECONNREFUSED == errno)
			{
				if (0 == ++count % 100) std::cout << "Connection refused! Restart the reflector." << std::endl;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			else
			{
				std::cerr << "Module " << module << " error: ";
				perror("connect");
				close(fd);
				return true;
			}
		}
	}
	else
	{
		// Reconnect: non-blocking, return immediately on failure
		if (connect(fd, ip.GetCPointer(), ip.GetSize()))
		{
			if (errno != ECONNREFUSED)
			{
				std::cerr << "Reconnect module " << module << ": ";
				perror("connect");
			}
			close(fd);
			return false;
		}
	}

	int sent = send(fd, &module, 1, 0); // send the identification byte
	if (sent < 0)
	{
		std::cerr << "Error sending ID byte to module '" << module << "':" << std::endl;
		perror("send");
		close(fd);
		return true;
	}
	else if (0 == sent)
	{
		std::cerr << "Could not set ID byte to module '" << module << "'" << std::endl;
		close(fd);
		return true;
	}

	if (! m_Password.empty() && authenticate(fd, module))
	{
		close(fd);
		return true;
	}

	// Enable TCP keepalive to detect dead connections
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
	int idle = 10;      // start probes after 10s idle
	int interval = 5;   // probe every 5s
	int cnt = 3;        // 3 failed probes = dead
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

	// Send every frame at once, see CTCServer::establish
	int nodelay = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

	{ std::ostringstream s; s << "File descriptor " << fd << " on " << ip << " opened for module '" << module << "'"; std::cout << s.str() << std::endl; }

	m_Pfd[pos].fd = fd;

	return false;
}

// returns true if the reflector did not accept the password
bool CTCClient::authenticate(int fd, char module)
{
	struct timeval tv = { 2, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	uint8_t challenge[TC_AUTH_MAGIC_LEN + TC_AUTH_NONCE_LEN];
	auto n = recv(fd, challenge, sizeof(challenge), MSG_WAITALL);
	if (n != (ssize_t)sizeof(challenge) || memcmp(challenge, TC_AUTH_MAGIC, TC_AUTH_MAGIC_LEN))
	{
		std::cerr << "Module " << module << ": the reflector sent no password challenge, is Password set in its [Transcoder] section?" << std::endl;
		return true;
	}

	uint8_t answer[32];
	auth_digest(m_Password, challenge + TC_AUTH_MAGIC_LEN, module, answer);
	if ((ssize_t)sizeof(answer) != send(fd, answer, sizeof(answer), MSG_NOSIGNAL))
	{
		perror("TC client send");
		return true;
	}

	char ok = 0;
	n = recv(fd, &ok, 1, MSG_WAITALL);
	if (n != 1 || ok != TC_AUTH_OK)
	{
		std::cerr << "Module " << module << ": the reflector refused the password, Password in tcd.ini must be the one of its [Transcoder] section" << std::endl;
		return true;
	}

	struct timeval notv = { 0, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &notv, sizeof(notv));
	return false;
}

void CTCClient::ReConnect() // and sometimes ping
{
	static std::chrono::system_clock::time_point start = std::chrono::system_clock::now();
	static std::chrono::steady_clock::time_point last_attempt = {};
	static double backoff_secs = 0.0;
	auto now = std::chrono::system_clock::now();
	std::chrono::duration<double> secs = now - start;

	// Probe all sockets to detect dead connections before checking FDs
	for (char m : m_Modules)
		IsModuleConnected(m);

	// Check if any module needs reconnecting
	bool need_reconnect = false;
	for (char m : m_Modules)
		if (0 > GetFD(m)) { need_reconnect = true; break; }

	if (need_reconnect)
	{
		auto steady_now = std::chrono::steady_clock::now();
		std::chrono::duration<double> since_last = steady_now - last_attempt;

		if (since_last.count() >= backoff_secs)
		{
			for (char m : m_Modules)
			{
				if (0 > GetFD(m))
				{
					std::cout << "Reconnecting module " << m << "..." << std::endl;
					Connect(m, false);
				}
			}
			last_attempt = steady_now;

			// Check if still disconnected → increase backoff (max 5s)
			bool still_down = false;
			for (char m : m_Modules)
				if (0 > GetFD(m)) { still_down = true; break; }

			if (still_down)
				backoff_secs = (backoff_secs < 0.5) ? 0.5 : std::min(backoff_secs * 2.0, 5.0);
			else
				backoff_secs = 0.0;
		}
	}
	else
	{
		backoff_secs = 0.0;
	}

	if(secs.count() > 5.0)
	{
		if (m_Pfd.size() > 0 && m_Pfd[0].fd >= 0)
		{
			STCPacket ping;
			ping.codec_in = ECodecType::ping;
			Send(&ping);
		}
		start = now;
	}
}

void CTCClient::Receive(std::queue<std::unique_ptr<STCPacket>> &queue, int ms)
{
	for (auto &pfd : m_Pfd)
		pfd.revents = 0;

	auto rv = poll(m_Pfd.data(), m_Pfd.size(), ms);

	if (rv < 0)
	{
		perror("Receive poll");
		return;
	}

	if (0 == rv)
		return;

	for (auto &pfd : m_Pfd)
	{
		if (pfd.fd < 0)
			continue;

		if (pfd.revents & POLLIN)
		{
			auto p_tcpack = std::make_unique<STCPacket>();
			if (receive(pfd.fd, p_tcpack.get()))
			{
				p_tcpack.reset();
				Close(pfd.fd);
			}
			else
			{
				queue.push(std::move(p_tcpack));
			}
		}

		if (pfd.revents & POLLERR || pfd.revents & POLLHUP)
		{
			std::cerr << "Connection lost on module " << GetMod(pfd.fd) << ", reconnecting..." << std::endl;
			Close(pfd.fd);
		}
		if (pfd.revents & POLLNVAL)
		{
			pfd.fd = -1;
		}
	}
}
