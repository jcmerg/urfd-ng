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
#include <sys/eventfd.h>

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

CCodecStream::CCodecStream(CPacketStream *PacketStream, char module) : m_CSModule(module), m_IsOpen(false), m_EventFD(-1),
	m_uiStreamId(0), m_uiPort(0), m_uiPid(0), m_eCodecIn(ECodecType::none),
	m_RTMin(-1), m_RTMax(-1), m_RTSum(0), m_RTCount(0),
	m_uiTotalPackets(0), m_uiMismatchCount(0), m_uiSkipCount(0), m_uiSuperframeCount(0)
{
	m_PacketStream = PacketStream;
	m_EventFD = eventfd(0, EFD_NONBLOCK);
	if (m_EventFD < 0)
		std::cerr << "CodecStream[" << module << "]: eventfd creation failed" << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////
// destructor

void CCodecStream::Push(std::unique_ptr<CDvFramePacket> p)
{
	m_Queue.Push(std::move(p));
	if (m_EventFD >= 0)
	{
		uint64_t val = 1;
		// a lost wakeup only delays Task until its 20 ms poll timeout
		if (write(m_EventFD, &val, sizeof(val)) < 0) {}
	}
}

CCodecStream::~CCodecStream()
{
	Stop();
	if (m_EventFD >= 0)
	{
		close(m_EventFD);
		m_EventFD = -1;
	}
}

void CCodecStream::Stop(void)
{
	// kill the thread
	keep_running = false;
	// wake up poll in Task
	if (m_EventFD >= 0)
	{
		uint64_t val = 1;
		// a lost wakeup only delays Task until its 20 ms poll timeout
		if (write(m_EventFD, &val, sizeof(val)) < 0) {}
	}
	if ( m_Future.valid() )
	{
		m_Future.get();
	}
}

void CCodecStream::ResetStats(uint16_t streamid, ECodecType type)
{
	// Drain stale packets from previous stream
	while (!m_LocalQueue.IsEmpty())
		m_LocalQueue.Pop();

	m_IsOpen = true;
	keep_running = true;
	m_uiStreamId = streamid;
	m_uiPid = 0;
	m_eCodecIn = type;
	m_RTMin = -1;
	m_RTMax = -1;
	m_RTSum = 0;
	m_RTCount = 0;
	m_uiTotalPackets = 0;
	m_uiMismatchCount = 0;
	m_uiSkipCount = 0;
	m_uiSuperframeCount = 0;

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
			if (proto == EProtocol::svxreflector || proto == EProtocol::svx || proto == EProtocol::mmdvmclient || proto == EProtocol::dmrmmdvm)
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

void CCodecStream::ReportStats()
{
	m_IsOpen = false;
	// display stats
	if (m_RTCount > 0)
	{
		double min = 1000.0 * m_RTMin;
		double max = 1000.0 * m_RTMax;
		double ave = 1000.0 * m_RTSum / double(m_RTCount);
		auto prec = std::cout.precision();
		std::cout.precision(1);
		std::cout << std::fixed << "TC round-trip time(ms): " << min << '/' << ave << '/' << max << ", " << m_RTCount << " total packets" << std::endl;
		std::cout.precision(prec);
	}
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
		m_Future = std::async(std::launch::async, &CCodecStream::Thread, this);
	}
	catch(const std::exception& e)
	{
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

void CCodecStream::Task(void)
{
	// Drain all pending transcoder responses (prevents TCP buffer buildup / deadlock)
	STCPacket pack;
	bool receivedAny = false;
	while (g_TCServer.Receive(m_CSModule, &pack, 0))
	{
		receivedAny = true;
		if (m_IsOpen && pack.streamid == m_uiStreamId && !m_LocalQueue.IsEmpty())
		{
			// frames tcd never answered (lost in an AMBE device that hung) come before
			// this answer: skip them, else every later answer missed its frame and the
			// rest of the transmission was dropped
			while (!m_LocalQueue.IsEmpty() && int32_t(pack.sequence - m_LocalQueue.Front()->GetCodecPacket()->sequence) > 0)
			{
				m_LocalQueue.Pop();
				if (m_uiSkipCount++ == 0)
					std::cerr << "Transcoder lost frames on module " << m_CSModule << ", skipped" << std::endl;
			}
			// an answer older than the queue's head was skipped already: drop it, the
			// head waits for its own answer
			if (m_LocalQueue.IsEmpty() || int32_t(pack.sequence - m_LocalQueue.Front()->GetCodecPacket()->sequence) < 0)
				continue;
			auto Packet = m_LocalQueue.Pop();

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
		else if (m_IsOpen && pack.streamid != m_uiStreamId)
		{
			if (m_uiMismatchCount++ == 0)
				std::cerr << "Transcoder mismatch on module " << m_CSModule << " (stale packet discarded)" << std::endl;
		}
	}

	if (!receivedAny && m_Queue.IsEmpty())
	{
		// Nothing to receive and nothing to send — wait for eventfd signal; a module
		// without a transcoder connection has nothing to receive and waits longer
		// (every module has a codec stream, most of them idle)
		struct pollfd pfd = { m_EventFD, POLLIN, 0 };
		poll(&pfd, 1, g_TCServer.GetFD(m_CSModule) < 0 ? 500 : 20);
		if (pfd.revents & POLLIN)
		{
			uint64_t val;
			if (read(m_EventFD, &val, sizeof(val)) < 0) {}  // only clears the counter
		}
	}

	// Send queued packets to transcoder
	while (!m_Queue.IsEmpty())
	{
		auto &Frame = m_Queue.Front();

		if (m_IsOpen)
		{
			Frame->SetTCParams(m_uiTotalPackets++, m_CSModule);

			int fd = g_TCServer.GetFD(m_CSModule);
			if (fd < 0)
			{
				// Transcoder disconnected — drain queues silently
				while (!m_Queue.IsEmpty()) m_Queue.Pop();
				while (!m_LocalQueue.IsEmpty()) m_LocalQueue.Pop();
				return;
			}

			Frame->m_rtTimer.start();
			if (g_TCServer.Send(Frame->GetCodecPacket()))
			{
				// Send failed — drain queues
				while (!m_Queue.IsEmpty()) m_Queue.Pop();
				while (!m_LocalQueue.IsEmpty()) m_LocalQueue.Pop();
				return;
			}

			m_LocalQueue.Push(std::move(m_Queue.Pop()));
		}
		else
		{
			m_Queue.Pop();
		}
	}
}
