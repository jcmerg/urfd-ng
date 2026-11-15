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

#include <atomic>
#include <future>
#include <mutex>
#include <array>

#include "DVFramePacket.h"
#include "SafePacketQueue.h"
#include "DStarSlowData.h"
#include "WakeFd.h"

////////////////////////////////////////////////////////////////////////////////////////
// class

class CPacketStream;

class CCodecStream
{
public:
	// constructor
	CCodecStream(CPacketStream *packetstream, char module);
	bool InitCodecStream();

	void ResetStats(uint16_t streamid, ECodecType codectype);

	// destructor
	virtual ~CCodecStream();
	// ends the thread and waits for it; the destructor does it too, but that
	// runs only when g_Reflector goes, after g_TCServer the thread polls
	void Stop(void);

	// get
	bool     IsOpen(void) const               { return m_IsOpen; }
	char     GetModule(void) const            { return m_CSModule; }

	// update slow data TG after source TG is known
	void     UpdateSlowDataTG(uint32_t tg);

	// stats accessors (for admin interface)
	// one stream's, taken together under the lock (a stream opening resets them)
	struct SStats
	{
		ECodecType codec_in;
		double rt_min, rt_max, rt_avg;
		unsigned int rt_count;
		uint32_t total_packets, mismatch_count;
	};
	SStats   GetStats(void) const;
	// the transmission ends: its stats, for PrintStats after the stream is free
	SStats   CloseStats(void);
	static void PrintStats(const SStats &st);
	bool     IsPipelineEmpty(void);

	// task
	void Thread(void);
	void Task(void);

	// pass-through (wakes up Task)
	void Push(std::unique_ptr<CDvFramePacket> p);

protected:
	// Task's wait ends
	void Wake(void);
	// connection: the tcd connection it came on (CTCSocket::GetConnectionCount)
	void Answer(const STCPacket &pack, uint32_t connection);
	void answerLocked(const STCPacket &pack, uint32_t connection, bool &skipped, bool &mismatch);
	SStats statsLocked(void) const;
	void DrainQueues(void);
	// the stream's state that a protocol thread resets (ResetStats) while the codec
	// thread sends and takes answers: m_LocalQueue's order, the stream id, the counters
	mutable std::mutex m_StreamMutex;

	// identity
	const char      m_CSModule;
	// state
	std::atomic<bool> m_IsOpen;
	// data
	ECodecType      m_eCodecIn;

	// associated packet stream
	CPacketStream  *m_PacketStream;

	// queues
	// m_Queue: to be sent, with the transmission it was pushed in (a frame left
	// over when the next one opens is not sent as that one's); m_LocalQueue: sent,
	// waiting for its answer
	struct SQueued { std::unique_ptr<CDvFramePacket> frame; uint32_t generation; };
	CSafePacketQueue<SQueued> m_Queue;
	// with the connection it went out on (CTCSocket::GetConnectionCount)
	struct SSent { std::unique_ptr<CDvFramePacket> frame; uint32_t connection; };
	CSafePacketQueue<SSent> m_LocalQueue;

	// thread
	std::atomic<bool> keep_running;
	std::future<void> m_Future;
	std::atomic<bool> m_Waiting { false };	// Task is about to wait or waits: Push wakes it
	std::atomic<const CWakeFd *> m_Wake { nullptr };	// g_TCServer's while the thread runs; nullptr for none

	// D-Star slow data for transcoded streams
	CDStarSlowData m_SlowData;
	uint32_t     m_uiSuperframeCount;
	std::mutex   m_SlowDataMutex;  // the slow data below: the protocol thread sets the TG (UpdateSlowDataTG), the codec thread sends them
	std::string  m_SlowDataMsg1;   // "Max TG26363", what fits in 20 characters
	std::string  m_SlowDataMsg2;   // the part that did not fit, in turn with Msg1; empty when all fits
	std::string  m_SlowDataName;   // the operator's first name from the ID databases, in ASCII
	std::string  m_SlowDataVia;    // the source: "DG15", "RAN6", or "DMR+" for a caller with a suffix of their own; empty when the suffix says it all
	bool         m_SlowDataOwner = false; // the stream has an owner, so a TG can follow
	uint32_t     m_SlowDataTG = 0; // the source TG, known once the stream runs
	void ComposeSlowData(void);
	static std::string AsciiName(const std::string &utf8);

	// statistics
	double       m_RTMin;
	double       m_RTMax;
	double       m_RTSum;
	unsigned int m_RTCount;
	uint32_t     m_uiTotalPackets;
	uint32_t     m_uiMismatchCount;
	uint32_t     m_uiSkipCount;		// frames of the stream tcd never answered
	// under m_StreamMutex (ResetStats): the transmission's number, its stream id
	// and the previous ones', whose late answers are dropped without a mismatch
	std::atomic<uint32_t> m_uiGeneration { 0 };	// written under the lock, read by Push without it
	std::atomic<uint32_t> m_uiPushedGeneration { 0 };	// of the last frame pushed
	uint16_t     m_uiStreamId = 0;
	std::array<uint16_t, 4> m_PrevStreamIds {};	// a few, for kerchunks in a row
	size_t       m_uiPrevNext = 0;
};
