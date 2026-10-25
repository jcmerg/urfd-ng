#pragma once

// SvxReflectorProtocol -- Connects to an SvxLink SvxReflector via TCP/UDP
// Implements SvxReflector protocol V2 with HMAC-SHA1 auth and OPUS audio
//
// A node has one selected TG on an SvxReflector, it gets audio of that TG only
// and its audio goes there. The other mapped TGs are monitored: a talker on one
// of them while this side is idle selects that TG, like SvxLink nodes do. After
// a while without traffic the first mapped TG (the home TG) is selected again.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "Defines.h"
#include "Timer.h"
#include "Protocol.h"
#include "DVHeaderPacket.h"
#include "DVFramePacket.h"
#include "SvxAudio.h"

#include <vector>
#include <unordered_map>
#include <set>
#include <string>
#include <mutex>
#include <atomic>
#include "TGModuleMap.h"

// TCP message types
#define SVX_TCP_MSG_HEARTBEAT            1
#define SVX_TCP_MSG_PROTO_VER            5
#define SVX_TCP_MSG_PROTO_VER_DOWNGRADE  6
#define SVX_TCP_MSG_AUTH_CHALLENGE        10
#define SVX_TCP_MSG_AUTH_RESPONSE         11
#define SVX_TCP_MSG_AUTH_OK              12
#define SVX_TCP_MSG_ERROR                13
#define SVX_TCP_MSG_SERVER_INFO          100
#define SVX_TCP_MSG_NODE_LIST            101
#define SVX_TCP_MSG_NODE_JOINED          102
#define SVX_TCP_MSG_NODE_LEFT            103
#define SVX_TCP_MSG_TALKER_START         104
#define SVX_TCP_MSG_TALKER_STOP          105
#define SVX_TCP_MSG_SELECT_TG            106
#define SVX_TCP_MSG_TG_MONITOR           107
#define SVX_TCP_MSG_NODE_INFO            111

// UDP message types
#define SVX_UDP_MSG_HEARTBEAT            1
#define SVX_UDP_MSG_AUDIO                101
#define SVX_UDP_MSG_FLUSH_SAMPLES        102
#define SVX_UDP_MSG_ALL_FLUSHED          103

class CSvxReflectorProtocol : public CProtocol
{
public:
	CSvxReflectorProtocol();
	virtual ~CSvxReflectorProtocol();

	// initialization
	bool Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6);

	// task
	void Task(void);

	// dynamic TG management (called from admin socket thread)
	bool AddDynamicTG(uint32_t tg, char module, int ttlSeconds);
	bool RemoveDynamicTG(uint32_t tg);
	std::vector<CTGModuleMap::STGInfo> GetTGMappings(void) const;
	void RequestReconnect(void) { m_ReconnectRequested = true; }
	uint32_t ModuleToTG(char module) const;
	void RefreshActivityByModule(char module);
	void ReloadStaticTGMap(void);

protected:
	// queue helper
	void HandleQueue(void);
	void HandleKeepalives(void);

	// TCP connection
	bool TcpConnect(void);
	void TcpDisconnect(void);
	void Disconnect(void);                 // closes the incoming stream too
	bool TcpSendFrame(const std::vector<uint8_t> &msg);
	bool TcpReceive(void);                 // false if the connection was lost

	// TCP message handlers
	void OnTcpMessage(uint16_t type, const std::vector<uint8_t> &payload);
	void OnAuthChallenge(const std::vector<uint8_t> &payload);
	void OnAuthOk(void);
	void OnServerInfo(const std::vector<uint8_t> &payload);
	void OnTalkerStart(const std::vector<uint8_t> &payload);
	void OnTalkerStop(const std::vector<uint8_t> &payload);

	// TG selection
	void SelectTG(uint32_t tg);
	void SendTgMonitor(void);
	uint32_t HomeTG(void) const;
	bool IsIdle(void) const { return !m_InStream.open && m_OutStreamTG.empty(); }
	void UpdateSelection(void);

	// UDP audio
	void OnUdpPacket(const CBuffer &buffer);
	void OnUdpAudio(const CBuffer &buffer);
	void PushInFrame(const int16_t *pcm, bool last);
	bool EnsureInStream(void);
	void CloseInStream(void);
	void EncodeAndSendAudio(const int16_t *pcm);
	void BuildUdpHeader(CBuffer &buf, uint16_t type);

	// TG mapping
	char TGToModule(uint32_t tg) const;
	void LoadTGMap(void);
	void RemoveOrphanClients(const std::set<char> &modules);

	// stream helpers
	void OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &, const CIp &);

	// state
	enum class EState { disconnected, connecting, authenticating, connected };
	EState m_State;

	// TCP
	int m_TcpFd;
	CIp m_ServerIp;
	std::vector<uint8_t> m_TcpRecvBuf;

	// timers
	CTimer m_TcpHeartbeatTimer;
	CTimer m_TcpLastReceiveTimer;
	CTimer m_UdpHeartbeatTimer;
	CTimer m_UdpLastReceiveTimer;
	CTimer m_ReconnectTimer;
	int m_ReconnectBackoff;

	// auth
	uint16_t m_ClientId;
	uint16_t m_UdpSeq;
	std::string m_Password;
	std::string m_Callsign;
	std::string m_Host;
	std::string m_Software;     // "sw" in the node info, urfd unless the ini says otherwise

	// OPUS
	OpusEncoder *m_OpusEncoder;
	CSvxOpusRx m_Rx;

	// gain (linear numerator, 256 = 0dB)
	int32_t m_RxGainNum;
	int32_t m_TxGainNum;

	// TG mapping (static + dynamic)
	mutable std::mutex m_TGMutex;
	std::unordered_map<uint32_t, char> m_TGToModule;
	std::unordered_map<char, uint32_t> m_ModuleToTG;

	// Dynamic TG tracking
	struct SDynTG {
		std::chrono::steady_clock::time_point expires;
	};
	std::unordered_map<uint32_t, SDynTG> m_DynTGs;  // dynamic TG expiry info

	// the mapped TGs changed (admin thread, reload): monitor list and selection follow
	std::atomic<bool> m_TGsChanged{false};

	// Reconnect flag (set by admin API, checked in Task loop)
	std::atomic<bool> m_ReconnectRequested{false};

	// the TG selected on the reflector, and when it last carried audio either way
	uint32_t m_SelectedTG;
	CTimer m_TGActivityTimer;

	// incoming stream state
	struct SIncomingStream {
		uint32_t tg = 0;
		char module = ' ';
		uint16_t streamId = 0;
		bool open = false;
		bool rejected = false;      // the module was busy, this talker's audio is dropped
		std::string talkerCallsign;
		CTimer audioTimer;          // since the last audio packet
	};
	SIncomingStream m_InStream;
	uint8_t m_StreamCounter;

	// outgoing stream state per module, and when it last sent a frame
	std::unordered_map<char, uint32_t> m_OutStreamTG;
	std::unordered_map<char, CTimer> m_OutActivity;
};
