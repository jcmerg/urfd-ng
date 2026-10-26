// urfd -- The universal reflector
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

#pragma once

#include "Defines.h"
#include "Timer.h"
#include "Protocol.h"
#include "DVHeaderPacket.h"
#include "DVFramePacket.h"
#include "Timer.h"
#include "Clients.h"
#include <unordered_set>

// class

class CUSRPStreamCacheItem
{
public:
	CUSRPStreamCacheItem() : m_iSeqCounter(0) {}

	CDvHeaderPacket m_dvHeader;
	uint32_t        m_iSeqCounter;
};

class CUSRPProtocol : public CProtocol
{
public:
	// initialization
	bool Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6);

	// task
	void Task(void);

protected:
	// queue helper
	void HandleQueue(void);
	void HandleKeepalives(void);

	// stream helpers
	void OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &, const CIp &);
	uint16_t GetStreamIdFromIp(const CIp &);
	uint16_t OpenStream(const CCallsign &, const CIp &);

	// packet decoding helpers
	bool IsValidDvPacket(const CBuffer &, uint16_t, std::unique_ptr<CDvFramePacket> &);
	bool IsValidDvHeaderPacket(const CBuffer &, CCallsign &);
	bool IsValidDvLastPacket(const CBuffer &);

	// packet encoding helpers
	void EncodeUSRPHeaderPacket(const CDvHeaderPacket &, uint32_t, CBuffer &) const;
	void EncodeUSRPPacket(const CDvHeaderPacket &, const CDvFramePacket &, uint32_t, CBuffer &Buffer, bool) const;

	// for keep alive
	CTimer m_LastKeepaliveTime;

	// for queue header caches
	std::unordered_map<char, CUSRPStreamCacheItem> m_StreamsCache;
	// streams whose unkey was pushed: voice after it until the stream closes is dropped
	std::unordered_set<uint16_t> m_EndedStreams;
	// the talker a TEXT/SET_INFO packet announced, until the unkey
	CCallsign m_Talker;
	bool m_HasTalker = false;

private:
	// CConfigure data
	CCallsign m_Callsign;
	char m_Module;
	uint16_t m_txPort;
	int32_t m_RxGainNum = 256;   // [USRP] RxGain / TxGain, 256 = 0 dB
	int32_t m_TxGainNum = 256;
};
