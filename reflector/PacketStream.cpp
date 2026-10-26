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


#include <cstring>

#include "PacketStream.h"
#include "Global.h"

////////////////////////////////////////////////////////////////////////////////////////
// constructor

CPacketStream::CPacketStream(char module) : m_PSModule(module)
{
	m_bOpen = false;
	m_uiStreamId = 0;
	m_uiPacketCntr = 0;
	m_uiSourceTG = 0;
	m_OwnerClient = nullptr;
	m_CodecStream = nullptr;
}

bool CPacketStream::InitCodecStream()
{
	m_CodecStream = std::unique_ptr<CCodecStream>(new CCodecStream(this, m_PSModule));
	if (m_CodecStream)
		return m_CodecStream->InitCodecStream();
	else
	{
		std::cerr << "Could not create a CCodecStream for module '" << m_PSModule << "'" << std::endl;
		return true;
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// open / close

bool CPacketStream::OpenPacketStream(const CDvHeaderPacket &DvHeader, std::shared_ptr<CClient>client, bool echo)
{
	// not already open?
	if ( !m_bOpen )
	{
		// update status
		m_bLastPushed = false;
		m_bLastRouted = false;
		m_bEcho = echo;
		m_uiStreamId = DvHeader.GetStreamId();
		m_bOpen = true;
		m_uiPacketCntr = 0;
		m_uiSourceTG = 0;
		m_DvHeader = DvHeader;
		m_OwnerClient = client;
		m_Dprs.Reset();
		m_LastPacketTime.start();
		// with [Transcoder] Modules = auto a module is transcoded only while tcd is connected for it;
		// an echo playback was recorded after the transcoder and has every codec already
		m_Transcode = !echo && m_CodecStream && g_Reflector.IsTranscoded(m_PSModule);
		if (m_Transcode)
			m_CodecStream->ResetStats(m_uiStreamId, m_DvHeader.GetCodecIn());
		return true;
	}
	return false;
}

void CPacketStream::ClosePacketStream(void)
{
	// update status
	m_bOpen = false;
	m_uiStreamId = 0;
	m_OwnerClient.reset();
	if (m_Transcode)
		m_CodecStream->ReportStats();
}

////////////////////////////////////////////////////////////////////////////////////////
// push & pop

void CPacketStream::Push(std::unique_ptr<CPacket> Packet)
{
	// update stream dependent packet data
	m_LastPacketTime.start();
	if (Packet->IsDvFrame())
	{
		Packet->UpdatePids(m_uiPacketCntr++);
		if (Packet->IsLastPacket())
			m_bLastPushed = true;
	}
	if ( m_Transcode && Packet->IsDvFrame() && Packet->IsLocalOrigin())
	{
		// yes, push packet to trancoder queue
		// first, recast to a CDvFramePacket
		auto Frame = std::unique_ptr<CDvFramePacket>(static_cast<CDvFramePacket *>(Packet.release()));
		// trancoder will push it to m_Queue after transcoding
		// is completed
		m_CodecStream->Push(std::move(Frame));
	}
	else
	{
		// no, just bypass transcoder
		m_Queue.Push(std::move(Packet));
	}
}

// A stream that ends without its last packet (lost, or the source stopped
// sending) gets one here, so every protocol sends its normal end of
// transmission (DMR terminator, YSF/NXDN/P25 terminator, D-Star end frame, M17
// EOS, ...). It holds silence in every codec and bypasses the transcoder.
void CPacketStream::PushEndPacket(void)
{
	static const uint8_t dstar[9]   = { 0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8 };
	static const uint8_t dvdata[3]  = { 0x16, 0x29, 0xF5 };
	static const uint8_t dmr[9]     = { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };
	static const uint8_t sync[7]    = { 0 };
	static const uint8_t c2_3200[8] = { 0x01, 0x00, 0x09, 0x43, 0x9C, 0xE4, 0x21, 0x08 };
	static const uint8_t c2_1600[8] = { 0x0C, 0x41, 0x09, 0x03, 0x0C, 0x41, 0x09, 0x03 };
	static const uint8_t imbe[11]   = { 0x04, 0x0C, 0xFD, 0x7B, 0xFB, 0x7D, 0xF2, 0x7B, 0x3D, 0x9E, 0x45 };

	if ( !m_bOpen )
		return;

	const ECodecType codec = m_DvHeader.GetCodecIn();
	STCPacket tc;
	memset(&tc, 0, sizeof(tc));
	tc.module = m_PSModule;
	tc.is_last = true;
	tc.streamid = m_uiStreamId;
	tc.codec_in = codec;
	memcpy(tc.dstar, dstar, sizeof(tc.dstar));
	memcpy(tc.dmr, dmr, sizeof(tc.dmr));
	if (ECodecType::c2_1600 == codec)
		memcpy(tc.m17, c2_1600, sizeof(c2_1600));	// the second half is meta data
	else
	{
		memcpy(tc.m17, c2_3200, sizeof(c2_3200));
		memcpy(tc.m17 + sizeof(c2_3200), c2_3200, sizeof(c2_3200));
	}
	memcpy(tc.p25, imbe, sizeof(tc.p25));

	// the pids 0xff are set from the stream's packet counter
	auto frame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(m_uiStreamId, 0xffu, dstar, dvdata, 0xffu, 0xffu, dmr, sync, codec, true));
	frame->SetCodecData(&tc);
	frame->SetPacketModule(m_PSModule);
	// an interlink peer's stream stays one: URF does not send it back to its peers
	if ( !m_DvHeader.IsLocalOrigin() )
		frame->SetRemotePeerOrigin();
	frame->UpdatePids(m_uiPacketCntr++);
	m_bLastPushed = true;
	m_Queue.Push(std::move(frame));
}

////////////////////////////////////////////////////////////////////////////////////////
// get

const CIp *CPacketStream::GetOwnerIp(void)
{
	if ( m_OwnerClient != nullptr )
	{
		return &(m_OwnerClient->GetIp());
	}
	return nullptr;
}
