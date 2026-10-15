#pragma once

// SvxProtocol -- SvxReflector server accepting incoming SvxLink node connections
// Implements SvxReflector protocol V2 with HMAC-SHA1 auth and OPUS audio
//
// Like SvxReflector: a node has one selected TG (MsgSelectTG replaces it) and
// a set of monitored TGs (MsgTgMonitor) it hears talker start/stop of. A TG has
// one talker at a time; nodes on the TG get the talker's OPUS packets as they
// came, mapped TGs also go to and come from their urfd module.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "Defines.h"
#include "Timer.h"
#include "Protocol.h"
#include "DVHeaderPacket.h"
#include "DVFramePacket.h"
#include "TGModuleMap.h"
#include "SvxAudio.h"

#include <vector>
#include <unordered_map>
#include <set>
#include <string>
#include <atomic>
#include <mutex>
#include <memory>

// Reuse message type defines from SvxReflectorProtocol.h
#include "SvxReflectorProtocol.h"

// Node info reported by SvxLink nodes
struct SSvxNodeInfo {
	std::string software;
	std::string location;
	std::string qth;
	std::string rxSiteName;
	std::string txSiteName;
	std::vector<std::string> logics;
	bool populated = false;
};

class CSvxProtocol : public CProtocol
{
public:
	CSvxProtocol();
	virtual ~CSvxProtocol();

	// initialization
	bool Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6);
	void Close(void);

	// task
	void Task(void);

	// dynamic TG management (called from admin socket thread)
	bool AddDynamicTG(uint32_t tg, char module, int ttlSeconds);
	bool RemoveDynamicTG(uint32_t tg);
	std::vector<CTGModuleMap::STGInfo> GetTGMappings(void) const;
	uint32_t ModuleToTG(char module) const;
	void RefreshActivityByModule(char module);
	void ReloadStaticTGMap(void);

	// user management (called from admin socket thread)
	bool AddUser(const std::string &callsign, const std::string &password);
	bool RemoveUser(const std::string &callsign);
	std::vector<std::string> GetUsers(void) const;

	// connected peer info (called from admin socket thread)
	struct SConnectedPeer {
		std::string callsign;
		std::string ip;
		std::set<uint32_t> subscribedTGs;   // the selected TG
		std::set<uint32_t> monitoredTGs;
		bool udpDiscovered;
		time_t connectedSince;
		SSvxNodeInfo nodeInfo;
	};
	std::vector<SConnectedPeer> GetConnectedPeers() const;

protected:
	// queue helper
	void HandleQueue(void);
	void HandleKeepalives(void);

	// per-peer state
	struct SSvxPeer
	{
		enum class EAuthState { expect_proto_ver, expect_auth_response, connected };

		int tcpFd = -1;
		CIp tcpAddr;              // address of the TCP connection, UDP must come from it
		CTimer connectTimer;      // since accept, for the login deadline
		uint32_t clientId = 0;
		std::string callsign;
		EAuthState authState = EAuthState::expect_proto_ver;
		uint8_t challenge[20] = {};
		bool sendFailed = false;  // a TCP send failed, the Task loop drops the peer
		time_t connectedSince = 0; // wall clock time of the login

		// UDP
		CIp udpEndpoint;
		bool udpDiscovered = false;
		uint16_t udpSeqSend = 0;

		// TG selection
		uint32_t selectedTG = 0;
		std::set<uint32_t> monitoredTGs;

		// where its CClient is, ' ' if it has none
		char clientModule = ' ';
		CIp clientIp;

		// OPUS decoder with sequence tracking (stateful, per peer)
		std::unique_ptr<CSvxOpusRx> rx;

		// keepalive timers
		CTimer tcpLastRecvTimer;
		CTimer tcpHeartbeatTimer;
		CTimer udpLastRecvTimer;

		// TCP receive buffer (partial frame accumulation)
		std::vector<uint8_t> tcpRecvBuf;

		// node info from SvxLink JSON
		SSvxNodeInfo nodeInfo;

		// this peer as the talker of a TG
		struct SInStream {
			uint32_t tg = 0;          // 0: not talking
			char module = ' ';        // the TG's module, ' ' if the TG is not mapped
			uint16_t streamId = 0;
			bool open = false;        // urfd stream opened (or tried, see rejected)
			bool rejected = false;    // module busy, no urfd stream for this talker
			CTimer audioTimer;        // since the last audio packet
		} inStream;
	};

	// TCP server
	bool TcpListen(void);
	void TcpAccept(void);
	bool TcpSendFrame(SSvxPeer &peer, const std::vector<uint8_t> &msg);
	void TcpReceiveFromPeer(uint32_t clientId);
	void TcpBroadcast(const std::vector<uint8_t> &msg, uint32_t exceptClientId = 0);
	void DisconnectPeer(uint32_t clientId);

	// TCP message handlers
	void OnPeerProtoVer(uint32_t clientId, const std::vector<uint8_t> &payload);
	void OnPeerAuthResponse(uint32_t clientId, const std::vector<uint8_t> &payload);
	void OnPeerNodeInfo(uint32_t clientId, const std::vector<uint8_t> &payload);
	void OnPeerSelectTG(uint32_t clientId, const std::vector<uint8_t> &payload);
	void OnPeerTgMonitor(uint32_t clientId, const std::vector<uint8_t> &payload);

	// talker start/stop to the nodes that have the TG selected or monitored
	void NotifyTalker(uint16_t type, uint32_t tg, const std::string &callsign);

	// UDP handling
	void OnUdpPacket(const CBuffer &buffer, const CIp &ip);
	void OnPeerUdpAudio(uint32_t clientId, const CBuffer &buffer);
	void OnPeerUdpFlush(uint32_t clientId);
	void ClosePeerInStream(uint32_t clientId);
	bool EnsurePeerInStream(SSvxPeer &peer);
	void PushPeerFrame(SSvxPeer &peer, const int16_t *pcm, bool last);

	// outbound audio to peers
	void SendUdpToPeer(SSvxPeer &peer, uint16_t type, const uint8_t *payload = nullptr, size_t len = 0);
	void BuildUdpHeader(CBuffer &buf, uint16_t type, uint16_t clientId, uint16_t &seq);

	// CClient entry of a peer for the module of its selected TG
	char PeerModule(const SSvxPeer &peer) const;
	void UpdatePeerClient(SSvxPeer &peer, bool remove = false);

	// stream helpers
	void OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &, const CIp &);

	// TG mapping
	char TGToModule(uint32_t tg) const;
	void LoadTGMap(void);

	// helper to get list of connected node callsigns
	std::vector<std::string> GetConnectedNodeList(void) const;

	// INI persistence for user add/remove
	bool IniAddUser(const std::string &callsign, const std::string &password);
	bool IniRemoveUser(const std::string &callsign);

	// TCP listen socket
	int m_TcpListenFd;

	// peers indexed by clientId
	std::unordered_map<uint32_t, SSvxPeer> m_Peers;
	std::unordered_map<int, uint32_t> m_FdToClientId;  // tcpFd -> clientId

	// the talker of each TG: a peer's clientId, or SVXS_URFD_TALKER for urfd audio
	std::unordered_map<uint32_t, uint32_t> m_TgTalker;

	// m_Peers belongs to the protocol thread, the admin socket gets this copy
	void UpdatePeerSnapshot(void);
	mutable std::mutex m_SnapshotMutex;
	std::vector<SConnectedPeer> m_PeerSnapshot;
	CTimer m_SnapshotTimer;
	CTimer m_AliveTimer;
	std::atomic<uint32_t> m_NextClientId;
	uint8_t m_StreamCounter;

	// per-user passwords (callsign -> password), protected by m_PasswordMutex
	mutable std::mutex m_PasswordMutex;
	std::unordered_map<std::string, std::string> m_Passwords;

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
	std::unordered_map<uint32_t, SDynTG> m_DynTGs;

	// outgoing stream state per module: TG, talker, one encoder for all nodes on the TG
	struct SOutStream {
		uint32_t tg = 0;
		std::string talker;
		CTimer lastFrame;
	};
	void EndOutStream(char module);
	std::unordered_map<char, SOutStream> m_OutStreams;
	std::unordered_map<char, OpusEncoder *> m_OutEncoders;
};
