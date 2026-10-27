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
#include <mutex>
#include "Timer.h"
#include "DVHeaderPacket.h"
#include "Client.h"
#include "CodecStream.h"
#include "DStarDprs.h"

////////////////////////////////////////////////////////////////////////////////////////

//#define STREAM_TIMEOUT      (0.600)
#define STREAM_TIMEOUT      (1.600)

////////////////////////////////////////////////////////////////////////////////////////
// class

class CPacketStream
{
public:
	CPacketStream(char module);
	bool InitCodecStream();

	// open / close
	// echo: the playback of a recording, which already carries every codec
	bool OpenPacketStream(const CDvHeaderPacket &, std::shared_ptr<CClient>, bool echo = false);
	void ClosePacketStream(void);
	// a silent last frame, for a stream that ends without one (timeout)
	void PushEndPacket(void);

	// push & pop
	void ReturnPacket(std::unique_ptr<CPacket> p) { m_Queue.Push(std::move(p)); }
	void Push(std::unique_ptr<CPacket> packet);
	void Tickle(void)                               { m_LastPacketTime.start(); }

	// get
	std::shared_ptr<CClient> GetOwnerClient(void)   { return m_OwnerClient; }
	const CIp       *GetOwnerIp(void);
	bool             IsExpired(void) const          { return (m_LastPacketTime.time() > STREAM_TIMEOUT); }
	// its last packet has been pushed / has been routed to the protocols
	bool             IsLastPushed(void) const       { return m_bLastPushed; }
	bool             IsEnded(void) const            { return m_bLastRouted; }
	void             SetLastRouted(uint16_t sid)    { if (m_bOpen && sid == m_uiStreamId) m_bLastRouted = true; }
	// the echo module plays a recording back
	bool             IsEchoPlayback(void) const     { return m_bEcho; }
	bool             IsOpen(void) const             { return m_bOpen; }
	uint16_t         GetStreamId(void) const        { return m_uiStreamId; }
	char             GetModule(void) const          { return m_PSModule; }
	CCallsign        GetUserCallsign(void) const    { std::lock_guard<std::mutex> lock(m_UserMutex); return m_DvHeader.GetMyCallsign(); }
	// the talker turned out to be someone else (a DMR ID learned from its talker
	// alias): what the dashboard shows, the modes got the header already
	void             SetUserCallsign(const CCallsign &cs) { std::lock_guard<std::mutex> lock(m_UserMutex); m_DvHeader.SetMyCallsign(cs); }
	char             GetRpt2Module(void) const      { return m_DvHeader.GetRpt2Module(); }
	CCodecStream    *GetCodecStream(void) const     { return m_CodecStream.get(); }
	// a D-Star frame's slow data, true with a new DPRS position
	bool             DprsIn(const CDvFramePacket &frame, double &lat, double &lon) { return m_Dprs.Add(frame.GetDstarPacketId(), frame.GetDvData(), lat, lon); }
	bool             HasCodecStream(void) const      { return m_CodecStream != nullptr; }
	// whether the open stream goes through the transcoder, fixed when it opened
	bool             IsTranscoding(void) const      { return m_Transcode; }
	bool             IsCodecPipelineEmpty(void) { return !m_CodecStream || m_CodecStream->IsPipelineEmpty(); }
	uint32_t         GetSourceTG(void) const        { return m_uiSourceTG; }
	void             SetSourceTG(uint32_t tg)       { m_uiSourceTG = tg; if (m_Transcode) m_CodecStream->UpdateSlowDataTG(tg); }

	// pass-through
	std::unique_ptr<CPacket> Pop()        { return m_Queue.Pop(); }
	std::unique_ptr<CPacket> PopWait()    { return m_Queue.PopWait(); }
	void Shutdown()                       { m_Queue.Shutdown(); }
	void StopCodecStream()                { if (m_CodecStream) m_CodecStream->Stop(); }
	bool IsEmpty()                        { return m_Queue.IsEmpty(); }

protected:
	// data
	CSafePacketQueue<std::unique_ptr<CPacket>> m_Queue;
	const char          m_PSModule;
	std::atomic<bool>   m_bOpen;
	std::atomic<uint16_t> m_uiStreamId;
	uint32_t            m_uiPacketCntr;
	CTimer              m_LastPacketTime;
	CDvHeaderPacket     m_DvHeader;
	mutable std::mutex  m_UserMutex;	// its MY may change while the stream runs
	std::shared_ptr<CClient> m_OwnerClient;
	std::unique_ptr<CCodecStream> m_CodecStream;
	std::atomic<bool> m_Transcode { false };	// this stream goes through the transcoder (decided when it opens)
	std::atomic<bool> m_bLastPushed { false };
	std::atomic<bool> m_bLastRouted { false };
	std::atomic<bool> m_bEcho { false };
	uint32_t            m_uiSourceTG;
	CDStarDprs          m_Dprs;
};
