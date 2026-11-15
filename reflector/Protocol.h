#pragma once

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

#include "UDPSocket.h"
#include "PacketStream.h"
#include "DVHeaderPacket.h"
#include "DVFramePacket.h"
#include <poll.h>
#include <set>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <chrono>
#include <vector>
#include <cstring>
#include <shared_mutex>

#include "WakeFd.h"

////////////////////////////////////////////////////////////////////////////////////////

// DMR defines
// slot n'
#define DMR_SLOT1                       1
#define DMR_SLOT2                       2
// call type
#define DMR_GROUP_CALL                  0
#define DMR_PRIVATE_CALL                1
// frame type
#define DMR_FRAMETYPE_VOICE             0
#define DMR_FRAMETYPE_VOICESYNC         1
#define DMR_FRAMETYPE_DATA              2
#define DMR_FRAMETYPE_DATASYNC          3
// data type
#define DMR_DT_VOICE_PI_HEADER          0
#define DMR_DT_VOICE_LC_HEADER          1
#define DMR_DT_TERMINATOR_WITH_LC       2
#define DMR_DT_CSBK                     3
#define DMR_DT_DATA_HEADER              6
#define DMR_DT_RATE_12_DATA             7
#define DMR_DT_RATE_34_DATA             8
#define DMR_DT_IDLE                     9
#define DMR_DT_RATE_1_DATA              10
// CRC masks
#define DMR_VOICE_LC_HEADER_CRC_MASK    0x96
#define DMR_TERMINATOR_WITH_LC_CRC_MASK 0x99
#define DMR_PI_HEADER_CRC_MASK          0x69
#define DMR_DATA_HEADER_CRC_MASK        0xCC
#define DMR_CSBK_CRC_MASK               0xA5


////////////////////////////////////////////////////////////////////////////////////////
// class

class CProtocol
{
public:
	// constructor
	CProtocol();

	// destructor
	virtual ~CProtocol();

	// initialization
	virtual bool Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6);
	virtual void Close(void);
	// ends the task thread: out of its wait at once, then joined
	void StopTask(void);

	// get
	const CCallsign &GetReflectorCallsign(void)const { return m_ReflectorCallsign; }
	uint16_t GetPort(void) const { return m_Port; }

	// task
	void Thread(void);
	virtual void Task(void) = 0;

	// pass-through
	void Push(std::unique_ptr<CPacket> p);
	// wakes the task's wait, for work another thread left it (Push does it itself)
	void Wake(void);

	// protocol blocking (bidirectional, thread-safe for runtime changes)
	bool IsSourceBlocked(EProtocol src) const { std::shared_lock lk(m_BlockMutex); return m_BlockedSources.count(src) > 0; }
	// a call from one MMDVMClient connection blocked here by its name (MMDVMClient1 ... 9),
	// on top of the type; an MMDVMClient adds its rules for other connections
	virtual bool IsSourceClientBlocked(const CClient &client) const;
	void SetSourceBlocked(EProtocol src)      { std::unique_lock lk(m_BlockMutex); m_BlockedSources.insert(src); }
	void ClearSourceBlocked(EProtocol src)    { std::unique_lock lk(m_BlockMutex); m_BlockedSources.erase(src); }
	std::set<EProtocol> GetBlockedSources() const { std::shared_lock lk(m_BlockMutex); return m_BlockedSources; }
	// one MMDVMClient connection (1 to 9) as a blocked source
	bool IsMmdvmConnectionBlocked(unsigned n) const { std::shared_lock lk(m_BlockMutex); return m_BlockedMmdvm.count(n) > 0; }
	void SetMmdvmConnectionBlocked(unsigned n)      { std::unique_lock lk(m_BlockMutex); m_BlockedMmdvm.insert(n); }
	void ClearMmdvmConnectionBlocked(unsigned n)    { std::unique_lock lk(m_BlockMutex); m_BlockedMmdvm.erase(n); }
	std::set<unsigned> GetBlockedMmdvm() const      { std::shared_lock lk(m_BlockMutex); return m_BlockedMmdvm; }
	void ResetBlocksToDefault()               { std::unique_lock lk(m_BlockMutex); m_BlockedSources = m_ConfigBlockedSources; m_BlockedMmdvm = m_ConfigBlockedMmdvm; }
	void SaveBlockDefaults()                  { std::unique_lock lk(m_BlockMutex); m_ConfigBlockedSources = m_BlockedSources; m_ConfigBlockedMmdvm = m_BlockedMmdvm; }
	// the BlockProtocols list under key: protocol names, "MMDVMClient" for every
	// MMDVMClient connection, "MMDVMClient1" ... "MMDVMClient9" for one of them
	void ParseBlockProtocols(const std::string &key, const std::string &label);
	// the MMDVMClient connection this is (1 to 9), 0 for every other protocol
	virtual unsigned GetMmdvmConnection(void) const { return 0; }
	EProtocol GetProtocolType(void) const { return m_ProtocolType; }

protected:
	// stream helpers
	virtual void OnDvFramePacketIn(std::unique_ptr<CDvFramePacket> &, const CIp * = nullptr);

	// stream handle helpers
	// the stream with this id owned by Ip (nullptr: none found); there is no
	// default, a call without an owner found nothing ever
	std::shared_ptr<CPacketStream> GetStream(uint16_t, const CIp *);
	void CheckStreamsTimeout(void);
	void CloseStreamsOf(const CIp &);
	using StreamMap = std::unordered_map<uint16_t, std::shared_ptr<CPacketStream>>;
	StreamMap::iterator CloseStreamAt(StreamMap::iterator it);
	// a stream left m_Streams: what a protocol keeps per stream id goes with it
	virtual void StreamClosed(uint16_t streamid);
	// stream ids for peers whose packets carry none (DMR+, YSF, NXDN, P25, USRP),
	// one per transmission as with the other protocols (the transcoder's answers
	// and reports, the last heard and loop checks tell them apart): the one of the
	// peer's open stream, 0 for none; a new one for a transmission that opens,
	// the same until a stream of this protocol closes (a talker refused again and
	// again is logged once); and the open one, else a new one
	uint16_t OpenStreamIdOf(const CIp &ip) const { uint16_t sid = 0; OpenStreamOf(ip, &sid); return sid; }
	// the stream the peer at ip owns (one at a time: OpenStream makes its client the master)
	std::shared_ptr<CPacketStream> OpenStreamOf(const CIp &ip, uint16_t *sid = nullptr) const;
	uint16_t NewStreamId(const CIp &ip);
	// a protocol that counts its own ids takes the next one not taken (next: the
	// next candidate); after 256 tries the last one, all of them taken
	bool StreamIdTaken(uint16_t id) const;
	// open here, or closed lately (its end may still be on its way out)
	bool StreamIdLive(uint16_t id) const;
	template <typename F> uint16_t FreeStreamId(F next) const
	{
		uint16_t id = next();
		for (int n = 0; n < 255 && StreamIdTaken(id); n++)
			id = next();
		return id;
	}
	uint16_t PeerStreamId(const CIp &ip) { const uint16_t sid = OpenStreamIdOf(ip); return sid ? sid : NewStreamId(ip); }
	uint16_t m_StreamIdSeed = 0;
	// once per stream id and peer: whether this id of this peer is new to the log
	// of kind (an orphan, a late entry), for the protocol's thread alone
	bool LogOnce(int kind, const CIp &ip, uint16_t id);
	std::unordered_map<uint64_t, uint16_t> m_Logged;
	// streams whose last packet was pushed (P25 TDU, USRP unkey): what comes after
	// it until the stream closes is dropped
	std::unordered_set<uint16_t> m_EndedStreams;

	// queue helper
	virtual void HandleQueue(void) = 0;

	// keepalive helpers
	virtual void HandleKeepalives(void) = 0;

	// syntax helper
	bool IsNumber(char) const;
	bool IsLetter(char) const;
	bool IsSpace(char) const;

	// a 16 bit value of a packet as it is stored there (e.g. a D-Star stream id),
	// at any offset: no unaligned load
	static uint16_t ReadU16(const uint8_t *p) { uint16_t v; memcpy(&v, p, sizeof(v)); return v; }

	// D-Star connectors: the remote reflector with a module for RPT2 of what is sent
	// to it ("XRF001 C"), as a gateway does: the callsign it announced, else the
	// first label of its host name when that is a callsign, else our own
	static CCallsign RemoteReflectorCallsign(const std::string &announced, const std::string &host, const CCallsign &own, char module);

	// dmr DstId to Module helper
	virtual char DmrDstIdToModule(uint32_t) const;
	virtual uint32_t ModuleToDmrDestId(char) const;

	// how long the task may wait for a packet, in ms (PROTOCOL_TICK, PROTOCOL_IDLE_WAIT)
	virtual int WaitTime(void) const;
	bool Wait(int fd4, int fd6, int time_ms, bool &ready4, bool &ready6);
	// waits for any of fds (POLLIN and the like, their revents set) or the queue,
	// at most time_ms; true when one of fds is ready
	bool WaitFor(std::vector<struct pollfd> &fds, int time_ms);
	bool Receive6(CBuffer &buf, CIp &Ip, int time_ms);
	bool Receive4(CBuffer &buf, CIp &Ip, int time_ms);
	bool ReceiveDS(CBuffer &buf, CIp &Ip, int time_ms);

	void Send(const CBuffer &buf, const CIp &Ip) const;
	void Send(const char    *buf, const CIp &Ip) const;
	void Send(const CBuffer &buf, const CIp &Ip, uint16_t port) const;
	void Send(const char    *buf, const CIp &Ip, uint16_t port) const;
	void Send(const SM17Frame &frame, const CIp &Ip) const;
#ifdef DEBUG
	void Dump(const char *title, const uint8_t *data, int length);
#endif

	// socket
	CUdpSocket m_Socket4;
	CUdpSocket m_Socket6;

	// streams
	StreamMap m_Streams;

	// queue, and what wakes the receive wait when it got a packet
	CSafePacketQueue<std::unique_ptr<CPacket>> m_Queue;
	CWakeFd m_QueueWake;

	// thread
	std::atomic<bool> keep_running;
	std::future<void> m_Future;

	// identity
	CCallsign       m_ReflectorCallsign;

	// protocol type and blocking
	EProtocol m_ProtocolType = EProtocol::none;
	mutable std::shared_mutex m_BlockMutex;
	std::set<EProtocol> m_BlockedSources;
	std::set<EProtocol> m_ConfigBlockedSources;  // startup defaults for reset
	std::set<unsigned> m_BlockedMmdvm, m_ConfigBlockedMmdvm;	// single MMDVMClient connections

	// data
	uint16_t m_Port;
	// debug
	CTimer      m_DebugTimer;

private:
	int PollWithQueue(struct pollfd *fds, nfds_t n, int time_ms);
};
