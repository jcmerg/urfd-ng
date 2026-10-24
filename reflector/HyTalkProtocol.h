#pragma once

// HyTalkProtocol -- a HyTalk PoC server for Hytera radios (P50 Pro, HyTalk app)
//
// The radio logs in with PNAS over KCP/UDP (HyTalkPnas, HyTalkKcp), fetches
// its talk groups from our small HTTP server (its "presence server"),
// affiliates and makes group calls. A talk group is an urfd module. Voice is
// RTP on a port of its own with AMBE+2, the DMR vocoder, so audio passes
// between the radios and DMR, YSF and NXDN without transcoding.
//
// We are the floor arbiter of every group: one talker at a time. A radio's
// PTT is a call request (or a floor request in a running call), the others
// see the talker in a floor inform.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "Defines.h"
#include "Timer.h"
#include "Protocol.h"
#include "DVHeaderPacket.h"
#include "DVFramePacket.h"
#include "HyTalkPnas.h"
#include "HyTalkKcp.h"
#include "HyTalkGtrp.h"
#include "HyTalkHttps.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define HYTALK_IPV4                 true
#define HYTALK_IPV6                 false

#define HYTALK_KEEPALIVE_TIME       30      // seconds, told the radio in the register accept
#define HYTALK_PEER_TIMEOUT         120     // seconds without a datagram before a radio is dropped
#define HYTALK_LOGIN_TIMEOUT        30      // seconds from the first datagram to the login
#define HYTALK_TALKER_TIMEOUT       1.5     // seconds without RTP before a talker's transmission ends
#define HYTALK_URFD_TIMEOUT         1.0     // seconds without frames before an urfd transmission ends
#define HYTALK_PONG_DELAY           20      // seconds we hold a heartbeat pong at most
#define PNAS_IE_HEARTBEAT_INTERVAL  0x010a  // in the ping: the radio's current interval in s, shifted left by 1
#define HYTALK_GIS_TIMEOUT          60      // seconds without a GTRP message before its connection closes (keep-alive every 10 s)

#define HYTALK_AMBE_PT              87      // RTP payload type of AMBE, as the radio offers it
#define HYTALK_AMBE_FRAMES          3       // 20 ms frames per RTP packet (ptime 60 ms)

class CHyTalkProtocol : public CProtocol
{
public:
	CHyTalkProtocol();
	virtual ~CHyTalkProtocol();

	bool Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6);
	void Close(void);
	void Task(void);

	// accounts (called from the admin socket thread)
	struct SAccountInfo { std::string number, callsign, source; };	// source: "ini" or "db"
	bool AddAccount(const std::string &number, const std::string &password, const std::string &callsign);
	int RemoveAccount(const std::string &number);	// 1 = removed, 0 = not found, -1 = the credential database failed
	std::vector<SAccountInfo> GetAccounts(void) const;

	// the groups and the connected radios (called from the admin socket thread)
	struct SGroupInfo { std::string gdn, name; char module; };
	std::vector<SGroupInfo> GetGroups(void) const;
	struct SConnectedRadio
	{
		std::string callsign, number, ip;
		std::vector<std::string> groups;
		std::string callGroup;        // the group call it is in, empty if none
		bool talking;
		time_t connectedSince;
		std::string location;         // Maidenhead locator of its last GPS position, empty if none
	};
	std::vector<SConnectedRadio> GetConnectedRadios(void) const;

protected:
	void HandleQueue(void);
	void HandleKeepalives(void);

	struct SAccount
	{
		std::string password;
		std::string callsign;
	};

	// a talk group, one urfd module, and its group call
	struct SGroup
	{
		std::string gdn;              // the group number the radios know
		std::string name;
		char module = ' ';

		uint32_t callId = 0;          // 0: no group call
		uint32_t talker = 0;          // peer id of the radio with the floor, 0: none
		bool urfdTalking = false;     // an urfd transmission has the floor
		std::string urfdTalker;       // its callsign
		std::string urfdVia;          // "via DMR TG91": where it comes from, shown with the callsign
		bool urfdViaTg = false;       // the TG is in urfdVia (it may come after the header)
		CTimer idleTimer;             // since the floor became free
		CTimer lastFrame;             // urfd: since its last frame
		uint8_t pending[HYTALK_AMBE_FRAMES][9];
		int npending = 0;
	};

	struct SPeer
	{
		uint32_t id = 0;
		CHyTalkProtocol *proto = nullptr;

		// transport
		CIp addr;
		uint32_t conv = 0;
		ikcpcb *kcp = nullptr;
		std::vector<uint8_t> rx;      // KCP stream data, not yet a whole unit
		uint8_t encType = 0;          // we answer the way the radio sends
		uint8_t keyId = 0;
		uint32_t txSeq = 1;
		CTimer lastRx;
		CTimer since;
		bool dead = false;

		// login
		bool online = false;
		std::string nonce;
		std::string number;           // its subscriber number, as it sends it
		std::string callsign;
		time_t onlineSince = 0;

		// its last GPS position (GTRP)
		std::string locator;

		// groups
		std::vector<std::string> affiliated;
		std::shared_ptr<CClient> client;
		char clientModule = ' ';
		std::shared_ptr<CClient> talkClient;	// a call in another module than client's, gone with the stream

		// the group call it is in
		std::string callGdn;
		uint32_t callId = 0;
		uint32_t callTid = 0;         // the transaction the call was set up in; the radio finds the call by it
		bool callByRadio = false;     // the radio set the call up (its CALL_REQUEST), not we
		uint32_t radioCallId = 0;     // the call id of that CALL_REQUEST; the radio keys the call by it
		bool callUp = false;          // the radio has accepted / completed

		// media
		CIp rtpAddr;                  // learned from its RTP, else from its audio description
		CIp natRtp;                   // where its NAT keepalives come from: its public RTP address
		bool natRtpKnown = false;
		bool rtpKnown = false;
		uint32_t ssrc = 0;
		uint16_t rtpSeq = 0;
		uint32_t rtpTs = 0;

		// its own transmission into urfd
		uint16_t streamId = 0;        // 0: none open
		char streamModule = ' ';
		bool rejected = false;        // module busy
		uint8_t pid = 0;
		CTimer lastRtp;

		// the pong to its last heartbeat ping, held back (see OnMessage)
		bool pongPending = false;
		uint32_t pongTid = 0;
		double pongDelay = 0;
		CTimer pongTimer;
	};

	// transport
	void OnDatagram(const uint8_t *data, size_t len, const CIp &ip);
	SPeer *NewPeer(const CIp &ip, uint32_t conv, const uint8_t *appid);
	static int KcpOutput(const char *buf, int len, ikcpcb *kcp, void *user);
	void Send(SPeer &peer, const CPnasWriter &msg);
	void DropPeer(SPeer &peer, const char *why);

	// signalling
	void OnMessage(SPeer &peer, const CPnasMsg &msg);
	void OnRegister(SPeer &peer, const CPnasMsg &msg);
	void OnAffiliate(SPeer &peer, const CPnasMsg &msg);
	void OnCallRequest(SPeer &peer, const CPnasMsg &msg);
	void OnCallAccept(SPeer &peer, const CPnasMsg &msg);
	void OnCallRelease(SPeer &peer, const CPnasMsg &msg);
	void OnFloorRequest(SPeer &peer, const CPnasMsg &msg);
	void OnFloorRelease(SPeer &peer, const CPnasMsg &msg);
	void OnMessageRequest(SPeer &peer, const CPnasMsg &msg);
	void SendCommonResponse(SPeer &peer, const CPnasMsg &msg, uint8_t type);

	// group calls
	SGroup *FindGroup(const std::string &gdn);
	SGroup *FindGroup(char module);
	void EnsureGroupCall(SGroup &group);
	void InviteToCall(SPeer &peer, SGroup &group);
	void FloorInform(SGroup &group, uint32_t exceptPeer = 0);
	void FloorInformPeer(SPeer &peer, SGroup &group);
	void ReleaseGroupCall(SGroup &group);
	void TalkerEnd(SGroup &group);
	std::vector<uint8_t> AudioDescription(void) const;
	void LearnRtpAddress(SPeer &peer, const CPnasMsg &msg);

	// media
	void OnRtp(const uint8_t *data, size_t len, const CIp &ip);
	// RTP PT 127 "NAT" keepalives: true if data was one (and went back to the radio)
	static bool EchoNatKeepalive(CUdpSocket &socket, const uint8_t *data, size_t len, const CIp &ip);
	void LearnNatAddress(const CIp &ip);
	void SendRtp(SPeer &peer, const uint8_t frames[][9], int n, bool marker);
	bool OpenPeerStream(SPeer &peer, SGroup &group);
	void ClosePeerStream(SPeer &peer);
	void OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &, const CIp &);

	// urfd client of a radio, on the module of its first affiliated group
	void UpdatePeerClient(SPeer &peer, bool remove = false);

	// presence HTTP: the group list
	int TcpListen(uint16_t port, const char *what) const;
	std::string HttpAnswer(const std::string &request) const;
	// the user profile the radios ask for before they log in
	std::string ProfileAnswer(const std::string &request, const std::string &body, const std::string &addr) const;

	// GTRP: login, keep-alive and GPS positions on TCP
	struct SGisConn
	{
		int fd;
		std::vector<uint8_t> in;
		CTimer lastRx;
		std::string number, callsign;  // after its register request
		std::string addr;              // the radio's IP address
		bool warned = false;           // about positions from a radio that is not logged in
	};
	void GisTask(void);
	bool OnGisMessage(SGisConn &conn, const SGtrpMsg &msg);
	void SendGis(SGisConn &conn, const std::vector<uint8_t> &m);
	SPeer *FindPeerByNumber(const std::string &number);

	// the admin socket's copies
	void UpdateSnapshot(void);
	bool IniWriteAccount(const std::string &number, const std::string *value);
	mutable std::mutex m_SnapshotMutex;
	std::vector<SConnectedRadio> m_Snapshot;
	CTimer m_SnapshotTimer;

	// config
	mutable std::mutex m_AccountMutex;	// m_Accounts, written by the admin socket
	std::string m_Realm;
	std::string m_Address;            // our address for the radios, dotted IPv4
	uint32_t m_AddressBe = 0;
	uint16_t m_RtpPort = 0;
	uint16_t m_HttpPort = 0;
	uint16_t m_GisPort = 0;
	uint16_t m_GisInterval = 60;
	unsigned m_HangTime = 20;        // seconds a group call stays up after the last talker (the radio's own limit is 60)
	bool m_Debug = false;            // [HyTalk] Debug: every message, dropped datagrams, KCP state
	CTimer m_DebugTimer;
	std::map<std::string, CTimer> m_ResetSent;    // by address and conv: the last reset we sent
	void DebugKcpState(void);
	std::map<std::string, SAccount> m_Accounts;   // by the user part of the account
	std::vector<SGroup> m_Groups;
	uint32_t m_GroupVersion = 1;

	// state
	std::map<uint32_t, std::unique_ptr<SPeer>> m_Peers;
	uint32_t m_NextPeerId = 1;
	uint32_t m_NextCallId;
	uint8_t m_StreamCounter = 0;
	CUdpSocket m_RtpSocket;
	CUdpSocket m_RtcpSocket;         // RtpPort + 1: the radio sends its NAT keepalives here too
	CHyTalkHttps m_Presence;       // the group list on TCP HttpPort, HTTP or HTTPS
	int m_GisFd = -1;
	CHyTalkHttps m_Profile;        // the user profile on TCP Port, asked before the PNAS login
	std::vector<SGisConn> m_GisConns;
	uint32_t m_GisTid = 1;
	CTimer m_AliveTimer;
};
