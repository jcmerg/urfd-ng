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
#include <unistd.h>
#include <poll.h>
#include <algorithm>

#include "Global.h"
#include "DVFramePacket.h"
#include "PacketStream.h"
#include "CodecStream.h"
#include "Reflector.h"
#include "DStarSlowData.h"
#include "MMDVMClientProtocol.h"
#include "SvxReflectorProtocol.h"
#include "NXDNProtocol.h"
#include "YSFClientPeer.h"

////////////////////////////////////////////////////////////////////////////////////////
// constructor

CCodecStream::CCodecStream(CPacketStream *PacketStream, char module) : m_CSModule(module), m_IsOpen(false),
	m_eCodecIn(ECodecType::none),
	m_RTMin(-1), m_RTMax(-1), m_RTSum(0), m_RTCount(0),
	m_uiTotalPackets(0), m_uiMismatchCount(0), m_uiSkipCount(0), m_uiSuperframeCount(0)
{
	m_PacketStream = PacketStream;
}

////////////////////////////////////////////////////////////////////////////////////////
// destructor

void CCodecStream::Push(std::unique_ptr<CDvFramePacket> p)
{
	// the transmission it is part of: a frame left over when the next one opens is
	// not sent as that one's. Read without the lock: written by ResetStats on the
	// thread that pushes the new transmission's frames, before it does
	const uint32_t generation = m_uiGeneration;
	m_uiPushedGeneration = generation;
	m_Queue.Push({ std::move(p), generation });
	// only a thread that waits needs it (else it sends this one in its round): it
	// looks at the queue once more after it says so, so either sees the other
	if (m_Waiting)
		Wake();
}

// the module's wake, g_TCServer's (it also wakes the thread when the connection
// changes); none once Stop() ended the thread, before g_TCServer goes. A lost
// wakeup only delays Task until its poll timeout
void CCodecStream::Wake(void)
{
	if (const CWakeFd *wake = m_Wake)
		wake->Signal();
}

CCodecStream::~CCodecStream()
{
	Stop();
}

void CCodecStream::Stop(void)
{
	// kill the thread
	keep_running = false;
	// wake up poll in Task; once: valid() is false after get(). The wake goes
	// with it, g_TCServer frees it at exit
	if (m_Future.valid())
	{
		Wake();
		try
		{
			m_Future.get();
		}
		catch (const std::exception &e)
		{
			// the destructor calls this too, where a throw would end the process
			std::cerr << "Codec thread of module '" << m_CSModule << "' ended with: " << e.what() << std::endl;
		}
		m_Wake = nullptr;
	}
}

void CCodecStream::ResetStats(uint16_t streamid, ECodecType type)
{
	{
		// the codec thread reads and changes these as it sends and takes answers
		std::lock_guard<std::mutex> lock(m_StreamMutex);
		++m_uiGeneration;
		// the previous transmission's frames still waiting to go out are dropped
		// (their generation, Push), its answers still due as they come: by their
		// stream id, without a mismatch (the protocols give each transmission its
		// own, CProtocol::NewStreamId)
		m_PrevStreamIds[m_uiPrevNext] = m_uiStreamId;
		m_uiPrevNext = (m_uiPrevNext + 1) % m_PrevStreamIds.size();
		m_uiStreamId = streamid;
		m_LocalQueue.Clear();

		m_IsOpen = true;
		m_eCodecIn = type;
		m_RTMin = -1;
		m_RTMax = -1;
		m_RTSum = 0;
		m_RTCount = 0;
		m_uiTotalPackets = 0;
		m_uiMismatchCount = 0;
		m_uiSkipCount = 0;
		m_uiSuperframeCount = 0;
	}

	// Initialize D-Star slow data for transcoded (non-D-Star) streams
	if (type != ECodecType::dstar && m_PacketStream)
	{
		auto owner = m_PacketStream->GetOwnerClient();
		CCallsign my = CReflector::DStarMy(m_PacketStream->GetUserCallsign(), owner ? owner->GetProtocol() : EProtocol::none);
		CCallsign rpt1 = g_Reflector.GetCallsign();
		rpt1.SetCSModule(m_CSModule);
		CCallsign rpt2 = g_Reflector.GetCallsign();
		rpt2.SetCSModule('G');

		// the source: "<Protocol> <TG/info>" (the name goes in front, ComposeSlowData);
		// the protocol is left out when the radio already shows it as the suffix
		// of MY ("N0CALL / DMR"), which leaves "Max TG262" in the 20 characters
		std::string msg, info;
		const bool suffixed = my.HasSuffix() && !m_PacketStream->GetUserCallsign().HasSuffix();
		if (owner)
		{
			EProtocol proto = owner->GetProtocol();
			// Short protocol names to fit 20-char D-Star text
			const char *shortName;
			switch (proto) {
				case EProtocol::svxreflector: shortName = "SVX"; break;
					case EProtocol::svx:             shortName = "SVX"; break;
				case EProtocol::mmdvmclient: shortName = "DMR"; break;
				case EProtocol::openbridge:  shortName = "DMR"; break;
				case EProtocol::ysf:         shortName = "YSF"; break;
				case EProtocol::dmrmmdvm:    shortName = "DMR"; break;
				case EProtocol::dmrplus:     shortName = "DMR+"; break;
				case EProtocol::m17:         shortName = "M17"; break;
				case EProtocol::p25:         shortName = "P25"; break;
				case EProtocol::nxdn:        shortName = "NXDN"; break;
				case EProtocol::usrp:        shortName = "USRP"; break;
				case EProtocol::dcsclient:   shortName = "DCS"; break;
				case EProtocol::dextraclient:shortName = "DExtra"; break;
				case EProtocol::dplusclient: shortName = "DPlus"; break;
				case EProtocol::ysfclient:   shortName = "YSF"; break;
				default:                     shortName = owner->GetProtocolName(); break;
			}
			if (!suffixed)
				msg = shortName;

			// For TG-based protocols, TG is appended later via UpdateSlowDataTG()
			// because the source TG is not yet known at stream-open time
			if (proto == EProtocol::svxreflector || proto == EProtocol::svx || proto == EProtocol::mmdvmclient || proto == EProtocol::openbridge || proto == EProtocol::dmrmmdvm)
			{
				// msg stays "DMR" or "SVX" (or empty with the suffix), the TG comes with UpdateSlowDataTG
			}
			else if (proto == EProtocol::ysf)
			{
				int dgid = 10 + (m_CSModule - 'A');
				info = "DG" + std::to_string(dgid);
			}
			else if (proto == EProtocol::nxdn)
			{
				uint8_t ran = CNXDNProtocol::ModuleToRAN(m_CSModule);
				if (ran != 0)
					info = "RAN" + std::to_string(ran);
			}
			else if (proto == EProtocol::ysfclient)
			{
				// the DG-ID if configured, e.g. "YSF DG15"; from the connector's client,
				// not its mappings: the clients are locked here, and the connector locks
				// its mappings before the clients
				auto peer = std::dynamic_pointer_cast<CYSFClientPeer>(owner);
				if (peer && peer->GetDgId() > 0)
					info = "DG" + std::to_string(peer->GetDgId());
			}
			if (!info.empty())
				msg = msg.empty() ? info : msg + " " + info;
		}

		// the operator's first name from the DMR or NXDN ID database, in front of
		// the source: "Max DMR TG262" (ComposeSlowData fits it into 20 characters)
		const std::string name = CUser::OperatorName(my);
		const auto first = name.find_first_not_of(' ');
		std::lock_guard<std::mutex> lock(m_SlowDataMutex);
		m_SlowDataName = AsciiName((std::string::npos == first) ? std::string() : name.substr(first, name.find(' ', first) - first));
		m_SlowDataVia = msg;
		m_SlowDataTG = 0;
		m_SlowDataOwner = (owner != nullptr);
		ComposeSlowData();
		m_SlowData.Init(my, rpt1, rpt2, m_SlowDataMsg1);
	}
}

// the end of the transmission: called before the packet stream is free for the
// next one (ResetStats), its stats printed after that (PrintStats)
CCodecStream::SStats CCodecStream::CloseStats(void)
{
	std::lock_guard<std::mutex> lock(m_StreamMutex);
	m_IsOpen = false;
	// frames still waiting for an answer (a tcd slower than EndStream waits): it is
	// not taken any more
	m_LocalQueue.Clear();
	return statsLocked();
}

void CCodecStream::PrintStats(const SStats &st)
{
	if (st.rt_count > 0)
	{
		double min = 1000.0 * st.rt_min;
		double max = 1000.0 * st.rt_max;
		double ave = 1000.0 * st.rt_avg;
		auto prec = std::cout.precision();
		std::cout.precision(1);
		std::cout << std::fixed << "TC round-trip time(ms): " << min << '/' << ave << '/' << max << ", " << st.rt_count << " total packets" << std::endl;
		std::cout.precision(prec);
	}
}

CCodecStream::SStats CCodecStream::GetStats(void) const
{
	std::lock_guard<std::mutex> lock(m_StreamMutex);
	return statsLocked();
}

CCodecStream::SStats CCodecStream::statsLocked(void) const
{
	SStats st;
	st.codec_in = m_eCodecIn;
	st.rt_min = m_RTMin;
	st.rt_max = m_RTMax;
	st.rt_avg = m_RTCount > 0 ? m_RTSum / m_RTCount : 0.0;
	st.rt_count = m_RTCount;
	st.total_packets = m_uiTotalPackets;
	st.mismatch_count = m_uiMismatchCount;
	return st;
}

bool CCodecStream::IsPipelineEmpty(void)
{
	// a frame between the queues (sent, or its answer on the way back) is under the
	// lock. In m_Queue the older transmissions' frames come first: with none of
	// this one pushed, what is there is theirs, only to be dropped (the codec
	// thread may be in a send to a tcd that stalls)
	std::lock_guard<std::mutex> lock(m_StreamMutex);
	return (m_Queue.IsEmpty() || m_uiPushedGeneration != m_uiGeneration) && m_LocalQueue.IsEmpty();
}

// a name for the D-Star text, which radios show in ASCII: umlauts and the common
// accented letters of the ID databases spelled out or plain, anything else left out
std::string CCodecStream::AsciiName(const std::string &utf8)
{
	static const std::pair<const char *, const char *> map[] = {
		{ "\xC3\xA4", "ae" }, { "\xC3\xB6", "oe" }, { "\xC3\xBC", "ue" }, { "\xC3\x84", "Ae" }, { "\xC3\x96", "Oe" },
		{ "\xC3\x9C", "Ue" }, { "\xC3\x9F", "ss" }, { "\xC3\xA0", "a" }, { "\xC3\xA1", "a" }, { "\xC3\xA2", "a" },
		{ "\xC3\xA5", "a" }, { "\xC3\xA7", "c" }, { "\xC3\xA8", "e" }, { "\xC3\xA9", "e" }, { "\xC3\xAA", "e" },
		{ "\xC3\xAB", "e" }, { "\xC3\xAD", "i" }, { "\xC3\xAE", "i" }, { "\xC3\xAF", "i" }, { "\xC3\xB1", "n" },
		{ "\xC3\xB3", "o" }, { "\xC3\xB4", "o" }, { "\xC3\xB8", "o" }, { "\xC3\xBA", "u" }, { "\xC3\xBB", "u" },
		{ "\xC3\x89", "E" }, { "\xC3\x85", "A" }, { "\xC3\x98", "O" } };
	std::string out;
	for (size_t i = 0; i < utf8.size(); )
	{
		const unsigned char c = (unsigned char)utf8[i];
		if (c < 0x80)
		{
			if (c >= 0x20 && c != 0x7F)
				out += char(c);
			i++;
			continue;
		}
		const char *rep = nullptr;
		for (const auto &[from, to] : map)
			if (utf8.compare(i, 2, from) == 0)
				rep = to;
		if (rep)
			out += rep;
		// skip the whole UTF-8 sequence
		i++;
		while (i < utf8.size() && ((unsigned char)utf8[i] & 0xC0) == 0x80)
			i++;
	}
	return out;
}

// the D-Star text: "<first name> <source> TG<n>" in the 20 characters a radio
// shows, the source without the protocol when MY carries it as suffix. When it
// is too long, the TG goes to the second text, which takes turns with the first;
// then the name is shortened so the source stays whole.
// Call with m_SlowDataMutex held.
void CCodecStream::ComposeSlowData(void)
{
	auto joined = [](const std::string &a, const std::string &b) { return a.empty() ? b : (b.empty() ? a : a + " " + b); };
	const std::string tg = m_SlowDataTG ? "TG" + std::to_string(m_SlowDataTG) : std::string();
	std::string src = joined(m_SlowDataVia, tg);
	std::string msg1 = joined(m_SlowDataName, src), msg2;
	if (msg1.size() > 20 && !tg.empty() && !m_SlowDataVia.empty())
	{
		msg2 = src;
		src = m_SlowDataVia;
		msg1 = joined(m_SlowDataName, src);
	}
	if (msg1.size() > 20 && !m_SlowDataName.empty() && !src.empty())
	{
		const size_t room = (src.size() + 1 < 20) ? 20 - src.size() - 1 : 0;
		msg1 = (room >= 3) ? m_SlowDataName.substr(0, room) + " " + src : src;
		if (msg2.empty())
			msg2 = m_SlowDataName;
	}
	if (msg1.size() > 20)
		msg1.resize(20);
	if (msg2.size() > 20)
		msg2.resize(20);
	m_SlowDataMsg1 = msg1;
	m_SlowDataMsg2 = msg2;
}

void CCodecStream::UpdateSlowDataTG(uint32_t tg)
{
	// the TG of a DMR or SVX stream, known once it runs ("Max TG2625"); this is
	// the protocol's thread, the codec thread sends the texts
	std::lock_guard<std::mutex> lock(m_SlowDataMutex);
	if (tg == 0 || !m_SlowDataOwner)
		return;
	m_SlowDataTG = tg;
	ComposeSlowData();

	// Re-init slow data with updated message
	CCallsign my;
	if (m_PacketStream)
	{
		auto owner = m_PacketStream->GetOwnerClient();
		my = CReflector::DStarMy(m_PacketStream->GetUserCallsign(), owner ? owner->GetProtocol() : EProtocol::none);
	}
	CCallsign rpt1 = g_Reflector.GetCallsign();
	rpt1.SetCSModule(m_CSModule);
	CCallsign rpt2 = g_Reflector.GetCallsign();
	rpt2.SetCSModule('G');
	m_SlowData.Init(my, rpt1, rpt2, m_SlowDataMsg1);
}

////////////////////////////////////////////////////////////////////////////////////////
// initialization

bool CCodecStream::InitCodecStream()
{
	keep_running = true;
	try
	{
		// made by g_TCServer.Open(), before this; dropped by Stop(), before it goes.
		// Set before the thread (it polls it), cleared again when that fails
		m_Wake = g_TCServer.GetWake(m_CSModule);
		m_Future = std::async(std::launch::async, &CCodecStream::Thread, this);
	}
	catch(const std::exception& e)
	{
		m_Wake = nullptr;
		std::cerr << "Could not start Codec processing on module '" << m_CSModule << "': " << e.what() << std::endl;
		return true;
	}
	return false;
}

////////////////////////////////////////////////////////////////////////////////////////
// thread

void CCodecStream::Thread()
{
	while (keep_running)
	{
		Task();
	}
}

// an answer of tcd: back into the stream with its frame. Under m_StreamMutex: a
// protocol thread opening the next stream (ResetStats) empties m_LocalQueue
void CCodecStream::Answer(const STCPacket &pack, uint32_t connection)
{
	// logged after the lock: the threads waiting for it do not wait for the log
	bool skipped = false, mismatch = false;
	{
		std::lock_guard<std::mutex> lock(m_StreamMutex);
		answerLocked(pack, connection, skipped, mismatch);
	}
	if (skipped)
		std::cerr << "Transcoder lost frames on module " << m_CSModule << ", skipped" << std::endl;
	if (mismatch)
		std::cerr << "Transcoder mismatch on module " << m_CSModule << " (stale packet discarded)" << std::endl;
}

// skipped, mismatch: the first of the stream, to be logged
void CCodecStream::answerLocked(const STCPacket &pack, uint32_t connection, bool &skipped, bool &mismatch)
{
	// an answer to a frame this transmission never sent: a late one of an earlier
	// one with the same stream id (a source that reuses its id). Not taken as a
	// sign of frames lost
	if (m_IsOpen && pack.streamid == m_uiStreamId && pack.sequence >= m_uiTotalPackets)
		return;
	// frames sent on a connection before this one's, which went (a reconnect, a
	// standby tcd taking over): no answer comes for them, and none was lost by tcd
	while (!m_LocalQueue.IsEmpty() && m_LocalQueue.Front().connection != connection)
		m_LocalQueue.Pop();
	if (m_IsOpen && pack.streamid == m_uiStreamId && !m_LocalQueue.IsEmpty())
	{
		// frames tcd never answered (lost in an AMBE device that hung) come before
		// this answer: skip them, else every later answer missed its frame and the
		// rest of the transmission was dropped
		while (!m_LocalQueue.IsEmpty() && int32_t(pack.sequence - m_LocalQueue.Front().frame->GetCodecPacket()->sequence) > 0)
		{
			m_LocalQueue.Pop();
			if (m_uiSkipCount++ == 0)
				skipped = true;
		}
		// an answer older than the queue's head was skipped already: drop it, the
		// head waits for its own answer
		if (m_LocalQueue.IsEmpty() || int32_t(pack.sequence - m_LocalQueue.Front().frame->GetCodecPacket()->sequence) < 0)
			return;
		auto Packet = std::move(m_LocalQueue.Pop().frame);

		if ((pack.streamid == Packet->GetCodecPacket()->streamid) && (pack.sequence == Packet->GetCodecPacket()->sequence))
		{
			auto rt = Packet->m_rtTimer.time();
			if (0 == m_RTCount)
			{
				m_RTMin = rt;
				m_RTMax = rt;
			}
			else
			{
				if (rt < m_RTMin)
					m_RTMin = rt;
				else if (rt > m_RTMax)
					m_RTMax = rt;
			}
			m_RTSum += rt;
			m_RTCount++;

			Packet->SetCodecData(&pack);
			// Set D-Star sync and slow data for transcoded streams
			if (ECodecType::dstar != Packet->GetCodecIn())
			{
				std::lock_guard<std::mutex> lock(m_SlowDataMutex);
				uint8_t frameInSuper = Packet->GetPacketId() % 21;
				if (frameInSuper == 0)
				{
					const uint8_t DStarSync[] = { 0x55, 0x2D, 0x16 };
					Packet->SetDvData(DStarSync);
					m_uiSuperframeCount++;
					// Rotate slow data text every ~5s (12 superframes)
					if (!m_SlowDataMsg2.empty() && m_uiSuperframeCount % 24 == 12)
						m_SlowData.SetMessage(m_SlowDataMsg2);
					else if (!m_SlowDataMsg2.empty() && m_uiSuperframeCount % 24 == 0)
						m_SlowData.SetMessage(m_SlowDataMsg1);
				}
				else if (m_SlowData.IsReady())
				{
					Packet->SetDvData(m_SlowData.GetSlowData(frameInSuper, m_uiSuperframeCount));
				}
			}

			m_PacketStream->ReturnPacket(std::move(Packet));
		}
	}
	// a late answer of the previous transmission (it ended with answers due): no mismatch
	else if (m_IsOpen && pack.streamid != m_uiStreamId && std::find(m_PrevStreamIds.begin(), m_PrevStreamIds.end(), pack.streamid) == m_PrevStreamIds.end())
	{
		if (m_uiMismatchCount++ == 0)
			mismatch = true;
	}
}

void CCodecStream::DrainQueues(void)
{
	std::lock_guard<std::mutex> lock(m_StreamMutex);
	m_Queue.Clear();
	m_LocalQueue.Clear();
}

void CCodecStream::Task(void)
{
	// Drain all pending transcoder responses (prevents TCP buffer buildup / deadlock)
	STCPacket pack{};
	bool receivedAny = false;
	int tcfd = -1;	// the connection, as the last receive left it
	// at most 64 a round (tcd answers one per frame sent): a tcd that floods its
	// connection with pings does not keep the frames queued here from going out
	for (int n = 0; n < 64; n++)
	{
		uint32_t connection = 0;
		const auto got = g_TCServer.Receive(m_CSModule, &pack, 0, &tcfd, &connection);
		if (ETCReceive::none == got)
			break;
		receivedAny = true;
		if (ETCReceive::ping == got)
			continue;	// a ping or a report: what follows it is read in this round too
		Answer(pack, connection);
	}

	// the answers still due on a connection that is gone (closed, or replaced: a
	// reconnect, a standby tcd taking over) never come: the end of the stream
	// (EndStream) does not wait for them. In the order sent, the oldest first
	if (!m_LocalQueue.IsEmpty())
	{
		const uint32_t connection = g_TCServer.GetConnectionCount(m_CSModule);
		std::lock_guard<std::mutex> lock(m_StreamMutex);
		while (!m_LocalQueue.IsEmpty() && (tcfd < 0 || m_LocalQueue.Front().connection != connection))
			m_LocalQueue.Pop();
	}

	if (!receivedAny)
	{
		// said before the queue is looked at: a frame pushed after it wakes the poll
		m_Waiting = true;
		if (m_Queue.IsEmpty())
		{
			// Nothing to receive and nothing to send — wait for the wake (a frame pushed,
			// the stop) or for the transcoder's answers, ping or report on its connection
			// (every module has a codec stream, most of them idle). The connection is the
			// one the receive saw, outside its lock: one that changed since (opened,
			// closed, dropped) signals the wake as well, and the next round takes the new
			// one; the timeout is only a safety net (without a wake, its creation failed,
			// a pushed frame waits for it: 20 ms)
			const CWakeFd *wake = m_Wake;
			const int wakefd = wake ? wake->Fd() : -1;
			struct pollfd pfd[2] = { { wakefd, POLLIN, 0 }, { tcfd, POLLIN, 0 } };	// poll leaves out a -1
			poll(pfd, 2, wakefd < 0 ? 20 : 500);
			if (pfd[0].revents & POLLIN)
				wake->Clear();
		}
		m_Waiting = false;
	}

	// Send queued packets to transcoder
	while (!m_Queue.IsEmpty())
	{
		auto &Entry = m_Queue.Front();
		auto &Frame = Entry.frame;

		{
			// the stream's state under m_StreamMutex (ResetStats), but not over the
			// send: it may wait up to TC_IO_TIMEOUT for a tcd that stopped reading.
			// A frame of a transmission that ended (the next one opened while it
			// waited here) or with none open is dropped, not sent as the new one's
			std::lock_guard<std::mutex> lock(m_StreamMutex);
			if (!m_IsOpen || Entry.generation != m_uiGeneration)
			{
				m_Queue.Pop();
				continue;
			}
			Frame->SetTCParams(m_uiTotalPackets++, m_CSModule);
		}

		Frame->m_rtTimer.start();
		uint32_t connection = 0;
		if (g_TCServer.Send(Frame->GetCodecPacket(), &connection))
		{
			// no transcoder connection, or the send failed: drain queues silently
			DrainQueues();
			return;
		}

		// waits for its answer, unless the next transmission opened meanwhile (its
		// answer has the old stream id then, and is dropped)
		std::lock_guard<std::mutex> lock(m_StreamMutex);
		if (Entry.generation == m_uiGeneration)
			m_LocalQueue.Push({ std::move(m_Queue.Pop().frame), connection });
		else
			m_Queue.Pop();
	}
}
