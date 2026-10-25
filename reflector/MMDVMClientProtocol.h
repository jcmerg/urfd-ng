#pragma once

// MMDVMClientProtocol -- Connects to a DMR master via the MMDVM protocol
// URFD acts as an MMDVM repeater client to an MMDVM master server.
//
// Copyright (C) 2024-2026
// Licensed under the GNU General Public License v3 or later

#include "Defines.h"
#include "Timer.h"
#include "Protocol.h"
#include "DVHeaderPacket.h"
#include "DVFramePacket.h"
#include "SHA256.h"
#include "TGModuleMap.h"
#include "BPTC19696.h"
#include "BrandMeisterApi.h"
#include "MmdvmClientConfig.h"
#include <chrono>
#include <set>

// MMDVM protocol constants
#define MMDVMCLI_RETRY_PERIOD       10  // seconds between retransmissions
#define MMDVMCLI_TIMEOUT_PERIOD     60  // seconds before connection is considered dead
#define MMDVMCLI_PING_PERIOD        10  // seconds between keepalive pings
#define MMDVMCLI_OPTIONS_WAIT        2  // seconds to wait for an ACK of the options; BrandMeister sends none
#define MMDVMCLI_OPTIONS_RESENDS     3  // times the options go out again, every MMDVMCLI_RETRY_PERIOD, until the master answers

// MMDVM data packet size
#define MMDVMCLI_DATA_PACKET_LENGTH 55

// MMDVM byte 15 flags (NOT the same as DMR_DT_* values from Protocol.h)
// These are the flag bits in the MMDVM wire format
#define MMDVMCLI_FLAG_SLOT1         0x00
#define MMDVMCLI_FLAG_SLOT2         0x80
#define MMDVMCLI_FLAG_DATA_SYNC     0x20
#define MMDVMCLI_FLAG_VOICE_SYNC    0x10
// Colour code for this repeater
#define MMDVMCLI_COLOUR_CODE        1

// Stream cache for outbound triplet buffering (3 AMBE frames per DMRD packet)
struct SMMDVMClientStreamCache
{
	CDvHeaderPacket header;
	CDvFramePacket  frames[3];
	uint8_t         frameCount;
	uint32_t        srcId;
	uint8_t         seqNo;       // voice superframe counter (0-5 cycle)
	uint8_t         pktSeqNo;    // global DMRD packet sequence counter
	uint32_t        streamId;
	bool            held = false;    // header waits for the first frame (module not transcoded)
	bool            local = false;   // not AMBE+2 on a module without transcoding: stays here
};

class CMMDVMClientProtocol : public CProtocol
{
public:
	// instance 1 is [MMDVMClient], n is [MMDVMClient<n>]
	explicit CMMDVMClientProtocol(unsigned instance = 1)
		: m_Instance(instance), m_Keys(MmdvmClientKeys(instance)), m_Name(MmdvmClientName(instance)) {}

	unsigned GetInstance(void) const          { return m_Instance; }
	const std::string &GetName(void) const    { return m_Name; }
	const std::string &GetNetwork(void) const { return m_Network; }
	uint32_t GetDmrId(void) const             { return m_uiDmrId; }
	const std::string &GetMasterAddress(void) const { return m_MasterAddress; }
	bool IsConnected(void) const              { return m_State == EHBState::RUNNING; }

	// a call from another MMDVMClient connection: never from this one or one in
	// the same network, else only if neither blocks the other by name
	bool IsSourceClientBlocked(const CClient &client) const override;
	unsigned GetMmdvmConnection(void) const override { return m_Instance; }

	// initialization
	bool Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6);

	// task
	void Task(void);

	// dynamic TG management (called from admin socket thread)
	CTGModuleMap &GetTGMap(void) { return m_TGMap; }
	void RequestReconnect(void) { m_ReconnectRequested = true; }
	void RequestKerchunk(uint32_t tg) { m_PendingKerchunk = tg; }
	CBrandMeisterApi &GetBmApi() { return m_BmApi; }

protected:
	// MMDVM state machine
	enum class EHBState
	{
		DISCONNECTED,
		WAITING_LOGIN,
		WAITING_AUTH,
		WAITING_CONFIG,
		WAITING_OPTIONS,
		RUNNING
	};

	// State machine
	void HandleStateMachine(void);
	void HandleIncoming(const CBuffer &Buffer, const CIp &Ip);
	void HandleQueue(void);
	void HandleKeepalives(void) {}  // keepalives handled by state machine

	// Auth packet helpers
	void SendLogin(void);
	void SendAuth(void);
	void SendConfig(void);
	void SendOptions(void);
	void SendPing(void);
	void SendClose(void);
	void SendKerchunk(uint32_t tg);

	// DMRD incoming: Master -> Reflector
	void OnDMRDPacketIn(const CBuffer &Buffer);
	void OnDMRDVoiceHeaderIn(const CBuffer &Buffer, uint32_t srcId, uint32_t dstId, uint32_t streamId);
	void OnDMRDVoiceFrameIn(const CBuffer &Buffer, uint32_t srcId, uint32_t dstId, uint32_t streamId);
	void OnDMRDTerminatorIn(const CBuffer &Buffer, uint32_t srcId, uint32_t dstId, uint32_t streamId);

	// DMRD outgoing: Reflector -> Master
	bool EncodeDMRDHeader(const CDvHeaderPacket &header, char module, CBuffer &buffer);
	bool EncodeDMRDVoiceFrame(const CDvFramePacket &frame0, const CDvFramePacket &frame1, const CDvFramePacket &frame2, uint8_t seqNo, uint32_t streamId, char module, CBuffer &buffer);
	bool EncodeDMRDTerminator(uint32_t streamId, char module, CBuffer &buffer);

	// DMR frame construction helpers (adapted from CDmrmmdvmProtocol)
	void AppendVoiceLCToBuffer(CBuffer *, uint32_t srcId, uint32_t dstId) const;
	void AppendTerminatorLCToBuffer(CBuffer *, uint32_t srcId, uint32_t dstId) const;
	void ReplaceEMBInBuffer(CBuffer *, uint8_t) const;
	void AppendDmrIdToBuffer(CBuffer *, uint32_t) const;
	void AppendDmrRptrIdToBuffer(CBuffer *, uint32_t) const;
	uint8_t SlotFlag(char module) const;
	// a module tcd transcodes: every mode reaches the master; others only pass AMBE+2
	bool Transcoded(char module) const;

	// DMR ID / Callsign helpers
	uint32_t CallsignToDmrId(const CCallsign &cs) const;
	CCallsign DmrIdToCallsign(uint32_t id) const;

	// Stream helpers
	void OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &, const CIp &);

private:
	// which connection this is
	const unsigned  m_Instance;
	const SJsonKeys::MMDVMCLIENT m_Keys;
	const std::string m_Name;
	std::string     m_Network;

	// the client of each module, ours alone: another connection may use the
	// same callsign to the same master
	std::unordered_map<char, std::shared_ptr<CClient>> m_ModuleClients;

	// Config
	uint32_t        m_FallbackDmrId;
	uint32_t        m_uiDmrId;
	uint8_t         m_uiId[4];         // DMR ID as big-endian 4 bytes
	std::string     m_Password;
	std::string     m_Callsign;
	std::string     m_MasterAddress;   // host name or address from the config
	CIp             m_MasterIp;
	uint16_t        m_MasterPort;
	void            ResolveMaster(void);
	CBrandMeisterApi m_BmApi;
	bool             m_BmSynced = false;
	// a sync that did not go through is tried again: a new hotspot ID is known to
	// the API only some minutes after its first login ("Device not found")
	std::chrono::steady_clock::time_point m_BmNextSync {};
	int              m_BmSyncDelay = 60;	// seconds, doubled up to 30 minutes
	bool SyncBrandMeisterTGs();	// true when BrandMeister has every static TG now

	// State machine
	std::atomic<EHBState> m_State{EHBState::DISCONNECTED};
	uint8_t         m_uiSalt[4];       // Auth salt from master

	// Timers
	CTimer          m_RetryTimer;
	unsigned        m_OptionsResends = 0;	// RUNNING: the options go out again this many times
	CTimer          m_TimeoutTimer;
	CTimer          m_PingTimer;

	// TG mapping
	CTGModuleMap    m_TGMap;

	// Incoming stream tracking: master streamId -> URFD streamId
	std::unordered_map<uint32_t, uint16_t> m_IncomingStreams;

	// Outbound stream cache per module (triplet buffering)
	std::unordered_map<char, SMMDVMClientStreamCache> m_OutboundCache;

	// SHA256
	CSHA256         m_SHA256;

	// Reconnect flag (set by admin API, checked in Task loop)
	std::atomic<bool> m_ReconnectRequested{false};
	// Kerchunk request (TG to kerchunk, 0 = none)
	std::atomic<uint32_t> m_PendingKerchunk{0};
};
