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

#pragma once

#include <string>
#include <cstdint>
#include <mutex>
#include <vector>
#include <functional>
#include <queue>
#include <memory>
#include <chrono>
#include <poll.h>

#include "IP.h"
#include "TCPacketDef.h"

class CTCSocket
{
public:
	CTCSocket() {}
	virtual ~CTCSocket() { Close(); }

	virtual bool Open(const std::string &address, const std::string &modules, uint16_t port) = 0;
	void Close(); // close all open sockets
	void Close(char module); // close a specific module
	void Close(int fd); // close a specific file descriptor

	// All bool functions, except Server Receive, return true if there was an error
	bool Send(const STCPacket *packet);

	int GetFD(char module) const; // can return -1!
	char GetMod(int fd) const;
	const std::string &GetModules(void) const { return m_Modules; }
	bool IsModuleConnected(char module);
	// shared secret of the transcoder link, empty for none; set it before Open()
	void SetPassword(const std::string &password) { m_Password = password; }

protected:
	bool receive(int fd, STCPacket *packet);
	std::vector<struct pollfd> m_Pfd;
	std::string m_Modules;
	std::string m_Password;
};

class CTCServer : public CTCSocket
{
public:
	CTCServer() : CTCSocket(), m_ListenFd(-1) {}
	~CTCServer() { for (auto &p : m_Pending) close(p.fd); if (m_ListenFd >= 0) close(m_ListenFd); }
	bool Open(const std::string &address, const std::string &modules, uint16_t port);
	// Returns true if there is data
	bool Receive(char module, STCPacket *packet, int ms);
	bool ReceiveNoPoll(char module, STCPacket *packet);
	bool AnyAreClosed();
	// Non-blocking: poll listen socket for ms and accept any pending connections
	void TryAccept(int ms = 100);
	// which of the modules a tcd may connect for now (urfd: [Transcoder] Modules, reloadable);
	// without a filter every module given to Open
	void SetAcceptFilter(std::function<bool(char)> filter) { m_Accept = filter; }

private:
	// a new connection in its handshake, see TryAccept()
	struct SPending
	{
		int fd;
		CIp peer;
		char module;        // 0 until the module byte is in
		uint8_t nonce[32];
		uint8_t answer[32];
		unsigned got;       // bytes of the answer so far
		std::chrono::steady_clock::time_point deadline;
	};

	CIp m_Ip;
	int m_ListenFd;
	std::function<bool(char)> m_Accept;
	std::vector<SPending> m_Pending;
	void acceptnew();
	bool advance(SPending &p);
	void establish(SPending &p);
};

class CTCClient : public CTCSocket
{
public:
	CTCClient() : CTCSocket(), m_Port(0) {}
	~CTCClient() {}
	bool Open(const std::string &address, const std::string &modules, uint16_t port);
	void Receive(std::queue<std::unique_ptr<STCPacket>> &queue, int ms);
	void ReConnect();

private:
	std::string m_Address;
	uint16_t m_Port;
	bool Connect(char module, bool blocking = false);
	bool authenticate(int fd, char module);
};
