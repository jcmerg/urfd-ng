// MMDVMClientProtocol -- Connects to a DMR master via the MMDVM protocol
// URFD acts as a virtual MMDVM repeater client to an MMDVM master server.
//
// Copyright (C) 2024-2026
// Licensed under the GNU General Public License v3 or later

#include <algorithm>
#include <string.h>
#include <cstdlib>
#include <sstream>
#include <set>

#include "Global.h"
#include "MMDVMClientPeer.h"
#include "MMDVMClientProtocol.h"
#include "BPTC19696.h"
#include "RS129.h"
#include "Golay2087.h"
#include "QR1676.h"

// DMR sync patterns
static uint8_t g_DmrSyncBSVoice[] = { 0x07,0x55,0xFD,0x7D,0xF7,0x5F,0x70 };
static uint8_t g_DmrSyncBSData[]  = { 0x0D,0xFF,0x57,0xD7,0x5D,0xF5,0xD0 };

// the name of a connection (MmdvmClientConfig.h): its section's for a named one
std::string MmdvmClientName(unsigned n)
{
	const std::string key = MmdvmClientNameKey(n);
	return g_Configure.Contains(key) ? g_Configure.GetString(key) : MmdvmClientDefaultName(n);
}

// "MMDVMClient1" ... "MMDVMClient9", or a connection's name (case does not matter)
unsigned MmdvmClientConnection(const std::string &name)
{
	if (name.size() == 12 && name.compare(0, 11, "MMDVMClient") == 0 && name[11] >= '1' && name[11] <= '0' + MMDVMCLI_MAX_INSTANCES)
		return unsigned(name[11] - '0');
	auto lower = [](std::string s) { for (auto &c : s) c = char(std::tolower((unsigned char)c)); return s; };
	const std::string wanted = lower(name);
	for (unsigned n = 2; n <= MMDVMCLI_MAX_INSTANCES; n++)
		if (g_Configure.Contains(MmdvmClientNameKey(n)) && lower(g_Configure.GetString(MmdvmClientNameKey(n))) == wanted)
			return n;
	return 0;
}

// Note: DMR_VOICE_LC_HEADER_CRC_MASK, DMR_TERMINATOR_WITH_LC_CRC_MASK,
// DMR_DT_VOICE_LC_HEADER, DMR_DT_TERMINATOR_WITH_LC are defined in Protocol.h

////////////////////////////////////////////////////////////////////////////////////////
// initialization

bool CMMDVMClientProtocol::Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6)
{
	m_uiDmrId = g_Configure.GetUnsigned(m_Keys.dmrid);
	// the modules tcd transcodes, fixed until a restart (the reflector's own copy is set by a reload only)
	m_FallbackDmrId = 0;
	if (g_Configure.Contains(m_Keys.fallbackdmrid))
		m_FallbackDmrId = g_Configure.GetUnsigned(m_Keys.fallbackdmrid);
	m_Password = g_Configure.GetString(m_Keys.password);
	m_Callsign = g_Configure.GetString(m_Keys.callsign);
	m_MasterPort = (uint16_t)g_Configure.GetUnsigned(m_Keys.port);

	// Store DMR ID as big-endian 4 bytes
	m_uiId[0] = (uint8_t)(m_uiDmrId >> 24);
	m_uiId[1] = (uint8_t)(m_uiDmrId >> 16);
	m_uiId[2] = (uint8_t)(m_uiDmrId >> 8);
	m_uiId[3] = (uint8_t)(m_uiDmrId);

	// Resolve master address
	m_MasterAddress = g_Configure.GetString(m_Keys.address);
	ResolveMaster();
	m_Network = MmdvmClientNetwork(g_Configure.Contains(m_Keys.network) ? g_Configure.GetString(m_Keys.network) : "", m_MasterAddress);

	// Parse BlockProtocols (comma-separated, e.g. "SvxReflector,YSF")
	ParseBlockProtocols(m_Keys.blockprotocols, m_Name);

	// Calls from MMDVMClient connections are judged by IsSourceClientBlocked:
	// never our own back to the master (that confuses BrandMeister and blocks
	// the other TS), never one from the same network

	// Save config defaults for runtime reset
	SaveBlockDefaults();

	// Load TG mappings (empty map is OK — dynamic TGs can be added via admin API)
	m_TGMap.LoadFromConfig(MmdvmClientPrefix(m_Instance), m_Name);

	// BrandMeister API (optional). A key belongs to the account and covers all its
	// hotspots, so a further connection in the network of [MMDVMClient] takes
	// that key when it has none: BrandMeister ignores the TGs in the login
	// options, only the API makes them static
	std::string apiKey;
	if (g_Configure.Contains(m_Keys.bmapikey))
		apiKey = g_Configure.GetString(m_Keys.bmapikey);
	else if (m_Instance > 1)
	{
		const auto first = MmdvmClientKeys(1);
		if (g_Configure.Contains(first.bmapikey) && g_Configure.Contains(first.address)
			&& MmdvmClientNetwork(g_Configure.Contains(first.network) ? g_Configure.GetString(first.network) : "", g_Configure.GetString(first.address)) == m_Network)
		{
			apiKey = g_Configure.GetString(first.bmapikey);
			std::cout << m_Name << ": using the BrandMeister API key of MMDVMClient" << std::endl;
		}
	}
	if (!apiKey.empty())
		m_BmApi.Configure(apiKey, m_uiDmrId);

	m_State = EHBState::DISCONNECTED;

	if (!CProtocol::Initialize(type, ptype, port, has_ipv4, has_ipv6))
		return false;

	m_RetryTimer.start();
	m_TimeoutTimer.start();
	m_PingTimer.start();

	std::cout << m_Name << ": initialized, DMR ID " << m_uiDmrId
	          << ", master " << m_MasterAddress << ":" << m_MasterPort << ", network " << m_Network << std::endl;

	return true;
}

bool CMMDVMClientProtocol::Transcoded(char module) const
{
	// an open stream keeps what it was given when it opened, even when tcd comes
	// or goes meanwhile (with Modules = auto); else the live state
	auto stream = g_Reflector.GetStream(module);
	if (stream && stream->IsOpen())
		return stream->IsTranscoding();
	return g_Reflector.IsTranscoded(module);
}

bool CMMDVMClientProtocol::IsSourceClientBlocked(const CClient &client) const
{
	if (client.GetProtocol() != EProtocol::mmdvmclient)
		return false;
	const auto *src = dynamic_cast<const CMMDVMClientProtocol *>(client.GetOwnerProtocol());
	if (!src || src == this)
		return true;	// our own call back to the master
	if (src->GetNetwork() == m_Network)
		return true;	// the network has it already
	// either connection names the other in its BlockProtocols or by an admin block
	return IsMmdvmConnectionBlocked(src->GetInstance()) || src->IsMmdvmConnectionBlocked(m_Instance);
}

// look the master up again, keeping the last address if that fails
void CMMDVMClientProtocol::ResolveMaster(void)
{
	// (a CIp built from a host name never reports IsSet(), so look at the address)
	CIp ip(m_MasterAddress.c_str(), AF_INET, SOCK_DGRAM, m_MasterPort);
	if (ip.AddressIsZero())
	{
		std::cerr << m_Name << ": cannot resolve master " << m_MasterAddress << std::endl;
		return;
	}
	CIp old(m_MasterIp);
	old.SetPort(m_MasterPort);
	if (!m_MasterIp.AddressIsZero() && (old != ip))
	{
		std::cout << m_Name << ": master " << m_MasterAddress << " is now at " << ip << std::endl;
		// our module clients carry the old address: the next call makes new ones
		CClients *clients = g_Reflector.GetClients();
		for (auto &mc : m_ModuleClients)
			if (mc.second && clients->IsClient(mc.second))
				clients->RemoveClient(mc.second);
		g_Reflector.ReleaseClients();
		m_ModuleClients.clear();
	}
	m_MasterIp = ip;
}

////////////////////////////////////////////////////////////////////////////////////////
// task

void CMMDVMClientProtocol::Task(void)
{
	CBuffer Buffer;
	CIp     Ip;

	if (Receive4(Buffer, Ip, 20))
	{
		HandleIncoming(Buffer, Ip);
	}

	// Handle admin-requested reconnect (for dynamic TG changes)
	if (m_ReconnectRequested.exchange(false))
	{
		std::cout << m_Name << ": reconnect requested (TG mapping changed)" << std::endl;
		SendClose();
		m_State = EHBState::DISCONNECTED;
		m_RetryTimer.start();
	}

	// Sync BrandMeister static TGs after the first connect, again later until it went through
	if (m_State == EHBState::RUNNING && m_BmApi.IsConfigured() && !m_BmSynced && std::chrono::steady_clock::now() >= m_BmNextSync)
	{
		if (SyncBrandMeisterTGs())
			m_BmSynced = true;
		else
		{
			std::cerr << m_Name << ": BrandMeister API sync incomplete, tried again in " << m_BmSyncDelay << " s (a new hotspot ID is known to BrandMeister only some minutes after its first login)" << std::endl;
			m_BmNextSync = std::chrono::steady_clock::now() + std::chrono::seconds(m_BmSyncDelay);
			m_BmSyncDelay = std::min(m_BmSyncDelay * 2, 1800);
		}
	}

	// Handle pending kerchunk (only when RUNNING — waits for reconnect to complete)
	if (m_State == EHBState::RUNNING && m_PendingKerchunk.load() != 0)
	{
		uint32_t kerchunkTG = m_PendingKerchunk.exchange(0);
		if (kerchunkTG != 0)
			SendKerchunk(kerchunkTG);
	}

	// Purge expired dynamic TGs periodically
	{
		auto expired = m_TGMap.PurgeExpired();
		if (!expired.empty())
		{
			std::cout << m_Name << ": " << expired.size() << " dynamic TG(s) expired, reconnecting" << std::endl;
			SendClose();
			m_State = EHBState::DISCONNECTED;
			m_RetryTimer.start();
		}
	}

	HandleStateMachine();
	CheckStreamsTimeout();

	if (m_State == EHBState::RUNNING)
	{
		HandleQueue();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// state machine

void CMMDVMClientProtocol::HandleStateMachine(void)
{
	switch (m_State)
	{
	case EHBState::DISCONNECTED:
		if (m_RetryTimer.time() > MMDVMCLI_RETRY_PERIOD)
		{
			// the master's address may have changed since the last login
			ResolveMaster();
			SendLogin();
			m_State = EHBState::WAITING_LOGIN;
			m_TimeoutTimer.start();
			m_RetryTimer.start();
		}
		break;

	case EHBState::WAITING_OPTIONS:
		// BrandMeister takes the options without an ACK (and asks for a beacon at
		// second 18 of a minute only): without a NAK they count as taken, so the
		// connection does not wait up to a minute without sending
		if (m_RetryTimer.time() > MMDVMCLI_OPTIONS_WAIT)
		{
			std::cout << m_Name << ": connected (options sent, the master confirms none)" << std::endl;
			m_State = EHBState::RUNNING;
			m_TimeoutTimer.start();
			m_PingTimer.start();
			// a master that sets its TGs from the options and lost them gets them
			// again, a few times, until it answers anything about them
			m_OptionsResends = MMDVMCLI_OPTIONS_RESENDS;
			m_RetryTimer.start();
		}
		break;

	case EHBState::WAITING_LOGIN:
	case EHBState::WAITING_AUTH:
	case EHBState::WAITING_CONFIG:
		if (m_RetryTimer.time() > MMDVMCLI_RETRY_PERIOD)
		{
			switch (m_State)
			{
			case EHBState::WAITING_LOGIN:  SendLogin();   break;
			case EHBState::WAITING_AUTH:   SendAuth();    break;
			case EHBState::WAITING_CONFIG: SendConfig();  break;
			default: break;
			}
			m_RetryTimer.start();
		}
		if (m_TimeoutTimer.time() > MMDVMCLI_TIMEOUT_PERIOD)
		{
			std::cout << m_Name << ": handshake timeout, reconnecting" << std::endl;
			m_State = EHBState::DISCONNECTED;
			m_RetryTimer.start();
		}
		break;

	case EHBState::RUNNING:
		if (m_PingTimer.time() > MMDVMCLI_PING_PERIOD)
		{
			SendPing();
			m_PingTimer.start();
		}
		if (m_OptionsResends > 0 && m_RetryTimer.time() > MMDVMCLI_RETRY_PERIOD)
		{
			SendOptions();
			m_OptionsResends--;
			m_RetryTimer.start();
		}
		if (m_TimeoutTimer.time() > MMDVMCLI_TIMEOUT_PERIOD)
		{
			std::cout << m_Name << ": keepalive timeout, reconnecting" << std::endl;
			m_State = EHBState::DISCONNECTED;
			m_RetryTimer.start();
		}
		break;
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// incoming packet handler

void CMMDVMClientProtocol::HandleIncoming(const CBuffer &Buffer, const CIp &Ip)
{
	// Only the master may talk to us: anyone else could otherwise take the
	// link over, with the password hash of the next login going to them
	CIp from(Ip);
	from.SetPort(m_MasterIp.GetPort());
	if (from != m_MasterIp)
		return;
	// the master may answer from another port than the configured one
	m_MasterIp = Ip;

	// RPTACK
	if (Buffer.size() >= 6 && 0 == Buffer.Compare((uint8_t *)"RPTACK", 6))
	{
		switch (m_State)
		{
		case EHBState::WAITING_LOGIN:
			if (Buffer.size() >= 10)
			{
				::memcpy(m_uiSalt, Buffer.data() + 6, 4);
				SendAuth();
				m_State = EHBState::WAITING_AUTH;
				m_TimeoutTimer.start();
				m_RetryTimer.start();
			}
			break;

		case EHBState::WAITING_AUTH:
			std::cout << m_Name << ": authentication successful" << std::endl;
			SendConfig();
			m_State = EHBState::WAITING_CONFIG;
			m_TimeoutTimer.start();
			m_RetryTimer.start();
			break;

		case EHBState::WAITING_CONFIG:
		{
			std::string opts = m_TGMap.GetOptionsString();
			if (!opts.empty())
			{
				std::cout << m_Name << ": config accepted, sending options: " << opts << std::endl;
				SendOptions();
				m_State = EHBState::WAITING_OPTIONS;
			}
			else
			{
				std::cout << m_Name << ": connected" << std::endl;
				m_State = EHBState::RUNNING;
			}
			m_TimeoutTimer.start();
			m_RetryTimer.start();
			m_PingTimer.start();
			break;
		}

		case EHBState::WAITING_OPTIONS:
			std::cout << m_Name << ": connected" << std::endl;
			m_State = EHBState::RUNNING;
			m_TimeoutTimer.start();
			m_PingTimer.start();
			break;

		default:
			m_OptionsResends = 0;	// RUNNING: the master answers the options late
			break;
		}
	}
	// RPTSBKN - beacon request (implicit ACK for options)
	else if (Buffer.size() >= 7 && 0 == Buffer.Compare((uint8_t *)"RPTSBKN", 7))
	{
		m_OptionsResends = 0;
		if (m_State == EHBState::WAITING_OPTIONS || m_State == EHBState::WAITING_CONFIG)
		{
			std::cout << m_Name << ": connected (beacon)" << std::endl;
			m_State = EHBState::RUNNING;
			m_TimeoutTimer.start();
			m_PingTimer.start();
		}
	}
	// MSTPONG
	else if (Buffer.size() >= 7 && 0 == Buffer.Compare((uint8_t *)"MSTPONG", 7))
	{
		m_TimeoutTimer.start();
		// keep this connection's clients alive, not another connection's whose master may be gone
		CClients *clients = g_Reflector.GetClients();
		for (auto it = clients->begin(); it != clients->end(); it++)
		{
			if ((*it)->GetOwnerProtocol() == this)
				(*it)->Alive();
		}
		g_Reflector.ReleaseClients();
	}
	// MSTNAK
	else if (Buffer.size() >= 6 && 0 == Buffer.Compare((uint8_t *)"MSTNAK", 6))
	{
		std::cout << m_Name << ": NAK in state " << (int)m_State.load() << std::endl;
		if (m_State == EHBState::RUNNING)
		{
			m_State = EHBState::WAITING_LOGIN;
			SendLogin();
			m_RetryTimer.start();
			m_TimeoutTimer.start();
		}
		else
		{
			m_State = EHBState::DISCONNECTED;
			m_RetryTimer.start();
		}
	}
	// MSTCL
	else if (Buffer.size() >= 5 && 0 == Buffer.Compare((uint8_t *)"MSTCL", 5))
	{
		if (m_State == EHBState::WAITING_CONFIG)
			std::cout << m_Name << ": the master refused the configuration and closed the connection (BrandMeister does so for a duplex hotspot with RxFreq = TxFreq)" << std::endl;
		else
			std::cout << m_Name << ": master closed connection" << std::endl;
		m_State = EHBState::DISCONNECTED;
		m_RetryTimer.start();
	}
	// DMRD
	else if (Buffer.size() == MMDVMCLI_DATA_PACKET_LENGTH && 0 == Buffer.Compare((uint8_t *)"DMRD", 4))
	{
		m_OptionsResends = 0;	// traffic: the master has taken the options
		if (m_State == EHBState::WAITING_OPTIONS)
		{
			m_State = EHBState::RUNNING;
			m_TimeoutTimer.start();
			m_PingTimer.start();
		}
		if (m_State == EHBState::RUNNING)
		{
			OnDMRDPacketIn(Buffer);
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// auth packet helpers

void CMMDVMClientProtocol::SendLogin(void)
{
	m_OptionsResends = 0;	// a new login sends the options in its handshake
	CBuffer buf;
	buf.Set((uint8_t *)"RPTL", 4);
	buf.Append(m_uiId, 4);
	Send(buf, m_MasterIp);
}

void CMMDVMClientProtocol::SendAuth(void)
{
	size_t pwlen = m_Password.size();
	std::vector<uint8_t> input(4 + pwlen);
	::memcpy(input.data(), m_uiSalt, 4);
	::memcpy(input.data() + 4, m_Password.c_str(), pwlen);

	uint8_t digest[32];
	m_SHA256.buffer(input.data(), input.size(), digest);

	CBuffer buf;
	buf.Set((uint8_t *)"RPTK", 4);
	buf.Append(m_uiId, 4);
	buf.Append(digest, 32);
	Send(buf, m_MasterIp);
}

void CMMDVMClientProtocol::SendConfig(void)
{
	// Read config values with defaults. A further connection takes what its
	// section leaves out from [MMDVMClient]: BrandMeister closed one that sent
	// the bare defaults right after its config.
	const auto first = MmdvmClientKeys(1);
	// the key that has the value: this section's, else [MMDVMClient]'s; nullptr for neither
	auto pick = [&](const std::string &own, const std::string &fallback) -> const std::string * {
		if (g_Configure.Contains(own))
			return &own;
		return (m_Instance > 1 && g_Configure.Contains(fallback)) ? &fallback : nullptr;
	};
	double lat = 0.0, lon = 0.0;
	// a split by default: the hotspot is duplex, and BrandMeister refuses a duplex
	// hotspot with RX = TX
	unsigned rxfreq = 430412500, txfreq = 439812500;
	std::string location, description, url;

	if (const auto *k = pick(m_Keys.latitude, first.latitude))
		lat = std::stod(g_Configure.GetString(*k));
	if (const auto *k = pick(m_Keys.longitude, first.longitude))
		lon = std::stod(g_Configure.GetString(*k));
	if (const auto *k = pick(m_Keys.rxfreq, first.rxfreq))
		rxfreq = g_Configure.GetUnsigned(*k);
	if (const auto *k = pick(m_Keys.txfreq, first.txfreq))
		txfreq = g_Configure.GetUnsigned(*k);
	if (const auto *k = pick(m_Keys.location, first.location))
		location = g_Configure.GetString(*k);
	if (const auto *k = pick(m_Keys.description, first.description))
		description = g_Configure.GetString(*k);
	if (const auto *k = pick(m_Keys.url, first.url))
		url = g_Configure.GetString(*k);

	char config[400];
	::memset(config, 0, sizeof(config));

	int n = ::snprintf(config, sizeof(config),
		"%-8.8s%09u%09u%02u%02u%+08.4f%+09.4f%03d%-20.20s%-19.19s%c%-124.124s%-40.40s%-40.40s",
		m_Callsign.c_str(), rxfreq, txfreq,
		1U, (unsigned)MMDVMCLI_COLOUR_CODE,
		lat, lon, 0,
		location.empty() ? "URFD Reflector" : location.c_str(),
		description.empty() ? g_Configure.GetString(g_Keys.names.callsign).c_str() : description.c_str(),
		'3',  // duplex (required by master)
		url.c_str(),
		g_Configure.Contains(m_Keys.firmware) ? g_Configure.GetString(m_Keys.firmware).c_str() : "20261031_URFD",
		g_Configure.Contains(m_Keys.software) ? g_Configure.GetString(m_Keys.software).c_str() : "MMDVM_URFD_Virtual"
	);

	CBuffer buf;
	buf.Set((uint8_t *)"RPTC", 4);
	buf.Append(m_uiId, 4);
	buf.Append((uint8_t *)config, n);
	Send(buf, m_MasterIp);
}

void CMMDVMClientProtocol::SendOptions(void)
{
	std::string opts = m_TGMap.GetOptionsString();
	if (opts.empty())
		return;

	CBuffer buf;
	buf.Set((uint8_t *)"RPTO", 4);
	buf.Append(m_uiId, 4);
	buf.Append((uint8_t *)opts.c_str(), opts.size());
	Send(buf, m_MasterIp);
}

void CMMDVMClientProtocol::SendPing(void)
{
	CBuffer buf;
	buf.Set((uint8_t *)"RPTPING", 7);
	buf.Append(m_uiId, 4);
	Send(buf, m_MasterIp);
}

void CMMDVMClientProtocol::SendClose(void)
{
	CBuffer buf;
	buf.Set((uint8_t *)"RPTCL", 5);
	buf.Append(m_uiId, 4);
	Send(buf, m_MasterIp);
}

void CMMDVMClientProtocol::SendKerchunk(uint32_t tg)
{
	// Send a minimal Voice LC Header + Terminator on the given TG
	// This activates the TG on BrandMeister (dynamic subscription)
	uint8_t ts = m_TGMap.TGToTimeslot(tg);
	uint8_t slotFlag = (ts == 1) ? MMDVMCLI_FLAG_SLOT1 : MMDVMCLI_FLAG_SLOT2;
	uint32_t streamId = (uint32_t)::rand();

	// Source ID must be a valid 24-bit DMR user ID (not the 9-digit repeater ID)
	// Look up the callsign's personal DMR ID, fall back to FallbackDmrId
	CCallsign cs;
	cs.SetCallsign(m_Callsign, false);
	uint32_t srcId = CallsignToDmrId(cs);
	if (srcId == 0)
		srcId = m_FallbackDmrId;
	if (srcId == 0 || srcId > 16777215)
	{
		std::cerr << m_Name << ": kerchunk aborted - no valid 24-bit DMR ID available" << std::endl;
		return;
	}

	// Voice LC Header
	{
		CBuffer buf;
		buf.Set((uint8_t *)"DMRD", 4);
		buf.Append((uint8_t)0);  // seq
		buf.Append((uint8_t)(srcId >> 16));
		buf.Append((uint8_t)(srcId >> 8));
		buf.Append((uint8_t)(srcId));
		buf.Append((uint8_t)(tg >> 16));
		buf.Append((uint8_t)(tg >> 8));
		buf.Append((uint8_t)(tg));
		buf.Append(m_uiId, 4);
		buf.Append((uint8_t)(slotFlag | MMDVMCLI_FLAG_DATA_SYNC | DMR_DT_VOICE_LC_HEADER));
		buf.Append((uint8_t *)&streamId, 4);
		AppendVoiceLCToBuffer(&buf, srcId, tg);
		buf.Append((uint8_t)0);
		buf.Append((uint8_t)0x32);
		Send(buf, m_MasterIp);
	}

	// Send a few silent AMBE voice frames (BM may ignore header-only kerchunks)
	{
		// Silent AMBE frame (DMR silence pattern)
		static const uint8_t silentAmbe[9] = { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };
		uint8_t pktSeq = 1;
		for (int voiceSeq = 0; voiceSeq < 3; voiceSeq++)
		{
			CBuffer buf;
			buf.Set((uint8_t *)"DMRD", 4);
			buf.Append(pktSeq++);
			buf.Append((uint8_t)(srcId >> 16));
			buf.Append((uint8_t)(srcId >> 8));
			buf.Append((uint8_t)(srcId));
			buf.Append((uint8_t)(tg >> 16));
			buf.Append((uint8_t)(tg >> 8));
			buf.Append((uint8_t)(tg));
			buf.Append(m_uiId, 4);

			uint8_t flags;
			if (voiceSeq == 0)
				flags = slotFlag | MMDVMCLI_FLAG_VOICE_SYNC | 0;
			else
				flags = slotFlag | voiceSeq;
			buf.Append(flags);
			buf.Append((uint8_t *)&streamId, 4);

			// 33-byte payload: 3x silent AMBE frames as a voice burst, the middle
			// one around the sync or EMB, as EncodeDMRDVoice lays them out
			uint8_t payload[33];
			::memset(payload, 0, sizeof(payload));
			::memcpy(payload, silentAmbe, 9);
			::memcpy(payload + 9, silentAmbe, 5);
			payload[13] &= 0xF0;
			::memcpy(payload + 19, silentAmbe + 4, 5);
			payload[19] &= 0x0F;
			::memcpy(payload + 24, silentAmbe, 9);
			buf.Append(payload, 33);
			ReplaceEMBInBuffer(&buf, (uint8_t)voiceSeq);
			buf.Append((uint8_t)0);
			buf.Append((uint8_t)0x32);
			Send(buf, m_MasterIp);
			usleep(60000);  // 60ms between frames
		}
	}

	// Terminator with LC
	{
		CBuffer buf;
		buf.Set((uint8_t *)"DMRD", 4);
		buf.Append((uint8_t)4);  // seq (after 3 voice frames)
		buf.Append((uint8_t)(srcId >> 16));
		buf.Append((uint8_t)(srcId >> 8));
		buf.Append((uint8_t)(srcId));
		buf.Append((uint8_t)(tg >> 16));
		buf.Append((uint8_t)(tg >> 8));
		buf.Append((uint8_t)(tg));
		buf.Append(m_uiId, 4);
		buf.Append((uint8_t)(slotFlag | MMDVMCLI_FLAG_DATA_SYNC | DMR_DT_TERMINATOR_WITH_LC));
		buf.Append((uint8_t *)&streamId, 4);
		AppendTerminatorLCToBuffer(&buf, srcId, tg);
		buf.Append((uint8_t)0);
		buf.Append((uint8_t)0x32);
		Send(buf, m_MasterIp);
	}

	std::cout << m_Name << ": kerchunk sent on TG" << tg << " srcId=" << srcId << " (stream 0x" << std::hex << streamId << std::dec << ")" << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////
// DMRD incoming: Master -> Reflector

void CMMDVMClientProtocol::OnDMRDPacketIn(const CBuffer &Buffer)
{
	uint32_t srcId = ((uint32_t)Buffer.data()[5] << 16) |
	                 ((uint32_t)Buffer.data()[6] << 8) |
	                  (uint32_t)Buffer.data()[7];
	uint32_t dstId = ((uint32_t)Buffer.data()[8] << 16) |
	                 ((uint32_t)Buffer.data()[9] << 8) |
	                  (uint32_t)Buffer.data()[10];
	uint32_t streamId;
	::memcpy(&streamId, Buffer.data() + 16, 4);
	if (streamId == 0) streamId = 1;

	uint8_t flags = Buffer.data()[15];
	uint8_t slot = (flags & MMDVMCLI_FLAG_SLOT2) ? 2 : 1;

	if (!m_TGMap.IsTGMapped(dstId))
		return;

	// Combine streamId with slot to allow parallel streams on different timeslots
	// Master may reuse the same streamId for both slots simultaneously
	uint32_t slotStreamId = (streamId & 0x00FFFFFF) | ((uint32_t)slot << 24);

	bool isDataSync = (flags & MMDVMCLI_FLAG_DATA_SYNC) != 0;
	uint8_t dataType = flags & 0x0F;

	if (isDataSync)
	{
		if (dataType == DMR_DT_VOICE_LC_HEADER)
			OnDMRDVoiceHeaderIn(Buffer, srcId, dstId, slotStreamId);
		else if (dataType == DMR_DT_TERMINATOR_WITH_LC)
			OnDMRDTerminatorIn(Buffer, srcId, dstId, slotStreamId);
	}
	else
	{
		OnDMRDVoiceFrameIn(Buffer, srcId, dstId, slotStreamId);
	}
}

void CMMDVMClientProtocol::OnDMRDVoiceHeaderIn(const CBuffer &Buffer, uint32_t srcId, uint32_t dstId, uint32_t streamId)
{
	char module = m_TGMap.TGToModule(dstId);
	if (module == ' ')
		return;

	// Refresh dynamic TG TTL on activity
	m_TGMap.RefreshActivity(dstId);

	CCallsign rpt1;
	rpt1.SetCallsign(m_Callsign, false);
	rpt1.SetCSModule(module);
	CCallsign rpt2(g_Reflector.GetCallsign());
	rpt2.SetCSModule(module);

	// Check if this master streamId is already mapped
	bool retry = false;
	auto existing = m_IncomingStreams.find(streamId);
	if (existing != m_IncomingStreams.end())
	{
		if (existing->second == 0)
		{
			// rejected while the module was busy: the previous stream closes
			// a moment after its last frame, and a talker who starts within
			// that moment would otherwise be lost for the whole call, so try
			// again with every frame until the module is free
			retry = true;
		}
		else
		{
			// Already mapped - tickle the stream if it still exists
			auto stream = GetStream(existing->second, &m_MasterIp);
			if (stream)
			{
				stream->Tickle();
				return;
			}
			// Stream closed - remove stale mapping so we can re-open
			m_IncomingStreams.erase(existing);
		}
	}

	// Generate unique URFD stream ID
	static uint16_t s_nextId = 0x100;
	uint16_t urfStreamId = s_nextId;
	s_nextId += 0x100;
	if (s_nextId == 0) s_nextId = 0x100;

	CCallsign my = DmrIdToCallsign(srcId);
	if (!retry)
	{
		std::ostringstream who;
		if (my.IsValid())
			who << my;
		else
			who << "unknown";
		std::cout << m_Name << ": voice from " << who.str() << " (ID " << srcId << ") TG" << dstId << " -> Module " << module << std::endl;
	}

	auto header = std::unique_ptr<CDvHeaderPacket>(new CDvHeaderPacket(srcId, CCallsign("CQCQCQ"), rpt1, rpt2, urfStreamId, 0, 0));
	// a source without a callsign (an ID not in the DMR ID database yet, or none
	// at all, e.g. from a mode without IDs on the master) gets a neutral MY with
	// its ID: never an empty one, which the other modes would show and send as
	// such, and not ours, which would look like we were talking
	if (!header->GetMyCallsign().IsValid())
	{
		header->SetMyCallsign(CCallsign(UNKNOWN_CALLSIGN, srcId));
		// its talker alias names it for its next call
		if (srcId != 0 && !m_UnknownTA.count(streamId))
		{
			if (m_UnknownTA.size() >= 16)
				m_UnknownTA.clear();	// streams that ended without a terminator
			m_UnknownTA[streamId].first = srcId;
		}
	}

	m_IncomingStreams[streamId] = urfStreamId;

	OnDvHeaderPacketIn(header, m_MasterIp);

	// Check if stream was actually opened, mark as rejected if not
	auto stream = GetStream(urfStreamId, &m_MasterIp);
	if (!stream)
		m_IncomingStreams[streamId] = 0; // sentinel: rejected
	else
		stream->SetSourceTG(dstId);
}

void CMMDVMClientProtocol::OnDMRDVoiceFrameIn(const CBuffer &Buffer, uint32_t srcId, uint32_t dstId, uint32_t streamId)
{
	if (m_UnknownTA.count(streamId) && !(Buffer.data()[15] & MMDVMCLI_FLAG_VOICE_SYNC))
	{
		const uint8_t *frame = Buffer.data() + 20;
		uint8_t sync[7];
		sync[0] = frame[13] & 0x0F;
		::memcpy(sync + 1, frame + 14, 5);
		sync[6] = frame[19] & 0xF0;
		LearnTalkerAlias(streamId, Buffer.data()[15] & 0x0F, sync);
	}

	auto it = m_IncomingStreams.find(streamId);
	if (it == m_IncomingStreams.end())
	{
		OnDMRDVoiceHeaderIn(Buffer, srcId, dstId, streamId);
		it = m_IncomingStreams.find(streamId);
		if (it == m_IncomingStreams.end())
			return;
	}

	if (it->second == 0)
	{
		// rejected so far, the module may be free by now
		OnDMRDVoiceHeaderIn(Buffer, srcId, dstId, streamId);
		it = m_IncomingStreams.find(streamId);
		if (it == m_IncomingStreams.end() || it->second == 0)
			return;
	}

	uint16_t urfStreamId = it->second;
	const uint8_t *dmrframe = Buffer.data() + 20;

	// Extract 3 x 9-byte AMBE2+ from interleaved DMR frame
	uint8_t dmr3ambe[27];
	::memcpy(dmr3ambe, dmrframe, 13);
	// the middle frame: the burst's byte 13 holds its bits 32..35 in the high
	// nibble (the low nibble starts the sync), byte 19 its bits 36..39 in the low
	dmr3ambe[13] = (dmrframe[13] & 0xF0) | (dmrframe[19] & 0x0F);
	::memcpy(dmr3ambe + 14, dmrframe + 20, 13);

	uint8_t dmrsync[7];
	dmrsync[0] = dmrframe[13] & 0x0F;
	::memcpy(dmrsync + 1, dmrframe + 14, 5);
	dmrsync[6] = dmrframe[19] & 0xF0;

	uint8_t flags = Buffer.data()[15];
	uint8_t pid = flags & 0x0F;
	if (flags & MMDVMCLI_FLAG_VOICE_SYNC)
		pid = 0;

	// Push frames directly to the stream
	auto stream = GetStream(urfStreamId, &m_MasterIp);
	if (!stream)
		return;

	for (int i = 0; i < 3; i++)
	{
		auto frame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(
			&dmr3ambe[i * 9], dmrsync,
			urfStreamId, (uint8_t)(pid % 6), (uint8_t)(i + 1), false));
		frame->SetPacketModule(stream->GetOwnerClient()->GetReflectorModule());
		stream->Push(std::move(frame));
	}
}

void CMMDVMClientProtocol::OnDMRDTerminatorIn(const CBuffer &Buffer, uint32_t srcId, uint32_t dstId, uint32_t streamId)
{
	m_UnknownTA.erase(streamId);
	auto it = m_IncomingStreams.find(streamId);
	if (it == m_IncomingStreams.end())
		return;

	uint16_t urfStreamId = it->second;
	m_IncomingStreams.erase(it);

	auto stream = GetStream(urfStreamId, &m_MasterIp);
	if (!stream)
		return;

	uint8_t silence[9] = { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };
	uint8_t sync[7] = { 0 };
	auto frame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(
		silence, sync, urfStreamId, 0, 0, true));
	frame->SetPacketModule(stream->GetOwnerClient()->GetReflectorModule());
	stream->Push(std::move(frame));
}

////////////////////////////////////////////////////////////////////////////////////////
// stream helper for incoming

void CMMDVMClientProtocol::OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip)
{
	auto stream = GetStream(Header->GetStreamId(), &m_MasterIp);
	if (stream)
	{
		stream->Tickle();
		return;
	}

	CCallsign my(Header->GetMyCallsign());
	CCallsign rpt1(Header->GetRpt1Callsign());
	CCallsign rpt2(Header->GetRpt2Callsign());

	CClients *clients = g_Reflector.GetClients();
	char module = Header->GetRpt2Module();
	CCallsign cs;
	cs.SetCallsign(m_Callsign, false);
	cs.SetCSModule(module);
	// our own client of the module: FindClient could return another connection's,
	// which may have the same callsign and master
	std::shared_ptr<CClient> &client = m_ModuleClients[module];
	if (!client || !clients->IsClient(client))
	{
		client = std::make_shared<CMMDVMClientPeer>(cs, Ip, module, this);
		clients->AddClient(client);
	}

	if (client)
	{
		client->Alive();
		client->SetReflectorModule(Header->GetRpt2Module());
		if ((stream = g_Reflector.OpenStream(Header, client)) != nullptr)
		{
			m_Streams[stream->GetStreamId()] = stream;
		}
	}

	g_Reflector.ReleaseClients();

	g_Reflector.GetUsers()->Hearing(my, rpt1, rpt2, m_Name.c_str());
	g_Reflector.ReleaseUsers();
}

////////////////////////////////////////////////////////////////////////////////////////
// outbound queue: Reflector -> Master

uint8_t CMMDVMClientProtocol::SlotFlag(char module) const
{
	return (m_TGMap.ModuleToTimeslot(module) == 2) ? MMDVMCLI_FLAG_SLOT2 : MMDVMCLI_FLAG_SLOT1;
}

void CMMDVMClientProtocol::HandleQueue(void)
{
	while (!m_Queue.IsEmpty())
	{
		auto packet = m_Queue.Pop();

		if (packet->IsDvHeader())
		{
			auto &header = (CDvHeaderPacket &)*packet;
			char module = header.GetPacketModule();

			uint32_t tg = m_TGMap.ModuleToTG(module);
			if (tg == 0)
				continue;

			// Resolve user DMR ID
			uint32_t userSrcId = CallsignToDmrId(header.GetMyCallsign());
			if (userSrcId == 0U)
				userSrcId = header.GetMyCallsign().GetDmrid();
			if (userSrcId == 0U)
			{
				if (m_FallbackDmrId != 0)
				{
					userSrcId = m_FallbackDmrId;
					std::cout << m_Name << ": using fallback DMR ID " << m_FallbackDmrId << " for " << header.GetMyCallsign() << std::endl;
				}
				else
				{
					std::cout << m_Name << ": dropping stream from " << header.GetMyCallsign() << " - no DMR ID found" << std::endl;
					continue;
				}
			}

			SMMDVMClientStreamCache cache;
			cache.header = header;
			cache.frameCount = 0;
			cache.seqNo = 0;
			cache.pktSeqNo = 0;
			cache.streamId = (uint32_t)::rand();
			cache.srcId = userSrcId;
			// without tcd the module carries the source's codec: only AMBE+2 (DMR, YSF,
			// NXDN) may go to the master, and only the first frame tells
			cache.held = !Transcoded(module);
			m_OutboundCache[module] = cache;
			if (cache.held)
				continue;

			CBuffer buf;
			if (EncodeDMRDHeader(header, module, buf))
			{
				Send(buf, m_MasterIp);
			}
		}
		else if (packet->IsDvFrame())
		{
			auto &frame = (CDvFramePacket &)*packet;
			char module = frame.GetPacketModule();

			auto it = m_OutboundCache.find(module);
			if (it == m_OutboundCache.end())
			{
				// Late entry: create cache and send header
				uint32_t tg = m_TGMap.ModuleToTG(module);
				if (tg == 0)
					continue;
				if (!Transcoded(module) && frame.GetCodecIn() != ECodecType::dmr)
					continue;

				// Try to resolve DMR ID from the active stream's callsign
				uint32_t srcId = 0;
				auto stream = g_Reflector.GetStream(module);
				if (stream)
				{
					CCallsign cs = stream->GetUserCallsign();
					srcId = cs.GetDmrid();
					if (srcId == 0U)
						srcId = CallsignToDmrId(cs);
				}
				if (srcId == 0U)
					srcId = m_FallbackDmrId;
				if (srcId == 0)
				{
					static std::set<char> warned;
					if (warned.insert(module).second)
						std::cout << m_Name << ": dropping late-entry frames on module " << module << " - no DMR ID" << std::endl;
					continue;
				}

				SMMDVMClientStreamCache cache;
				cache.frameCount = 0;
				cache.seqNo = 0;
				cache.pktSeqNo = 0;
				cache.streamId = (uint32_t)::rand();
				cache.srcId = srcId;
				m_OutboundCache[module] = cache;
				it = m_OutboundCache.find(module);

				CBuffer buf;
				buf.Set((uint8_t *)"DMRD", 4);
				buf.Append(cache.pktSeqNo++);
				buf.Append((uint8_t)(srcId >> 16));
				buf.Append((uint8_t)(srcId >> 8));
				buf.Append((uint8_t)(srcId));
				buf.Append((uint8_t)(tg >> 16));
				buf.Append((uint8_t)(tg >> 8));
				buf.Append((uint8_t)(tg));
				buf.Append(m_uiId, 4);
				buf.Append((uint8_t)(SlotFlag(module) | MMDVMCLI_FLAG_DATA_SYNC | DMR_DT_VOICE_LC_HEADER));
				buf.Append((uint8_t *)&cache.streamId, 4);
				AppendVoiceLCToBuffer(&buf, srcId, tg);
				buf.Append((uint8_t)0);
				buf.Append((uint8_t)0x32);
				Send(buf, m_MasterIp);
			}

			auto &c = it->second;

			if (c.held && !c.local)
			{
				if (frame.GetCodecIn() == ECodecType::dmr)
				{
					c.held = false;
					CBuffer hb;
					if (EncodeDMRDHeader(c.header, module, hb))
						Send(hb, m_MasterIp);
				}
				else
				{
					c.local = true;
					std::cout << m_Name << ": stream of " << c.header.GetMyCallsign() << " on module " << module
					          << " stays local, it is not AMBE+2 and the module is not transcoded" << std::endl;
				}
			}
			if (c.local)
			{
				if (frame.IsLastPacket())
					m_OutboundCache.erase(it);
				continue;
			}

			if (frame.IsLastPacket())
			{
				CBuffer buf;
				if (EncodeDMRDTerminator(c.streamId, module, buf))
					Send(buf, m_MasterIp);
				m_OutboundCache.erase(it);
				continue;
			}

			// Buffer frames in triplets (3 AMBE frames per DMRD packet)
			c.frames[c.frameCount++] = frame;

			if (c.frameCount >= 3)
			{
				CBuffer buf;
				if (EncodeDMRDVoiceFrame(c.frames[0], c.frames[1], c.frames[2],
				                          c.seqNo, c.streamId, module, buf))
				{
					Send(buf, m_MasterIp);
				}
				c.seqNo++;
				c.frameCount = 0;
			}
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// DMRD encoding: Reflector -> Master

bool CMMDVMClientProtocol::EncodeDMRDHeader(const CDvHeaderPacket &header, char module, CBuffer &buffer)
{
	uint32_t tg = m_TGMap.ModuleToTG(module);
	auto it = m_OutboundCache.find(module);
	if (it == m_OutboundCache.end())
		return false;

	uint32_t srcId = it->second.srcId;
	uint32_t streamId = it->second.streamId;

	buffer.Set((uint8_t *)"DMRD", 4);
	buffer.Append(it->second.pktSeqNo++);
	buffer.Append((uint8_t)(srcId >> 16));
	buffer.Append((uint8_t)(srcId >> 8));
	buffer.Append((uint8_t)(srcId));
	buffer.Append((uint8_t)(tg >> 16));
	buffer.Append((uint8_t)(tg >> 8));
	buffer.Append((uint8_t)(tg));
	buffer.Append(m_uiId, 4);
	buffer.Append((uint8_t)(SlotFlag(module) | MMDVMCLI_FLAG_DATA_SYNC | DMR_DT_VOICE_LC_HEADER));
	buffer.Append((uint8_t *)&streamId, 4);
	AppendVoiceLCToBuffer(&buffer, srcId, tg);
	buffer.Append((uint8_t)0);
	buffer.Append((uint8_t)0x32);

	return true;
}

bool CMMDVMClientProtocol::EncodeDMRDVoiceFrame(
	const CDvFramePacket &frame0, const CDvFramePacket &frame1, const CDvFramePacket &frame2,
	uint8_t seqNo, uint32_t streamId, char module, CBuffer &buffer)
{
	uint32_t tg = m_TGMap.ModuleToTG(module);
	auto cit = m_OutboundCache.find(module);
	uint32_t srcId = (cit != m_OutboundCache.end()) ? cit->second.srcId : m_uiDmrId;

	buffer.Set((uint8_t *)"DMRD", 4);
	buffer.Append(cit->second.pktSeqNo++);
	buffer.Append((uint8_t)(srcId >> 16));
	buffer.Append((uint8_t)(srcId >> 8));
	buffer.Append((uint8_t)(srcId));
	buffer.Append((uint8_t)(tg >> 16));
	buffer.Append((uint8_t)(tg >> 8));
	buffer.Append((uint8_t)(tg));
	buffer.Append(m_uiId, 4);

	uint8_t voiceSeq = seqNo % 6;
	uint8_t flags = SlotFlag(module);
	flags |= (voiceSeq == 0) ? MMDVMCLI_FLAG_VOICE_SYNC : voiceSeq;
	buffer.Append(flags);

	buffer.Append((uint8_t *)&streamId, 4);

	// Build 33-byte DMR voice payload from 3 x 9 AMBE frames
	buffer.resize(53);
	::memset(buffer.data() + 20, 0, 33);
	buffer.ReplaceAt(20, frame0.GetCodecData(ECodecType::dmr), 9);
	buffer.ReplaceAt(29, frame1.GetCodecData(ECodecType::dmr), 5);
	buffer.ReplaceAt(33, (uint8_t)(buffer.at(33) & 0xF0));
	buffer.ReplaceAt(39, frame1.GetCodecData(ECodecType::dmr) + 4, 5);
	buffer.ReplaceAt(39, (uint8_t)(buffer.at(39) & 0x0F));
	buffer.ReplaceAt(44, frame2.GetCodecData(ECodecType::dmr), 9);
	ReplaceEMBInBuffer(&buffer, voiceSeq);

	buffer.Append((uint8_t)0);
	buffer.Append((uint8_t)0x32);

	return true;
}

bool CMMDVMClientProtocol::EncodeDMRDTerminator(uint32_t streamId, char module, CBuffer &buffer)
{
	uint32_t tg = m_TGMap.ModuleToTG(module);

	buffer.Set((uint8_t *)"DMRD", 4);
	buffer.Append((uint8_t)0);
	buffer.Append((uint8_t)(m_uiDmrId >> 16));
	buffer.Append((uint8_t)(m_uiDmrId >> 8));
	buffer.Append((uint8_t)(m_uiDmrId));
	buffer.Append((uint8_t)(tg >> 16));
	buffer.Append((uint8_t)(tg >> 8));
	buffer.Append((uint8_t)(tg));
	buffer.Append(m_uiId, 4);
	buffer.Append((uint8_t)(SlotFlag(module) | MMDVMCLI_FLAG_DATA_SYNC | DMR_DT_TERMINATOR_WITH_LC));
	buffer.Append((uint8_t *)&streamId, 4);
	AppendTerminatorLCToBuffer(&buffer, m_uiDmrId, tg);
	buffer.Append((uint8_t)0);
	buffer.Append((uint8_t)0x32);

	return true;
}

////////////////////////////////////////////////////////////////////////////////////////
// DMR frame construction helpers (adapted from CDmrmmdvmProtocol)

void CMMDVMClientProtocol::AppendVoiceLCToBuffer(CBuffer *buffer, uint32_t srcId, uint32_t dstId) const
{
	uint8_t payload[33];
	CBPTC19696 bptc;
	::memset(payload, 0, sizeof(payload));

	uint8_t lc[12];
	::memset(lc, 0, sizeof(lc));
	lc[3] = (uint8_t)LOBYTE(HIWORD(dstId));
	lc[4] = (uint8_t)HIBYTE(LOWORD(dstId));
	lc[5] = (uint8_t)LOBYTE(LOWORD(dstId));
	lc[6] = (uint8_t)LOBYTE(HIWORD(srcId));
	lc[7] = (uint8_t)HIBYTE(LOWORD(srcId));
	lc[8] = (uint8_t)LOBYTE(LOWORD(srcId));
	uint8_t parity[4];
	CRS129::encode(lc, 9, parity);
	lc[9]  = parity[2] ^ DMR_VOICE_LC_HEADER_CRC_MASK;
	lc[10] = parity[1] ^ DMR_VOICE_LC_HEADER_CRC_MASK;
	lc[11] = parity[0] ^ DMR_VOICE_LC_HEADER_CRC_MASK;

	::memcpy(payload + 13, g_DmrSyncBSData, 7);
	uint8_t slottype[3];
	::memset(slottype, 0, sizeof(slottype));
	slottype[0]  = (MMDVMCLI_COLOUR_CODE << 4) & 0xF0;
	slottype[0] |= DMR_DT_VOICE_LC_HEADER & 0x0F;
	CGolay2087::encode(slottype);
	payload[12] = (payload[12] & 0xC0) | ((slottype[0] >> 2) & 0x3F);
	payload[13] = (payload[13] & 0x0F) | ((slottype[0] << 6) & 0xC0) | ((slottype[1] >> 2) & 0x30);
	payload[19] = (payload[19] & 0xF0) | ((slottype[1] >> 2) & 0x0F);
	payload[20] = (payload[20] & 0x03) | ((slottype[1] << 6) & 0xC0) | ((slottype[2] >> 2) & 0x3C);

	bptc.encode(lc, payload);
	buffer->Append(payload, sizeof(payload));
}

void CMMDVMClientProtocol::AppendTerminatorLCToBuffer(CBuffer *buffer, uint32_t srcId, uint32_t dstId) const
{
	uint8_t payload[33];
	CBPTC19696 bptc;
	::memset(payload, 0, sizeof(payload));

	uint8_t lc[12];
	::memset(lc, 0, sizeof(lc));
	lc[3] = (uint8_t)LOBYTE(HIWORD(dstId));
	lc[4] = (uint8_t)HIBYTE(LOWORD(dstId));
	lc[5] = (uint8_t)LOBYTE(LOWORD(dstId));
	lc[6] = (uint8_t)LOBYTE(HIWORD(srcId));
	lc[7] = (uint8_t)HIBYTE(LOWORD(srcId));
	lc[8] = (uint8_t)LOBYTE(LOWORD(srcId));
	uint8_t parity[4];
	CRS129::encode(lc, 9, parity);
	lc[9]  = parity[2] ^ DMR_TERMINATOR_WITH_LC_CRC_MASK;
	lc[10] = parity[1] ^ DMR_TERMINATOR_WITH_LC_CRC_MASK;
	lc[11] = parity[0] ^ DMR_TERMINATOR_WITH_LC_CRC_MASK;

	::memcpy(payload + 13, g_DmrSyncBSData, 7);
	uint8_t slottype[3];
	::memset(slottype, 0, sizeof(slottype));
	slottype[0]  = (MMDVMCLI_COLOUR_CODE << 4) & 0xF0;
	slottype[0] |= DMR_DT_TERMINATOR_WITH_LC & 0x0F;
	CGolay2087::encode(slottype);
	payload[12] = (payload[12] & 0xC0) | ((slottype[0] >> 2) & 0x3F);
	payload[13] = (payload[13] & 0x0F) | ((slottype[0] << 6) & 0xC0) | ((slottype[1] >> 2) & 0x30);
	payload[19] = (payload[19] & 0xF0) | ((slottype[1] >> 2) & 0x0F);
	payload[20] = (payload[20] & 0x03) | ((slottype[1] << 6) & 0xC0) | ((slottype[2] >> 2) & 0x3C);

	bptc.encode(lc, payload);
	buffer->Append(payload, sizeof(payload));
}

void CMMDVMClientProtocol::ReplaceEMBInBuffer(CBuffer *buffer, uint8_t uiDmrPacketId) const
{
	if (uiDmrPacketId == 0)
	{
		buffer->ReplaceAt(33, (uint8_t)(buffer->at(33) | (g_DmrSyncBSVoice[0] & 0x0F)));
		buffer->ReplaceAt(34, g_DmrSyncBSVoice + 1, 5);
		buffer->ReplaceAt(39, (uint8_t)(buffer->at(39) | (g_DmrSyncBSVoice[6] & 0xF0)));
	}
	else
	{
		uint8_t emb[2];
		emb[0]  = (MMDVMCLI_COLOUR_CODE << 4) & 0xF0;
		emb[1]  = 0x00;
		CQR1676::encode(emb);
		buffer->ReplaceAt(33, (uint8_t)((buffer->at(33) & 0xF0) | ((emb[0] >> 4) & 0x0F)));
		buffer->ReplaceAt(34, (uint8_t)((emb[0] << 4) & 0xF0));
		buffer->ReplaceAt(35, (uint8_t)0);
		buffer->ReplaceAt(36, (uint8_t)0);
		buffer->ReplaceAt(37, (uint8_t)0);
		buffer->ReplaceAt(38, (uint8_t)((buffer->at(38) & 0xF0) | ((emb[1] >> 4) & 0x0F)));
		buffer->ReplaceAt(39, (uint8_t)((buffer->at(39) & 0x0F) | ((emb[1] << 4) & 0xF0)));
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// BrandMeister API — sync static talkgroups

bool CMMDVMClientProtocol::SyncBrandMeisterTGs()
{
	std::cout << m_Name << ": BrandMeister API: syncing static talkgroups..." << std::endl;

	// Get all active TGs (static + dynamic) — dynamic TGs added via admin
	// have already been registered on BM via API and must not be removed
	auto mappings = m_TGMap.GetAllMappings();
	std::set<std::pair<uint32_t, uint8_t>> desired;  // (tg, slot)
	for (const auto &m : mappings)
	{
		desired.insert({m.tg, m.timeslot});
	}

	// Get current BM static TGs
	std::vector<BMTalkgroup> bmTGs;
	if (!m_BmApi.GetStaticTGs(bmTGs))
	{
		std::cerr << m_Name << ": BrandMeister API: failed to get static TGs" << std::endl;
		return false;
	}
	bool ok = true;

	// Remove BM TGs that are not in our config
	for (const auto &bm : bmTGs)
	{
		if (desired.find({bm.talkgroup, bm.slot}) == desired.end() && !m_BmApi.RemoveStaticTG(bm.talkgroup, bm.slot))
			ok = false;
	}

	// Add our TGs that are missing on BM
	std::set<std::pair<uint32_t, uint8_t>> bmSet;
	for (const auto &bm : bmTGs)
		bmSet.insert({bm.talkgroup, bm.slot});

	for (const auto &d : desired)
	{
		if (bmSet.find(d) == bmSet.end() && !m_BmApi.AddStaticTG(d.first, d.second))
			ok = false;
	}

	if (ok)
		std::cout << m_Name << ": BrandMeister API: sync complete (" << desired.size() << " static TGs)" << std::endl;
	return ok;
}

////////////////////////////////////////////////////////////////////////////////////////
// DMR ID / Callsign helpers

uint32_t CMMDVMClientProtocol::CallsignToDmrId(const CCallsign &cs) const
{
	return g_LDid.FindDmrid(cs.GetKey());
}

CCallsign CMMDVMClientProtocol::DmrIdToCallsign(uint32_t id) const
{
	CCallsign cs;
	g_LDid.Lock();
	const UCallsign *ucs = g_LDid.FindCallsign(id);
	if (ucs)
		cs = CCallsign(*ucs);
	g_LDid.Unlock();
	return cs;
}

void CMMDVMClientProtocol::LearnTalkerAlias(uint32_t streamId, unsigned burst, const uint8_t *sync)
{
	auto &entry = m_UnknownTA[streamId];
	std::string text;
	if (!entry.second.Collect(burst, sync, text))
		return;
	const uint32_t srcId = entry.first;
	m_UnknownTA.erase(streamId);

	// "CALLSIGN Firstname", as BrandMeister sets it, or what the radio has
	std::istringstream words(text);
	std::string base, name;
	words >> base;
	std::getline(words >> std::ws, name);
	std::transform(base.begin(), base.end(), base.begin(), ::toupper);
	CCallsign cs;
	if (base.size() < CALLSIGN_LEN)
		cs.SetCallsign(base, false);
	if (!cs.IsValid() || cs.GetBase().find_first_of("0123456789") == std::string::npos)
	{
		std::cout << m_Name << ": talker alias of unknown ID " << srcId << " has no callsign: \"" << text << "\"" << std::endl;
		return;
	}
	g_LDid.Lock();
	g_LDid.Learn(srcId, cs.GetKey(), name);
	g_LDid.Unlock();
	std::cout << m_Name << ": ID " << srcId << " is " << cs << " by its talker alias, until the DMR ID database has it" << std::endl;

	// the dashboard shows it already for this call; the other modes got the
	// header with N0CALL and keep it until the call ends
	auto incoming = m_IncomingStreams.find(streamId);
	if (incoming == m_IncomingStreams.end() || incoming->second == 0)
		return;
	auto stream = GetStream(incoming->second, &m_MasterIp);
	if (!stream)
		return;
	const CCallsign was = stream->GetUserCallsign();
	if (was.GetBase() != CCallsign(UNKNOWN_CALLSIGN).GetBase())
		return;
	CCallsign now(cs);
	now.SetDmrid(srcId, false);
	stream->SetUserCallsign(now);
	g_Reflector.GetUsers()->StationRenamed(stream->GetModule(), was, now);
	g_Reflector.ReleaseUsers();
}

void CMMDVMClientProtocol::AppendDmrIdToBuffer(CBuffer *buffer, uint32_t id) const
{
	buffer->Append((uint8_t)(id >> 16));
	buffer->Append((uint8_t)(id >> 8));
	buffer->Append((uint8_t)(id));
}

void CMMDVMClientProtocol::AppendDmrRptrIdToBuffer(CBuffer *buffer, uint32_t id) const
{
	buffer->Append((uint8_t)(id >> 24));
	buffer->Append((uint8_t)(id >> 16));
	buffer->Append((uint8_t)(id >> 8));
	buffer->Append((uint8_t)(id));
}
