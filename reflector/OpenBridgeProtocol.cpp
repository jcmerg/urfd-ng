// OpenBridgeProtocol -- links urfd to a DMR network server to server
// (OpenBridgeProtocol.h)
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <chrono>
#include <cstring>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#include <openssl/params.h>
#endif

#include "Global.h"
#include "DMRMMDVMClient.h"
#include "OpenBridgePeer.h"
#include "OpenBridgeProtocol.h"

static uint32_t Be32(const uint8_t *p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static void PutBe32(uint8_t *p, uint32_t v)
{
	p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

COpenBridgeProtocol::COpenBridgeProtocol() : CMMDVMClientProtocol(std::string("OpenBridge"))
{
}

COpenBridgeProtocol::~COpenBridgeProtocol()
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	EVP_MAC_free(m_Blake);
#endif
}

////////////////////////////////////////////////////////////////////////////////////////
// initialization

bool COpenBridgeProtocol::Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6)
{
	const auto &k = g_Keys.openbridge;
	m_NetworkId = g_Configure.GetUnsigned(k.networkid);
	m_SourceServer = g_Configure.Contains(k.sourceserver) ? g_Configure.GetUnsigned(k.sourceserver) : m_NetworkId;
	m_PeerId = g_Configure.Contains(k.peerid) ? g_Configure.GetUnsigned(k.peerid) : 0;
	m_Passphrase = g_Configure.GetString(k.passphrase);
	m_FreeDmr = g_Configure.Contains(k.dialect) && g_Configure.GetString(k.dialect) == "freedmr";
	m_AutoUpgrade = !g_Configure.Contains(k.autoupgrade) || g_Configure.GetBoolean(k.autoupgrade);
	m_PreserveSourcePeer = g_Configure.Contains(k.preservesourcepeer) && g_Configure.GetBoolean(k.preservesourcepeer);
	m_FallbackDmrId = g_Configure.Contains(k.fallbackdmrid) ? g_Configure.GetUnsigned(k.fallbackdmrid) : 0;
	m_uiDmrId = m_NetworkId;
	PutBe32(m_uiId, m_NetworkId);
	m_Callsign = "OBP";	// the link's clients: OBP A, OBP B, ...
	m_EchoCheck = false;	// no master echoes a call back here

	m_MasterPort = uint16_t(g_Configure.GetUnsigned(k.port));
	m_MasterAddress = g_Configure.GetString(k.address);
	ResolveMaster();
	m_Network = MmdvmClientNetwork(g_Configure.Contains(k.network) ? g_Configure.GetString(k.network) : "", m_MasterAddress);

	ParseBlockProtocols(k.blockprotocols, m_Name);
	SaveBlockDefaults();
	m_TGMap.LoadFromConfig("obp", m_Name);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	m_Blake = EVP_MAC_fetch(nullptr, "BLAKE2BMAC", nullptr);
#endif
	if (!m_Blake && (m_FreeDmr || m_AutoUpgrade))
	{
		std::cerr << m_Name << ": OpenSSL has no BLAKE2BMAC (OpenSSL 3 is needed) for FreeDMR, the link stays classic" << std::endl;
		m_FreeDmr = m_AutoUpgrade = false;
	}

	// everything the task reads is set before it starts
	m_State = EHBState::RUNNING;	// nothing to log in to
	m_KeepaliveTimer.start();
	m_VersionTimer.start();
	m_ResolveTimer.start();
	m_StartControl = m_FreeDmr.load();	// FreeDMR sends nothing to a peer it has no keepalive of
	std::cout << m_Name << ": network ID " << m_NetworkId << ", " << (m_FreeDmr ? "FreeDMR" : "classic")
	          << ", target " << m_MasterAddress << ":" << m_MasterPort << ", local port " << port << ", network " << m_Network << std::endl;
	return CProtocol::Initialize(type, ptype, port, has_ipv4, has_ipv6);
}

bool COpenBridgeProtocol::IsSourceClientBlocked(const CClient &client) const
{
	if (client.GetProtocol() != EProtocol::mmdvmclient && client.GetProtocol() != EProtocol::openbridge)
		return false;
	const auto *src = dynamic_cast<const CMMDVMClientProtocol *>(client.GetOwnerProtocol());
	if (!src || src == this)
		return true;	// our own call back to the other end
	if (src->GetNetwork() == m_Network)
		return true;	// the network has it already
	return client.GetProtocol() == EProtocol::mmdvmclient && IsMmdvmConnectionBlocked(src->GetInstance());
}

nlohmann::json COpenBridgeProtocol::Status(void) const
{
	nlohmann::json s = {{"name", m_Name}, {"target", m_MasterAddress + ":" + std::to_string(m_MasterPort)}, {"network", m_Network},
		{"network_id", m_NetworkId}, {"dialect", m_FreeDmr ? "freedmr" : "classic"}, {"stunned", m_Stunned.load()}};
	const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	s["last_heard_seconds"] = m_HeardFrom ? nlohmann::json(now - m_LastHeard.load()) : nlohmann::json(nullptr);
	return s;
}

std::shared_ptr<CClient> COpenBridgeProtocol::MakeModuleClient(const CCallsign &cs, const CIp &ip, char module)
{
	return std::make_shared<COpenBridgePeer>(cs, ip, module, this);
}

////////////////////////////////////////////////////////////////////////////////////////
// task

void COpenBridgeProtocol::Task(void)
{
	CBuffer Buffer;
	CIp     Ip;
	if (Receive4(Buffer, Ip, 20))
		HandleOpenBridge(Buffer, Ip);

	if (m_FreeDmr && !m_Stunned)
	{
		if (m_StartControl.exchange(false))
		{
			SendKeepalive();
			SendVersion();
		}
		if (m_KeepaliveTimer.time() >= OBP_KEEPALIVE_PERIOD)
			SendKeepalive();
		if (m_VersionTimer.time() >= OBP_VERSION_PERIOD)
			SendVersion();
	}
	// the target looked up again only while nothing comes from it: a lookup blocks
	// this thread, and a host with more addresses must not switch in a call
	if (m_ResolveTimer.time() >= OBP_RESOLVE_PERIOD && m_AliveTimer.time() >= OBP_ALIVE_TIMEOUT)
	{
		ResolveMaster();
		m_ResolveTimer.start();
	}
	if (m_HousekeepingTimer.time() >= 1.0)
	{
		m_HousekeepingTimer.start();
		// the link has no state of its own: a FreeDMR one is up while its
		// keepalives come, a classic one, without keepalives, for a while after
		// its last packet; its clients show it
		if (m_HeardFrom && m_AliveTimer.time() < (m_FreeDmr ? OBP_ALIVE_TIMEOUT : OBP_CLASSIC_IDLE))
		{
			CClients *clients = g_Reflector.GetClients();
			for (auto it = clients->begin(); it != clients->end(); it++)
				if ((*it)->GetOwnerProtocol() == this)
					(*it)->Alive();
			g_Reflector.ReleaseClients();
		}
		Expire(m_Ours, OBP_ENDED_WINDOW);
		Expire(m_Ended, OBP_ENDED_WINDOW);
		Expire(m_Quenched, OBP_QUENCH_WINDOW);	// else gone with the call's end
		// streams of the other end that went quiet without their terminator
		for (auto it = m_Heard.begin(); it != m_Heard.end();)
		{
			if (it->second.time() < OBP_STREAM_QUIET)
			{
				it++;
				continue;
			}
			const uint32_t slotStreamId = (it->first & 0x00FFFFFF) | (1U << 24);	// as OnDMRDPacketIn keys it
			m_IncomingStreams.erase(slotStreamId);
			m_IncomingStreams.erase((it->first & 0x00FFFFFF) | (2U << 24));
			it = m_Heard.erase(it);
		}
	}

	CheckStreamsTimeout();
	HandleQueue();
}

void COpenBridgeProtocol::Expire(std::map<uint32_t, CTimer> &ids, double seconds)
{
	for (auto it = ids.begin(); it != ids.end();)
		it = (it->second.time() > seconds) ? ids.erase(it) : std::next(it);
}

void COpenBridgeProtocol::Alive(void)
{
	m_AliveTimer.start();
	m_HeardFrom = true;
	m_LastHeard = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void COpenBridgeProtocol::Upgrade(const char *why)
{
	if (m_FreeDmr || !m_AutoUpgrade)
		return;
	m_FreeDmr = true;
	std::cout << m_Name << ": the other end speaks FreeDMR (" << why << "), the link turns FreeDMR" << std::endl;
	SendKeepalive();
	SendVersion();
}

////////////////////////////////////////////////////////////////////////////////////////
// authentication

std::string COpenBridgeProtocol::Key(void) const
{
	// FreeDMR (and HBlink3) cut the passphrase to 20 bytes and pad it with NULs.
	// The padding changes nothing for HMAC-SHA1 (it pads every key shorter than
	// its 64 byte block with NULs itself), the cut does for a longer passphrase;
	// for BLAKE2b, whose key length counts, both do
	if (!m_FreeDmr)
		return m_Passphrase;
	std::string key = m_Passphrase.substr(0, 20);
	key.resize(20, '\0');
	return key;
}

void COpenBridgeProtocol::Sha1(const uint8_t *data, size_t len, uint8_t *digest) const
{
	const std::string key = Key();
	unsigned int dlen = 20;
	HMAC(EVP_sha1(), key.data(), int(key.size()), data, len, digest, &dlen);
}

bool COpenBridgeProtocol::Sha1Ok(const uint8_t *data, size_t len, const uint8_t *hash) const
{
	uint8_t digest[20];
	Sha1(data, len, digest);
	if (0 == CRYPTO_memcmp(digest, hash, 20))
		return true;
	if (m_Passphrase.size() <= 20)
		return false;
	// a classic link to HBlink3 or FreeDMR, which use the first 20 characters only
	const std::string key = m_Passphrase.substr(0, 20);
	unsigned int dlen = 20;
	HMAC(EVP_sha1(), key.data(), int(key.size()), data, len, digest, &dlen);
	return 0 == CRYPTO_memcmp(digest, hash, 20);
}

bool COpenBridgeProtocol::Blake2b(const uint8_t *data, size_t len, uint8_t *digest) const
{
#if OPENSSL_VERSION_NUMBER < 0x30000000L
	(void)data; (void)len; (void)digest;
	return false;
#else
	if (!m_Blake)
		return false;
	std::string key = m_Passphrase.substr(0, 20);
	key.resize(20, '\0');
	size_t size = 16, out = 0;
	OSSL_PARAM params[] = {
		OSSL_PARAM_construct_size_t(OSSL_MAC_PARAM_SIZE, &size),
		OSSL_PARAM_construct_end()
	};
	EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(m_Blake);
	const bool ok = ctx
		&& EVP_MAC_init(ctx, (const unsigned char *)key.data(), key.size(), params)
		&& EVP_MAC_update(ctx, data, len)
		&& EVP_MAC_final(ctx, digest, &out, 16) && out == 16;
	EVP_MAC_CTX_free(ctx);
	return ok;
#endif
}

////////////////////////////////////////////////////////////////////////////////////////
// incoming

void COpenBridgeProtocol::HandleOpenBridge(const CBuffer &Buffer, const CIp &Ip)
{
	// only the other end: its packets are authenticated, but nobody else need be
	// heard; it may send from another port than it listens on
	CIp from(Ip);
	from.SetPort(m_MasterIp.GetPort());
	if (from != m_MasterIp)
		return;

	const uint8_t *d = Buffer.data();
	const size_t size = Buffer.size();
	if (size < 4)
		return;

	if (size == OBP_CLASSIC_LENGTH && 0 == memcmp(d, "DMRD", 4))
	{
		if (!Sha1Ok(d, OBP_DMRD_LENGTH, d + OBP_DMRD_LENGTH))
			return;
		uint32_t sid;
		memcpy(&sid, d + 16, 4);
		if (m_PeerId && Be32(d + 11) != m_PeerId)
		{
			if (!m_Ended.count(sid))
				std::cout << m_Name << ": packet with repeater ID " << Be32(d + 11) << " instead of PeerId " << m_PeerId << " dropped" << std::endl;
			m_Ended[sid].start();
			return;
		}
		TakeDMRD(d, sid);
	}
	else if (size >= 56 && 0 == memcmp(d, "DMRE", 4))
	{
		// the layout by the length: a FreeDMR set to version 4 may mark its
		// packets 5 all the same
		const uint8_t version = d[55];
		if ((size != OBP_DMRE4_LENGTH && size != OBP_DMRE5_LENGTH) || version < 4 || version > OBP_FREEDMR_VERSION)
			return;
		const size_t len = size;
		const bool v5 = (size == OBP_DMRE5_LENGTH);
		uint8_t digest[16];
		if (!Blake2b(d, len - 16, digest) || 0 != CRYPTO_memcmp(digest, d + len - 16, 16))
			return;
		Upgrade("DMRE");
		uint32_t sid;
		memcpy(&sid, d + 16, 4);
		if (m_PeerId && Be32(d + 11) != m_PeerId)
		{
			if (!m_Ended.count(sid))
				std::cout << m_Name << ": packet with repeater ID " << Be32(d + 11) << " instead of PeerId " << m_PeerId << " dropped" << std::endl;
			m_Ended[sid].start();
			return;
		}
		// too old (a clock off, or a long way round) or too many hops: the other
		// end stops sending it
		uint64_t ns = 0;
		for (int i = 56; i < 64; i++)
			ns = (ns << 8) | d[i];
		const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
		const unsigned hops = (v5 ? d[72] : d[68]) + 1U;
		if (ns + uint64_t(OBP_MAX_AGE) * 1000000000ULL < now || hops > OBP_MAX_HOPS)
		{
			if (!m_Ended.count(sid))
			{
				std::cout << m_Name << ": call on TG" << ((unsigned(d[8]) << 16) | (unsigned(d[9]) << 8) | d[10]) << " dropped, "
				          << (hops > OBP_MAX_HOPS ? "too many hops" : "its packets are more than 5 s old (is the clock synchronised?)") << std::endl;
				SendQuench(d + 8, d + 16);
			}
			m_Ended[sid].start();
			return;
		}
		TakeDMRD(d, sid);
	}
	else if (size == 24 && 0 == memcmp(d, "BCKA", 4))
	{
		if (Sha1Ok(d, 4, d + 4))
		{
			Alive();
			Upgrade("keepalive");
		}
	}
	else if (size == 25 && 0 == memcmp(d, "BCVE", 4))
	{
		if (Sha1Ok(d + 4, 1, d + 5) && d[4] >= 4)
			Upgrade("version");
	}
	else if (size == 31 && 0 == memcmp(d, "BCSQ", 4))
	{
		if (Sha1Ok(d, 11, d + 11))
		{
			uint32_t sid;
			memcpy(&sid, d + 7, 4);
			if (!m_Quenched.count(sid))
				std::cout << m_Name << ": the other end wants no more of our call on TG" << ((unsigned(d[4]) << 16) | (unsigned(d[5]) << 8) | d[6]) << " (source quench)" << std::endl;
			m_Quenched[sid].start();
		}
	}
	else if (size == 24 && 0 == memcmp(d, "BCST", 4))
	{
		if (Sha1Ok(d, 4, d + 4) && !m_Stunned)
		{
			m_Stunned = true;
			std::cout << m_Name << ": the other end stunned the link (BCST), nothing goes to it until urfd restarts" << std::endl;
		}
	}
}

void COpenBridgeProtocol::TakeDMRD(const uint8_t *dmrd, uint32_t streamId)
{
	Alive();
	if (m_Ours.count(streamId) || m_Ended.count(streamId))
		return;	// our own call come back, or the rest of one that ended
	const uint8_t flags = dmrd[15];
	const uint32_t tg = (uint32_t(dmrd[8]) << 16) | (uint32_t(dmrd[9]) << 8) | dmrd[10];
	if ((flags & 0x40) || !m_TGMap.IsTGMapped(tg))
	{
		// a private call (urfd has none) or a TG the link does not map: FreeDMR
		// sends every call of its bridges until it is told to stop
		SendQuench(dmrd + 8, dmrd + 16);
		m_Ended[streamId].start();
		return;
	}
	m_Heard[streamId].start();
	CBuffer buffer(const_cast<uint8_t *>(dmrd), OBP_DMRD_LENGTH);
	buffer.ReplaceAt(0, (const uint8_t *)"DMRD", 4);
	buffer.Append((uint8_t)0);	// BER and RSSI, as MMDVM has them
	buffer.Append((uint8_t)0);
	OnDMRDPacketIn(buffer);
	if ((flags & MMDVMCLI_FLAG_DATA_SYNC) && (flags & 0x0F) == DMR_DT_TERMINATOR_WITH_LC)
	{
		m_Ended[streamId].start();
		m_Heard.erase(streamId);
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// outgoing

void COpenBridgeProtocol::SendDMRD(CBuffer &buffer, char module)
{
	if (m_Stunned || buffer.size() < OBP_DMRD_LENGTH)
		return;
	uint8_t *d = buffer.data();
	uint32_t sid;
	memcpy(&sid, d + 16, 4);
	m_Ours[sid].start();
	if (m_Quenched.count(sid))
	{
		if ((d[15] & MMDVMCLI_FLAG_DATA_SYNC) && (d[15] & 0x0F) == DMR_DT_TERMINATOR_WITH_LC)
			m_Quenched.erase(sid);
		return;
	}

	const uint8_t flags = d[15];
	const bool header = (flags & MMDVMCLI_FLAG_DATA_SYNC) && (flags & 0x0F) == DMR_DT_VOICE_LC_HEADER;
	const bool last = (flags & MMDVMCLI_FLAG_DATA_SYNC) && (flags & 0x0F) == DMR_DT_TERMINATOR_WITH_LC;
	if (header || !m_SourceRptr.count(module))
	{
		// the hotspot or repeater the call comes from, where urfd knows it
		uint32_t rptr = 0;
		auto stream = g_Reflector.GetStream(module);
		auto owner = stream ? stream->GetOwnerClient() : nullptr;	// held while it is read
		if (auto *dmr = dynamic_cast<const CDmrmmdvmClient *>(owner.get()))
			rptr = dmr->GetRawDmrId();
		m_SourceRptr[module] = rptr;
	}
	const uint32_t rptr = m_SourceRptr[module];

	d[15] = flags & 0x7F;	// no timeslot on the wire
	// FreeDMR wants our ID here, and the repeater in its own field
	PutBe32(d + 11, (m_PreserveSourcePeer && !m_FreeDmr && rptr) ? rptr : m_NetworkId);

	if (!m_FreeDmr)
	{
		CBuffer out(d, OBP_DMRD_LENGTH);
		uint8_t digest[20];
		Sha1(d, OBP_DMRD_LENGTH, digest);
		out.Append(digest, 20);
		Send(out, m_MasterIp);
	}
	else
	{
		uint8_t p[OBP_DMRE5_LENGTH];
		memcpy(p, d, OBP_DMRD_LENGTH);
		memcpy(p, "DMRE", 4);
		p[53] = 0;	// BER
		p[54] = 0;	// RSSI
		p[55] = OBP_FREEDMR_VERSION;
		const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
		for (int i = 0; i < 8; i++)
			p[56 + i] = uint8_t(ns >> (56 - 8 * i));
		PutBe32(p + 64, m_SourceServer);	// the source server: we
		PutBe32(p + 68, rptr);
		p[72] = 1;	// hops
		if (!Blake2b(p, 73, p + 73))
			return;
		Send(CBuffer(p, OBP_DMRE5_LENGTH), m_MasterIp);
	}
	if (last)
	{
		m_SourceRptr.erase(module);
		m_Quenched.erase(sid);
	}
}

void COpenBridgeProtocol::SendControl(const char *opcode, const uint8_t *data, size_t len, bool hashOpcode)
{
	// FreeDMR's bridge control: its HMAC-SHA1 covers the opcode and data, but
	// BCVE's the version byte alone
	CBuffer p((uint8_t *)opcode, 4);
	if (len)
		p.Append(data, len);
	uint8_t digest[20];
	if (hashOpcode)
		Sha1(p.data(), p.size(), digest);
	else
		Sha1(data, len, digest);
	p.Append(digest, 20);
	Send(p, m_MasterIp);
}

void COpenBridgeProtocol::SendKeepalive(void)
{
	SendControl("BCKA", nullptr, 0, true);
	m_KeepaliveTimer.start();
}

void COpenBridgeProtocol::SendVersion(void)
{
	const uint8_t version = OBP_FREEDMR_VERSION;
	SendControl("BCVE", &version, 1, false);
	m_VersionTimer.start();
}

void COpenBridgeProtocol::SendQuench(const uint8_t *tg, const uint8_t *streamId)
{
	if (!m_FreeDmr)
		return;	// a classic end does not know it
	uint8_t data[7];
	memcpy(data, tg, 3);
	memcpy(data + 3, streamId, 4);
	SendControl("BCSQ", data, 7, true);
}
