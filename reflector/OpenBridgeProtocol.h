#pragma once

// OpenBridgeProtocol -- links urfd to a DMR network server to server, without a
// login: HBlink3, HBlink4, FreeDMR, BrandMeister.
//
// Each packet is the 53 bytes of an MMDVM DMRD packet, its slot bit clear and our
// NetworkId as the repeater ID, followed by its authentication. Two dialects:
// - classic (HBlink3, HBlink4, BrandMeister): HMAC-SHA1 of the 53 bytes with the
//   passphrase, 73 bytes in all, nothing else on the wire
// - freedmr (FreeDMR protocol version 5): DMRE packets of 89 bytes, the 53 bytes
//   with BER, RSSI, version, a timestamp, the source server and repeater and a
//   hop count, keyed BLAKE2b-16 with the passphrase padded or cut to 20 bytes;
//   besides them BCKA (keepalive, every 10 s: FreeDMR sends nothing to a peer
//   without one), BCVE (version, every 60 s), BCSQ (source quench), BCST (stun)
// A classic link turns freedmr when the other end shows it speaks that (DMRE,
// BCKA, BCVE), unless AutoUpgrade = false: a FreeDMR server refuses classic
// packets by default, as FreeDMR itself upgrades.
//
// Calls are told apart by their stream ID, not by a timeslot: every talkgroup the
// link maps runs at the same time as the others, one call per module.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <atomic>
#include <map>
#include <openssl/evp.h>
#include <openssl/opensslv.h>

#include "MMDVMClientProtocol.h"

#define OBP_DMRD_LENGTH          53	// the DMRD part of every packet
#define OBP_CLASSIC_LENGTH       73	// + HMAC-SHA1
#define OBP_DMRE4_LENGTH         85	// FreeDMR version 4: no source repeater
#define OBP_DMRE5_LENGTH         89	// FreeDMR version 5
#define OBP_FREEDMR_VERSION      5
#define OBP_MAX_HOPS             20	// as FreeDMR's MAX_FBP_HOPS
#define OBP_MAX_AGE              5	// seconds a DMRE packet may be old
#define OBP_KEEPALIVE_PERIOD     10	// seconds between BCKA
#define OBP_VERSION_PERIOD       60	// seconds between BCVE
#define OBP_ALIVE_TIMEOUT        60	// seconds without anything from a FreeDMR end
#define OBP_CLASSIC_IDLE         900	// seconds a classic end, which sends no keepalives, counts as there after its last packet
#define OBP_ENDED_WINDOW         180	// seconds a stream ID stays ended, as FreeDMR keeps it
#define OBP_QUENCH_WINDOW        900	// seconds a quenched call of ours stays quenched without its end
#define OBP_RESOLVE_PERIOD       300	// seconds between lookups of the target
#define OBP_STREAM_QUIET         5	// seconds a stream of the other end may pause

class COpenBridgeProtocol : public CMMDVMClientProtocol
{
public:
	COpenBridgeProtocol();
	~COpenBridgeProtocol();

	bool Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6) override;
	void Task(void) override;

	// a call from a DMR network connection into the same network, or our own,
	// does not go back
	bool IsSourceClientBlocked(const CClient &client) const override;

	bool IsFreeDmr(void) const { return m_FreeDmr; }
	// for the admin status: target, network, dialect, when the other end was last heard
	nlohmann::json Status(void) const;

protected:
	void SendDMRD(CBuffer &buffer, char module) override;
	std::shared_ptr<CClient> MakeModuleClient(const CCallsign &cs, const CIp &ip, char module) override;
	uint32_t TerminatorSrcId(const SMMDVMClientStreamCache &c) const override { return c.srcId; }

private:
	void HandleOpenBridge(const CBuffer &Buffer, const CIp &Ip);
	// the DMRD part of a classic or DMRE packet that passed its checks
	void TakeDMRD(const uint8_t *dmrd, uint32_t streamId);
	void Upgrade(const char *why);
	void Alive(void);

	// authentication
	std::string Key(void) const;	// the passphrase as this dialect uses it
	void Sha1(const uint8_t *data, size_t len, uint8_t *digest) const;
	bool Blake2b(const uint8_t *data, size_t len, uint8_t *digest) const;
	bool Sha1Ok(const uint8_t *data, size_t len, const uint8_t *hash) const;

	// FreeDMR bridge control
	void SendControl(const char *opcode, const uint8_t *data, size_t len, bool hashOpcode);
	void SendKeepalive(void);
	void SendVersion(void);
	void SendQuench(const uint8_t *tg, const uint8_t *streamId);

	std::string m_Passphrase;
	uint32_t    m_NetworkId = 0;
	uint32_t    m_SourceServer = 0;	// FreeDMR's source server of our calls, NetworkId by default
	uint32_t    m_PeerId = 0;		// 0: any repeater ID from the other end
	std::atomic<bool> m_FreeDmr{false};	// atomics: the admin status reads them from its thread
	bool        m_AutoUpgrade = true;
	bool        m_PreserveSourcePeer = false;
	std::atomic<bool> m_Stunned{false};	// the other end sent BCST: nothing goes to it any more
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	EVP_MAC    *m_Blake = nullptr;	// BLAKE2b for FreeDMR, OpenSSL 3 and later
#else
	void       *m_Blake = nullptr;	// none before OpenSSL 3: classic only
#endif

	CTimer      m_KeepaliveTimer, m_VersionTimer, m_AliveTimer, m_ResolveTimer, m_HousekeepingTimer;
	std::atomic<bool> m_HeardFrom{false};
	std::atomic<int64_t> m_LastHeard{0};	// steady clock, seconds
	std::atomic<bool> m_StartControl{false};	// FreeDMR: keepalive and version at once, from the task

	// the repeater of the call each module sends now (PreserveSourcePeer, and
	// FreeDMR's source repeater), 0 for none
	std::map<char, uint32_t> m_SourceRptr;
	// stream IDs on the wire: ours (a loop back to us) and the other end's that
	// ended or were quenched, with when
	std::map<uint32_t, CTimer> m_Ours, m_Ended, m_Quenched;
	// the other end's streams by when they were last heard: one that stops
	// without its terminator leaves the base class's mapping behind
	std::map<uint32_t, CTimer> m_Heard;
	void Expire(std::map<uint32_t, CTimer> &ids, double seconds);
};
