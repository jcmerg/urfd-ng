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

#include <array>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "Defines.h"
#include "Timer.h"
#include "Protocol.h"
#include "DVHeaderPacket.h"
#include "DVFramePacket.h"
#include "YSFDefines.h"
#include "YSFFich.h"
#include "YSFGps.h"
#include "WiresXInfo.h"
#include "WiresXCmdHandler.h"

////////////////////////////////////////////////////////////////////////////////////////
// define


// Wires-X commands
#define WIRESX_CMD_UNKNOWN          0
#define WIRESX_CMD_DX_REQ           1
#define WIRESX_CMD_ALL_REQ          2
#define WIRESX_CMD_SEARCH_REQ       3
#define WIRESX_CMD_CONN_REQ         4
#define WIRESX_CMD_DISC_REQ         5

// YSF Module ID
#define YSF_MODULE_ID             'B'

////////////////////////////////////////////////////////////////////////////////////////
// class

class CYsfStreamCacheItem
{
public:
	CYsfStreamCacheItem()     {}
	~CYsfStreamCacheItem()    {}

	CDvHeaderPacket m_dvHeader;
	CDvFramePacket  m_dvFrames[5];
	uint8_t         m_uiPending = 0;	// frames of the current group of 5 not sent yet
	uint8_t         m_uiFrameId = 0;	// network frame counter of the last group sent
	// a module without transcoder: an IMBE stream (VW, P25, by its header's codec) goes out as VW
	// (2 IMBE frames in the first VW frame, then 5), anything else as DN
	bool            m_bVW = false;
	std::vector<std::array<uint8_t, 11>> m_vwImbe;
	uint8_t         m_uiVwFrames = 0;	// VW voice frames sent
	uint8_t         m_uiVwCounter = 0;	// network frame counter
};

class CYsfProtocol : public CProtocol
{
public:
	// constructor
	CYsfProtocol();

	// initialization
	bool Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6);
	void Close(void);

	// task
	void Task(void);

protected:
	// queue helper
	void HandleQueue(void);

	// keepalive helpers
	void HandleKeepalives(void);

	// stream helpers
	void OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &, const CIp &, uint8_t = 0);

	// DV packet decoding helpers
	bool IsValidConnectPacket(const CBuffer &, CCallsign *);
	bool IsValidDisconnectPacket(const CBuffer &);
	bool IsValidDvPacket(const CBuffer &, CYSFFICH *);
	bool IsValidDvHeaderPacket(const CIp &, const CYSFFICH &, const CBuffer &, std::unique_ptr<CDvHeaderPacket> &, std::array<std::unique_ptr<CDvFramePacket>, 5> &);
	bool IsValidDvFramePacket(const CIp &, const CYSFFICH &, const CBuffer &, std::unique_ptr<CDvHeaderPacket> &, std::array<std::unique_ptr<CDvFramePacket>, 5> &);
	bool IsValidDvLastFramePacket(const CIp &, const CYSFFICH &, const CBuffer &, std::unique_ptr<CDvFramePacket> &);

	// DV packet encoding helpers
	void EncodeConnectAckPacket(CBuffer *) const;
	//void EncodeConnectNackPacket(const CCallsign &, char, CBuffer *);
	//void EncodeDisconnectPacket(CBuffer *, std::shared_ptr<CClient>);
	bool EncodeYSFHeaderPacket(const CDvHeaderPacket &, CBuffer *) const;
	bool EncodeYSFPacket(const CDvHeaderPacket &, const CDvFramePacket *, CBuffer *) const;
	bool EncodeLastYSFPacket(const CDvHeaderPacket &, uint8_t, CBuffer *) const;
	// VW: header (fi header or terminator) and voice frames; the radio id of the stream, else spaces
	void EncodeVWHeaderPacket(const CDvHeaderPacket &, uint8_t fi, uint8_t counter, CBuffer *) const;
	void EncodeVWPacket(const CDvHeaderPacket &, uint8_t fn, uint8_t ft, const std::array<uint8_t, 11> *imbe, uint8_t counter, CBuffer *) const;
	void VwRadioId(const CDvHeaderPacket &, uint8_t *id20) const;
	// a frame of a stream to YSF: DN (groups of 5, the last one filled with silence) or VW (the IMBE
	// frames of the cache in VW frames of 2, then 5; at the end the rest filled with silence)
	void HandleDNFrame(CYsfStreamCacheItem &, CPacket &, std::vector<CBuffer> &) const;
	void HandleVWFrame(CYsfStreamCacheItem &, CPacket &, std::vector<CBuffer> &) const;
	uint8_t ModuleToDGID(char) const;

	// Wires-X packet decoding helpers
	bool IsValidwirexPacket(const CBuffer &, CYSFFICH *, CCallsign *, int *, int*);

	// server status packet decoding helpers
	bool IsValidServerStatusPacket(const CBuffer &) const;
	bool IsValidInfoPacket(const CBuffer &) const;
	bool IsValidAckPacket(const CBuffer &) const;
	bool IsValidOptionsPacket(const CBuffer &) const;
	uint32_t CalcHash(const uint8_t *, int) const;

	// server status packet encoding helpers
	bool EncodeServerStatusPacket(CBuffer *) const;

	// uiStreamId helpers
	uint32_t IpToStreamId(const CIp &) const;

	// debug
	bool DebugTestDecodePacket(const CBuffer &);
	bool DebugDumpHeaderPacket(const CBuffer &);
	bool DebugDumpDvPacket(const CBuffer &);
	bool DebugDumpLastDvPacket(const CBuffer &);

protected:
	// for keep alive
	CTimer m_LastKeepaliveTime;

	// for queue header caches
	std::unordered_map<char, CYsfStreamCacheItem> m_StreamsCache;
	// the positions in the data channel, by stream id
	std::unordered_map<uint16_t, CYsfGps> m_Gps;
	CYsfGps &GpsOf(uint32_t streamid);
	// VW: the radio id of a stream's first frame, for the VW out (by stream id)
	std::unordered_map<uint16_t, std::array<uint8_t, 20>> m_VwRadioId;

	// for wires-x
	CWiresxCmdHandler   m_WiresxCmdHandler;
	unsigned char m_seqNo;

	// config data
	char m_AutolinkModule;
	bool m_EnableDGID;
	unsigned m_RegistrationId;
	std::string m_RegistrationName, m_RegistrationDesc;
};
