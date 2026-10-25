// SvxReflectorProtocol -- Connects to an SvxLink SvxReflector via TCP/UDP
// Implements SvxReflector protocol V2 with HMAC-SHA1 auth and OPUS audio
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <thread>

#include <openssl/hmac.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <map>

#include "SvxReflectorProtocol.h"
#include "SvxReflectorClient.h"
#include "Global.h"
#include "Utils.h"

////////////////////////////////////////////////////////////////////////////////////////
// constructor / destructor

CSvxReflectorProtocol::CSvxReflectorProtocol()
	: m_State(EState::disconnected)
	, m_TcpFd(-1)
	, m_ReconnectBackoff(SVX_RECONNECT_PERIOD)
	, m_ClientId(0)
	, m_UdpSeq(0)
	, m_OpusEncoder(nullptr)
	, m_RxGainNum(256)   // 0 dB default
	, m_TxGainNum(256)   // 0 dB default
	, m_SelectedTG(0)
	, m_StreamCounter(0)
{
}

CSvxReflectorProtocol::~CSvxReflectorProtocol()
{
	TcpDisconnect();
	if (m_OpusEncoder) opus_encoder_destroy(m_OpusEncoder);
}

////////////////////////////////////////////////////////////////////////////////////////
// TG mapping

char CSvxReflectorProtocol::TGToModule(uint32_t tg) const
{
	std::lock_guard<std::mutex> lock(m_TGMutex);
	auto it = m_TGToModule.find(tg);
	return (it != m_TGToModule.end()) ? it->second : ' ';
}

uint32_t CSvxReflectorProtocol::ModuleToTG(char module) const
{
	std::lock_guard<std::mutex> lock(m_TGMutex);
	auto it = m_ModuleToTG.find(module);
	return (it != m_ModuleToTG.end()) ? it->second : 0;
}

void CSvxReflectorProtocol::RefreshActivityByModule(char module)
{
	std::lock_guard<std::mutex> lock(m_TGMutex);
	auto modIt = m_ModuleToTG.find(module);
	if (modIt == m_ModuleToTG.end())
		return;
	uint32_t tg = modIt->second;
	auto dynIt = m_DynTGs.find(tg);
	if (dynIt != m_DynTGs.end())
	{
		auto newExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(900);
		if (newExpiry > dynIt->second.expires)
			dynIt->second.expires = newExpiry;
	}
}

void CSvxReflectorProtocol::LoadTGMap(void)
{
	m_TGToModule.clear();
	m_ModuleToTG.clear();
	const auto &jdata = g_Configure.GetData();
	for (auto it = jdata.begin(); it != jdata.end(); ++it)
	{
		const std::string &key = it.key();
		if (key.substr(0, 5) == "svxTG")
		{
			try {
				uint32_t tg = std::stoul(key.substr(5));
				char mod = it.value().get<std::string>()[0];
				m_TGToModule[tg] = mod;
				m_ModuleToTG[mod] = tg;
				std::cout << "SvxReflector TG mapping: TG" << tg << " <-> Module " << mod << std::endl;
			} catch (...) {}
		}
	}
	if (m_TGToModule.size() > 1)
		std::cout << "SvxReflector: one TG is selected at a time, TG" << HomeTG()
		          << " by default, the others are monitored and selected on activity" << std::endl;
}

void CSvxReflectorProtocol::ReloadStaticTGMap(void)
{
	{
		std::lock_guard<std::mutex> lock(m_TGMutex);

		// Collect dynamic entries to preserve
		std::unordered_map<uint32_t, SDynTG> savedDynTGs = m_DynTGs;
		std::unordered_map<uint32_t, char> savedDynMappings;
		for (auto &[tg, dynInfo] : m_DynTGs)
		{
			auto it = m_TGToModule.find(tg);
			if (it != m_TGToModule.end())
				savedDynMappings[tg] = it->second;
		}

		// Reload static from config
		m_TGToModule.clear();
		m_ModuleToTG.clear();
		const auto &jdata = g_Configure.GetData();
		for (auto it = jdata.begin(); it != jdata.end(); ++it)
		{
			const std::string &key = it.key();
			if (key.substr(0, 5) != "svxTG") continue;
			try {
				uint32_t tg = std::stoul(key.substr(5));
				char mod = it.value().get<std::string>()[0];
				m_TGToModule[tg] = mod;
				m_ModuleToTG[mod] = tg;
			} catch (...) {}
		}

		// Re-add dynamic entries (skip if TG now exists as static)
		m_DynTGs.clear();
		for (auto &[tg, dynInfo] : savedDynTGs)
		{
			if (m_TGToModule.find(tg) != m_TGToModule.end())
				continue;  // now static
			auto mapIt = savedDynMappings.find(tg);
			if (mapIt == savedDynMappings.end())
				continue;
			char mod = mapIt->second;
			m_TGToModule[tg] = mod;
			if (m_ModuleToTG.find(mod) == m_ModuleToTG.end())
				m_ModuleToTG[mod] = tg;
			m_DynTGs[tg] = dynInfo;
		}

		std::cout << "SvxReflector: TG mappings reloaded (" << m_TGToModule.size() << " total)" << std::endl;
	}
	m_TGsChanged = true;
}

// the TG selected by default: the lowest static one, else the lowest dynamic one
uint32_t CSvxReflectorProtocol::HomeTG(void) const
{
	// LoadTGMap calls this before the mutex matters, everyone else holds no lock
	uint32_t home = 0, homeDyn = 0;
	for (const auto &[tg, mod] : m_TGToModule)
	{
		if (m_DynTGs.count(tg))
		{
			if (homeDyn == 0 || tg < homeDyn) homeDyn = tg;
		}
		else if (home == 0 || tg < home)
			home = tg;
	}
	return home ? home : homeDyn;
}

// SVX clients of modules that lost their last TG; never called with m_TGMutex held,
// the client list is always locked first
void CSvxReflectorProtocol::RemoveOrphanClients(const std::set<char> &modules)
{
	if (modules.empty())
		return;
	CClients *clients = g_Reflector.GetClients();
	for (char mod : modules)
	{
		for (auto cit = clients->begin(); cit != clients->end(); ++cit)
		{
			if ((*cit)->GetProtocol() == EProtocol::svxreflector && (*cit)->GetReflectorModule() == mod)
			{
				clients->RemoveClient(*cit);
				break;
			}
		}
	}
	g_Reflector.ReleaseClients();
}

////////////////////////////////////////////////////////////////////////////////////////
// TCP connection

bool CSvxReflectorProtocol::TcpConnect(void)
{
	// DNS resolve
	struct addrinfo hints{}, *res;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	std::string portStr = std::to_string(g_Configure.GetUnsigned(g_Keys.svx.port));
	int err = getaddrinfo(m_Host.c_str(), portStr.c_str(), &hints, &res);
	if (err != 0)
	{
		std::cerr << "SvxReflector: DNS resolution failed for " << m_Host << ": " << gai_strerror(err) << std::endl;
		return false;
	}

	m_TcpFd = socket(res->ai_family, SOCK_STREAM, 0);
	if (m_TcpFd < 0)
	{
		freeaddrinfo(res);
		return false;
	}

	// Save server IP for UDP sends (resolve hostname to IP string)
	char ipstr[INET_ADDRSTRLEN];
	struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
	inet_ntop(AF_INET, &sa->sin_addr, ipstr, sizeof(ipstr));
	uint16_t udpPort = (uint16_t)g_Configure.GetUnsigned(g_Keys.svx.port);
	m_ServerIp = CIp(AF_INET, udpPort, ipstr);

	// non-blocking connect with timeout; the socket stays non-blocking, reads
	// collect whatever arrived and sends wait for room (SvxTcpSendFrame)
	fcntl(m_TcpFd, F_SETFL, O_NONBLOCK);
	int ret = connect(m_TcpFd, res->ai_addr, res->ai_addrlen);

	freeaddrinfo(res);

	if (ret < 0 && errno != EINPROGRESS)
	{
		close(m_TcpFd);
		m_TcpFd = -1;
		return false;
	}

	// wait for connect (5s timeout)
	struct pollfd pfd = { m_TcpFd, POLLOUT, 0 };
	ret = poll(&pfd, 1, 5000);
	if (ret <= 0 || !(pfd.revents & POLLOUT))
	{
		close(m_TcpFd);
		m_TcpFd = -1;
		return false;
	}

	// check for connect error
	int sockerr = 0;
	socklen_t slen = sizeof(sockerr);
	getsockopt(m_TcpFd, SOL_SOCKET, SO_ERROR, &sockerr, &slen);
	if (sockerr != 0)
	{
		close(m_TcpFd);
		m_TcpFd = -1;
		return false;
	}

	m_TcpRecvBuf.clear();
	std::cout << "SvxReflector: TCP connected to " << m_Host << ":" << portStr << std::endl;
	return true;
}

void CSvxReflectorProtocol::TcpDisconnect(void)
{
	if (m_TcpFd >= 0)
	{
		close(m_TcpFd);
		m_TcpFd = -1;
	}
	m_TcpRecvBuf.clear();
	m_State = EState::disconnected;
	m_ClientId = 0;
	m_SelectedTG = 0;
}

void CSvxReflectorProtocol::Disconnect(void)
{
	CloseInStream();
	m_OutStreamTG.clear();
	TcpDisconnect();
	m_ReconnectTimer.start();
}

bool CSvxReflectorProtocol::TcpSendFrame(const std::vector<uint8_t> &msg)
{
	if (m_TcpFd < 0)
		return false;
	if (!SvxTcpSendFrame(m_TcpFd, msg.data(), (uint32_t)msg.size()))
	{
		std::cerr << "SvxReflector: TCP send failed: " << strerror(errno) << std::endl;
		Disconnect();
		return false;
	}
	m_TcpHeartbeatTimer.start();
	return true;
}

bool CSvxReflectorProtocol::TcpReceive(void)
{
	// collect what has arrived, a message may come in several pieces
	while (m_TcpFd >= 0)
	{
		uint8_t tmp[4096];
		ssize_t n = recv(m_TcpFd, tmp, sizeof(tmp), 0);
		if (n > 0)
		{
			m_TcpRecvBuf.insert(m_TcpRecvBuf.end(), tmp, tmp + n);
			m_TcpLastReceiveTimer.start();
			continue;
		}
		if (n == 0)
		{
			std::cerr << "SvxReflector: TCP connection closed by server" << std::endl;
			Disconnect();
			return false;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			break;
		std::cerr << "SvxReflector: TCP recv error: " << strerror(errno) << std::endl;
		Disconnect();
		return false;
	}

	std::vector<uint8_t> frame;
	int r;
	while (m_TcpFd >= 0 && (r = SvxTcpTakeFrame(m_TcpRecvBuf, frame)) != 0)
	{
		if (r < 0)
		{
			std::cerr << "SvxReflector: oversized TCP frame from server" << std::endl;
			Disconnect();
			return false;
		}
		if (frame.size() >= 2)
			OnTcpMessage(((uint16_t)frame[0] << 8) | frame[1], frame);
	}
	return m_TcpFd >= 0;
}

////////////////////////////////////////////////////////////////////////////////////////
// V2 authentication

void CSvxReflectorProtocol::OnAuthChallenge(const std::vector<uint8_t> &payload)
{
	// payload: type(2) + vector_length(2) + nonce(20)
	if (payload.size() < 24) return;

	// Skip type(2) and vector length prefix(2), nonce starts at offset 4
	const uint8_t *nonce = &payload[4];

	// HMAC-SHA1(password, nonce)
	unsigned int digest_len = 20;
	uint8_t digest[20];
	HMAC(EVP_sha1(),
	     m_Password.c_str(), (int)m_Password.size(),
	     nonce, 20,
	     digest, &digest_len);

	// Build MsgAuthResponse: type(2) + callsign(string) + digest(vector<uint8_t>)
	// Field order matches ASYNC_MSG_MEMBERS(m_callsign, m_digest)
	std::vector<uint8_t> resp;
	SvxPack16(resp, SVX_TCP_MSG_AUTH_RESPONSE);
	SvxPackString(resp, m_Callsign);          // callsign FIRST (length-prefixed string)
	SvxPack16(resp, 20);                      // vector<uint8_t> length prefix
	resp.insert(resp.end(), digest, digest + 20); // 20 bytes HMAC digest

	TcpSendFrame(resp);
	std::cout << "SvxReflector: auth response sent for " << m_Callsign << std::endl;
}

void CSvxReflectorProtocol::OnAuthOk(void)
{
	std::cout << "SvxReflector: authentication successful" << std::endl;
	m_State = EState::connected;
}

void CSvxReflectorProtocol::OnServerInfo(const std::vector<uint8_t> &payload)
{
	// payload: type(2) + reserved(2) + client_id(2) + nodes + codecs
	if (payload.size() < 6) return;

	size_t pos = 4;
	m_ClientId = SvxUnpack16(payload, pos);
	std::cout << "SvxReflector: received ServerInfo, client_id=" << m_ClientId << std::endl;

	// Send MsgNodeInfo (JSON metadata), built by the library so any value is escaped
	nlohmann::json info = { {"sw", m_Software}, {"callsign", m_Callsign} };
	std::vector<uint8_t> msg;
	SvxPack16(msg, SVX_TCP_MSG_NODE_INFO);
	SvxPackString(msg, info.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
	if (!TcpSendFrame(msg))
		return;

	// one TG selected, all mapped ones monitored; without TGs stay connected and
	// wait for dynamic ones from the admin API
	m_SelectedTG = 0;
	SendTgMonitor();
	uint32_t home;
	{
		std::lock_guard<std::mutex> lock(m_TGMutex);
		home = HomeTG();
	}
	if (home)
		SelectTG(home);
	else
		std::cout << "SvxReflector: no TGs configured, waiting for dynamic TG assignment" << std::endl;

	// Send initial UDP heartbeat immediately so server learns our UDP address
	{
		m_UdpSeq = 0;
		m_Rx.ResetSeq();
		CBuffer hb;
		BuildUdpHeader(hb, SVX_UDP_MSG_HEARTBEAT);
		Send(hb, m_ServerIp);
		std::cout << "SvxReflector: initial UDP heartbeat sent, client_id=" << m_ClientId << std::endl;
	}
	m_UdpHeartbeatTimer.start();
	m_UdpLastReceiveTimer.start();

	// Reset reconnect backoff
	m_ReconnectBackoff = SVX_RECONNECT_PERIOD;
}

////////////////////////////////////////////////////////////////////////////////////////
// TG selection

void CSvxReflectorProtocol::SelectTG(uint32_t tg)
{
	if (tg == m_SelectedTG)
		return;
	// audio of the old TG ends here
	if (m_InStream.open)
		CloseInStream();
	std::vector<uint8_t> sel;
	SvxPack16(sel, SVX_TCP_MSG_SELECT_TG);
	SvxPack32(sel, tg);
	if (!TcpSendFrame(sel))
		return;
	m_SelectedTG = tg;
	m_TGActivityTimer.start();
	if (tg)
		std::cout << "SvxReflector: selected TG" << tg << " (Module " << TGToModule(tg) << ")" << std::endl;
	else
		std::cout << "SvxReflector: no TG selected" << std::endl;
}

void CSvxReflectorProtocol::SendTgMonitor(void)
{
	std::set<uint32_t> tgs;
	{
		std::lock_guard<std::mutex> lock(m_TGMutex);
		for (const auto &[tg, mod] : m_TGToModule)
			tgs.insert(tg);
	}
	// MsgTgMonitor: std::set<uint32_t>, packed as count + elements
	std::vector<uint8_t> msg;
	SvxPack16(msg, SVX_TCP_MSG_TG_MONITOR);
	SvxPack16(msg, (uint16_t)tgs.size());
	for (uint32_t tg : tgs)
		SvxPack32(msg, tg);
	if (TcpSendFrame(msg) && tgs.size() > 1)
		std::cout << "SvxReflector: monitoring " << tgs.size() << " TGs" << std::endl;
}

// after mapping changes and in idle times: keep the selection on a mapped TG,
// and go back to the home TG when the selected one has been quiet for a while
void CSvxReflectorProtocol::UpdateSelection(void)
{
	uint32_t home;
	bool selectedMapped;
	{
		std::lock_guard<std::mutex> lock(m_TGMutex);
		home = HomeTG();
		selectedMapped = m_TGToModule.count(m_SelectedTG) > 0;
	}
	if (m_TGsChanged.exchange(false))
	{
		SendTgMonitor();
		if (!selectedMapped || m_SelectedTG == 0)
		{
			SelectTG(home);
			return;
		}
	}
	if (m_SelectedTG != home && IsIdle() && m_TGActivityTimer.time() >= SVX_TG_RETURN)
		SelectTG(home);
}

////////////////////////////////////////////////////////////////////////////////////////
// TCP message dispatcher

void CSvxReflectorProtocol::OnTcpMessage(uint16_t type, const std::vector<uint8_t> &payload)
{
	switch (type)
	{
		case SVX_TCP_MSG_HEARTBEAT:
			break; // timer already reset by TcpReceive
		case SVX_TCP_MSG_AUTH_CHALLENGE:
			OnAuthChallenge(payload);
			break;
		case SVX_TCP_MSG_AUTH_OK:
			OnAuthOk();
			break;
		case SVX_TCP_MSG_SERVER_INFO:
			OnServerInfo(payload);
			break;
		case SVX_TCP_MSG_TALKER_START:
			OnTalkerStart(payload);
			break;
		case SVX_TCP_MSG_TALKER_STOP:
			OnTalkerStop(payload);
			break;
		case SVX_TCP_MSG_ERROR:
		{
			size_t pos = 2;
			std::string errmsg = (payload.size() > 2) ? SvxUnpackString(payload, pos) : "unknown";
			std::cerr << "SvxReflector: error from server: " << errmsg << std::endl;
			Disconnect();
			m_ReconnectBackoff = 300; // 5min on server error
			break;
		}
		case SVX_TCP_MSG_NODE_LIST:
		case SVX_TCP_MSG_NODE_JOINED:
		case SVX_TCP_MSG_NODE_LEFT:
			break; // informational, ignore
		default:
			break;
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// initialization

bool CSvxReflectorProtocol::Initialize(const char *type, const EProtocol ptype,
	const uint16_t port, const bool has_ipv4, const bool has_ipv6)
{
	m_Host = g_Configure.GetString(g_Keys.svx.host);
	m_Password = g_Configure.GetString(g_Keys.svx.password);
	m_Callsign = g_Configure.GetString(g_Keys.svx.callsign);
	m_Software = g_Configure.Contains(g_Keys.svx.software) ? g_Configure.GetString(g_Keys.svx.software) : "";
	if (m_Software.empty())
		m_Software = "urfd";
	else
		std::cout << "SvxReflector: reports its software as '" << m_Software << "'" << std::endl;

	// Parse BlockProtocols (comma-separated, e.g. "MMDVMClient,USRP")
	ParseBlockProtocols(g_Keys.svx.blockprotocols, "SvxReflector");

	// Save config defaults for runtime reset
	SaveBlockDefaults();

	// gain in dB, -40 to +40
	if (g_Configure.Contains(g_Keys.svx.rxgain))
	{
		int db = std::stoi(g_Configure.GetString(g_Keys.svx.rxgain));
		m_RxGainNum = CUtils::GainFromDb(db);
		std::cout << "SvxReflector: RxGain = " << db << " dB" << std::endl;
	}
	if (g_Configure.Contains(g_Keys.svx.txgain))
	{
		int db = std::stoi(g_Configure.GetString(g_Keys.svx.txgain));
		m_TxGainNum = CUtils::GainFromDb(db);
		std::cout << "SvxReflector: TxGain = " << db << " dB" << std::endl;
	}

	LoadTGMap();

	// OPUS: encoder at urfd's 8 kHz (the reflector side decodes it at 16 kHz),
	// decoder at 8 kHz (libopus resamples the 16 kHz stream, anti-aliased)
	m_OpusEncoder = SvxCreateEncoder();
	if (!m_OpusEncoder)
	{
		std::cerr << "SvxReflector: opus_encoder_create failed" << std::endl;
		return false;
	}
	if (!m_Rx.Init())
	{
		std::cerr << "SvxReflector: opus_decoder_create failed" << std::endl;
		return false;
	}

	// Initialize base protocol (opens UDP socket)
	if (!CProtocol::Initialize(type, ptype, port, has_ipv4, has_ipv6))
		return false;

	// Start timers
	m_ReconnectTimer.start();

	std::cout << "SvxReflector: initialized, connecting to " << m_Host << std::endl;
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////
// task (main loop)

void CSvxReflectorProtocol::Task(void)
{
	// Handle admin-requested reconnect
	if (m_ReconnectRequested.exchange(false))
	{
		std::cout << "SvxReflector: reconnect requested" << std::endl;
		Disconnect();
	}

	// Handle TCP connection state machine
	switch (m_State)
	{
		case EState::disconnected:
			if (m_ReconnectTimer.time() >= m_ReconnectBackoff)
			{
				std::cout << "SvxReflector: attempting TCP connect to " << m_Host << std::endl;
				m_ReconnectTimer.start();
				if (TcpConnect())
				{
					m_State = EState::authenticating;
					m_TcpHeartbeatTimer.start();
					m_TcpLastReceiveTimer.start();
					// Send ProtoVer(2,0)
					std::vector<uint8_t> ver;
					SvxPack16(ver, SVX_TCP_MSG_PROTO_VER);
					SvxPack16(ver, 2); // major
					SvxPack16(ver, 0); // minor
					TcpSendFrame(ver);
				}
				else
				{
					std::cerr << "SvxReflector: TCP connect failed, next attempt in " << std::min(m_ReconnectBackoff * 2, 60) << "s" << std::endl;
					// Increase backoff: 5, 10, 20, 40, 60, 60, ...
					if (m_ReconnectBackoff < 60)
						m_ReconnectBackoff = std::min(m_ReconnectBackoff * 2, 60);
				}
			}
			else
			{
				// nothing to do but wait, don't spin
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			break;

		case EState::authenticating:
		case EState::connected:
		{
			// Drain all pending UDP packets; before the login wait on nothing
			{
				CBuffer buffer;
				CIp ip;
				if (Receive4(buffer, ip, 20))
				{
					do {
						// only the reflector's own packets, from its address
						CIp from(ip);
						from.SetPort(m_ServerIp.GetPort());
						if (m_State == EState::connected && from == m_ServerIp)
						{
							m_UdpLastReceiveTimer.start();
							OnUdpPacket(buffer);
						}
					} while (Receive4(buffer, ip, 0));
				}
			}

			// Receive TCP frames
			if (!TcpReceive())
				break;

			// Check TCP timeout
			if (m_TcpLastReceiveTimer.time() > SVX_TCP_KEEPALIVE_TIMEOUT)
			{
				std::cerr << "SvxReflector: TCP heartbeat timeout" << std::endl;
				Disconnect();
				break;
			}

			// Send TCP heartbeat (TcpSendFrame disconnects on failure)
			if (m_TcpHeartbeatTimer.time() >= SVX_TCP_KEEPALIVE_PERIOD)
			{
				std::vector<uint8_t> hb;
				SvxPack16(hb, SVX_TCP_MSG_HEARTBEAT);
				if (!TcpSendFrame(hb))
					break;
			}

			if (m_State != EState::connected)
				break;

			// Send UDP heartbeat
			if (m_UdpHeartbeatTimer.time() >= SVX_UDP_KEEPALIVE_PERIOD)
			{
				CBuffer hb;
				BuildUdpHeader(hb, SVX_UDP_MSG_HEARTBEAT);
				Send(hb, m_ServerIp);
				m_UdpHeartbeatTimer.start();
			}

			// Check UDP timeout
			if (m_UdpLastReceiveTimer.time() > SVX_UDP_KEEPALIVE_TIMEOUT)
			{
				std::cerr << "SvxReflector: UDP heartbeat timeout" << std::endl;
				Disconnect();
				break;
			}

			// a talker whose flush got lost ends after a short silence
			if (m_InStream.open && m_InStream.audioTimer.time() >= SVX_TALKER_TIMEOUT)
				CloseInStream();
			// and urfd audio that stopped without its last packet
			for (auto it = m_OutStreamTG.begin(); it != m_OutStreamTG.end(); )
			{
				if (m_OutActivity[it->first].time() < SVX_TALKER_TIMEOUT)
				{
					++it;
					continue;
				}
				if (it->second == m_SelectedTG)
				{
					CBuffer flushBuf;
					BuildUdpHeader(flushBuf, SVX_UDP_MSG_FLUSH_SAMPLES);
					Send(flushBuf, m_ServerIp);
				}
				it = m_OutStreamTG.erase(it);
			}

			// Purge expired dynamic TGs
			{
				std::set<char> orphans;
				{
					std::lock_guard<std::mutex> lock(m_TGMutex);
					auto now = std::chrono::steady_clock::now();
					for (auto it = m_DynTGs.begin(); it != m_DynTGs.end(); )
					{
						if (now < it->second.expires)
						{
							++it;
							continue;
						}
						uint32_t tg = it->first;
						auto tgIt = m_TGToModule.find(tg);
						if (tgIt != m_TGToModule.end())
						{
							char mod = tgIt->second;
							// Only clear primary mapping if this TG was the primary
							auto modIt = m_ModuleToTG.find(mod);
							if (modIt != m_ModuleToTG.end() && modIt->second == tg)
								m_ModuleToTG.erase(modIt);
							m_TGToModule.erase(tgIt);
							std::cout << "SvxReflector: dynamic TG" << tg << " expired (Module " << mod << ")" << std::endl;
							bool hasTGsLeft = false;
							for (const auto &t : m_TGToModule)
								if (t.second == mod) { hasTGsLeft = true; break; }
							if (!hasTGsLeft)
								orphans.insert(mod);
						}
						it = m_DynTGs.erase(it);
						m_TGsChanged = true;
					}
				}
				RemoveOrphanClients(orphans);
			}

			// monitor list, selection and the way back to the home TG
			UpdateSelection();
			if (m_State != EState::connected)
				break;

			// Handle end of streaming timeout
			CheckStreamsTimeout();

			// Refresh SVX clients for active modules, remove orphaned ones
			{
				std::set<char> mapped;
				{
					std::lock_guard<std::mutex> lock(m_TGMutex);
					for (const auto &t : m_TGToModule)
						mapped.insert(t.second);
				}
				CClients *clients = g_Reflector.GetClients();
				for (auto cit = clients->begin(); cit != clients->end(); )
				{
					if ((*cit)->GetProtocol() == EProtocol::svxreflector)
					{
						if (mapped.count((*cit)->GetReflectorModule()))
						{
							(*cit)->Alive();
							++cit;
						}
						else if (!(*cit)->IsAlive())
						{
							clients->RemoveClient(*cit);
							break; // iterator invalidated
						}
						else
							++cit;
					}
					else
						++cit;
				}
				g_Reflector.ReleaseClients();
			}

			// Handle outbound queue
			HandleQueue();
			break;
		}

		default:
			break;
	}
}

void CSvxReflectorProtocol::HandleKeepalives(void)
{
	// handled in Task() directly
}

////////////////////////////////////////////////////////////////////////////////////////
// incoming audio path: SvxReflector -> urfd streams

void CSvxReflectorProtocol::CloseInStream(void)
{
	if (m_InStream.open)
	{
		// what the decoder still holds, then a silent last frame: the squelch
		// tail would give D-Star artifacts (crackle, tones) through AMBE
		SvxPcmFrame rest;
		if (m_Rx.Flush(rest))
			PushInFrame(rest.data(), false);
		int16_t silence[160] = {};
		PushInFrame(silence, true);

		auto it = m_Streams.find(m_InStream.streamId);
		if (it != m_Streams.end() && it->second)
		{
			g_Reflector.CloseStream(it->second);
			m_Streams.erase(it);
		}
	}
	else
	{
		SvxPcmFrame rest;
		m_Rx.Flush(rest);
	}
	m_InStream.open = false;
	m_InStream.rejected = false;
	m_InStream.streamId = 0;
	m_InStream.tg = 0;
	m_InStream.module = ' ';
	m_InStream.talkerCallsign.clear();
}

void CSvxReflectorProtocol::OnTalkerStart(const std::vector<uint8_t> &payload)
{
	// payload: type(2) + tg(4) + callsign(string)
	if (payload.size() < 6) return;
	size_t pos = 2;
	uint32_t tg = SvxUnpack32(payload, pos);
	char module = TGToModule(tg);
	if (module == ' ')
		return;
	std::string talkerCs = SvxExtractCallsign(SvxUnpackString(payload, pos));  // "DL4JC-APP" -> "DL4JC"

	// a talker on a monitored TG: select it if this side is idle, else it is busy
	if (tg != m_SelectedTG)
	{
		if (!IsIdle() || m_TGActivityTimer.time() < SVX_TG_HOLD)
		{
			std::cout << "SvxReflector: talker " << talkerCs << " on monitored TG" << tg << ", TG" << m_SelectedTG << " is busy" << std::endl;
			return;
		}
		SelectTG(tg);
		if (m_SelectedTG != tg)
			return;
	}

	// Refresh dynamic TG TTL on activity
	{
		std::lock_guard<std::mutex> lock(m_TGMutex);
		auto dynIt = m_DynTGs.find(tg);
		if (dynIt != m_DynTGs.end())
		{
			auto newExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(900);
			if (newExpiry > dynIt->second.expires)
				dynIt->second.expires = newExpiry;
		}
	}

	// the same talker again keeps the stream, its audio may still be in flight;
	// someone else gets a stream of their own, under their callsign
	if (m_InStream.open && (m_InStream.tg != tg || m_InStream.talkerCallsign != talkerCs))
		CloseInStream();
	m_InStream.tg = tg;
	m_InStream.module = module;
	m_InStream.talkerCallsign = talkerCs;

	std::cout << "SvxReflector: talker start on TG" << tg << " -> Module " << module << " by " << talkerCs << std::endl;
}

void CSvxReflectorProtocol::OnTalkerStop(const std::vector<uint8_t> &payload)
{
	// informational: the stream ends with the UDP flush, or a short silence when
	// the flush got lost
	size_t pos = 2;
	uint32_t tg = SvxUnpack32(payload, pos);
	std::cout << "SvxReflector: talker stop on TG" << tg << std::endl;
}

void CSvxReflectorProtocol::OnUdpPacket(const CBuffer &buffer)
{
	// V2 header: type(2) + client_id(2) + seq(2)
	if (buffer.size() < 6)
		return;
	uint16_t type = ((uint16_t)buffer.data()[0] << 8) | buffer.data()[1];
	uint16_t seq = ((uint16_t)buffer.data()[4] << 8) | buffer.data()[5];

	// every message of the connection counts, a late audio packet is dropped
	if (!m_Rx.CheckSeq(seq))
		return;

	switch (type)
	{
		case SVX_UDP_MSG_AUDIO:
			OnUdpAudio(buffer);
			break;
		case SVX_UDP_MSG_FLUSH_SAMPLES:
			CloseInStream();
			// tell the reflector all samples are out
			{
				CBuffer ack;
				BuildUdpHeader(ack, SVX_UDP_MSG_ALL_FLUSHED);
				Send(ack, m_ServerIp);
			}
			break;
		default:
			break;   // heartbeat, all flushed
	}
}

void CSvxReflectorProtocol::OnUdpAudio(const CBuffer &buffer)
{
	// audio belongs to the selected TG
	if (m_SelectedTG == 0)
		return;
	char module = TGToModule(m_SelectedTG);
	if (module == ' ')
		return;

	// V2 UDP audio: type(2) + client_id(2) + seq(2) + opus_len(2) + opus_data
	if (buffer.size() < 9)
		return;
	uint16_t opusLen = ((uint16_t)buffer.data()[6] << 8) | buffer.data()[7];
	if ((size_t)(8 + opusLen) > buffer.size())
	{
		std::cerr << "SvxReflector: audio packet too short: opusLen=" << opusLen << " bufSize=" << buffer.size() << std::endl;
		return;
	}

	// audio without a talker start (it raced the TCP message, or got lost)
	if (m_InStream.tg != m_SelectedTG)
	{
		if (m_InStream.open)
			CloseInStream();
		m_InStream.tg = m_SelectedTG;
		m_InStream.module = module;
	}

	std::vector<SvxPcmFrame> frames;
	if (!m_Rx.Decode(buffer.data() + 8, opusLen, frames))
	{
		std::cerr << "SvxReflector: opus_decode failed" << std::endl;
		return;
	}
	m_InStream.audioTimer.start();
	m_TGActivityTimer.start();

	for (auto &f : frames)
	{
		CUtils::ApplyGain(f.data(), 160, m_RxGainNum);
		PushInFrame(f.data(), false);
	}
}

// opens the stream on the first frame of a talker, and again when urfd closed it
// meanwhile (stream timeout); false if the module is busy with someone else
bool CSvxReflectorProtocol::EnsureInStream(void)
{
	if (m_InStream.open && (m_InStream.rejected || m_Streams.count(m_InStream.streamId)))
		return !m_InStream.rejected;

	m_InStream.streamId = 0x5C00 | m_StreamCounter++;

	// Use talker callsign if available, otherwise reflector callsign
	std::string userCs = m_InStream.talkerCallsign.empty() ? m_Callsign : m_InStream.talkerCallsign;
	CCallsign my;
	my.SetCallsign(userCs, true); // true = lookup DMR ID
	if (my.GetDmrid() == 0)
		std::cout << "SvxReflector: no DMR ID found for " << userCs << " - DMR output depends on MMDVMClient FallbackDmrId" << std::endl;
	CCallsign rpt1(g_Reflector.GetCallsign());
	rpt1.SetCSModule(m_InStream.module);
	CCallsign rpt2 = m_ReflectorCallsign;
	rpt2.SetCSModule(m_InStream.module);
	auto header = std::unique_ptr<CDvHeaderPacket>(
		new CDvHeaderPacket(my, CCallsign("CQCQCQ"), rpt1, rpt2, m_InStream.streamId, true));
	OnDvHeaderPacketIn(header, m_ServerIp);
	m_InStream.open = true;

	// module busy: the talker's audio is dropped until it ends, not retried per packet
	if (!m_Streams.count(m_InStream.streamId))
	{
		m_InStream.rejected = true;
		return false;
	}
	auto stream = g_Reflector.GetStream(m_InStream.module);
	if (stream)
		stream->SetSourceTG(m_InStream.tg);
	std::cout << "SvxReflector: stream opened for " << userCs << " (DMR ID " << my.GetDmrid() << ") on module " << m_InStream.module << std::endl;
	return true;
}

void CSvxReflectorProtocol::PushInFrame(const int16_t *pcm, bool last)
{
	if (!last && !EnsureInStream())
		return;
	auto it = m_Streams.find(m_InStream.streamId);
	if (it == m_Streams.end() || !it->second)
		return;
	auto frame = std::unique_ptr<CDvFramePacket>(
		new CDvFramePacket(pcm, m_InStream.streamId, last, ECodecType::svx));
	frame->SetPacketModule(m_InStream.module);
	it->second->Push(std::move(frame));
}

void CSvxReflectorProtocol::OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip)
{
	// Check if stream already open
	auto stream = GetStream(Header->GetStreamId(), &Ip);
	if (stream)
	{
		stream->Tickle();
		return;
	}

	CCallsign my(Header->GetMyCallsign());
	CCallsign rpt1(Header->GetRpt1Callsign());
	CCallsign rpt2(Header->GetRpt2Callsign());

	// Find or create client — use configured node callsign
	CClients *clients = g_Reflector.GetClients();
	char module = Header->GetRpt2Module();
	CCallsign cs;
	cs.SetCallsign(SvxExtractCallsign(m_Callsign), false); // "DL4JC-HS" -> "DL4JC"
	cs.SetCSModule(module);
	std::shared_ptr<CClient> client = clients->FindClient(cs, module, Ip, EProtocol::svxreflector);
	if (client == nullptr)
	{
		clients->AddClient(std::make_shared<CSvxReflectorClient>(cs, Ip, module));
		client = clients->FindClient(cs, module, Ip, EProtocol::svxreflector);
	}

	if (client)
	{
		// Force clear master status from previous stream
		if (client->IsAMaster())
		{
			client->NotAMaster();
		}
		client->Alive();
		client->SetReflectorModule(module);
		if ((stream = g_Reflector.OpenStream(Header, client)) != nullptr)
		{
			m_Streams[stream->GetStreamId()] = stream;
		}
		else
		{
			std::cerr << "SvxReflector: OpenStream failed for module " << module << " (busy)" << std::endl;
		}
	}
	else
	{
		std::cerr << "SvxReflector: client not found/created for module " << module << std::endl;
	}

	g_Reflector.ReleaseClients();

	g_Reflector.GetUsers()->Hearing(my, rpt1, rpt2, "SvxReflector");
	g_Reflector.ReleaseUsers();
}

////////////////////////////////////////////////////////////////////////////////////////
// outgoing audio path: urfd streams -> SvxReflector

void CSvxReflectorProtocol::HandleQueue(void)
{
	while (!m_Queue.IsEmpty())
	{
		auto packet = m_Queue.Pop();
		const char module = packet->GetPacketModule();

		if (packet->IsDvHeader())
		{
			// Check if this stream originated from SVX — don't echo back
			{
				auto stream = g_Reflector.GetStream(module);
				if (stream)
				{
					auto owner = stream->GetOwnerClient();
					if (owner && owner->GetProtocol() == EProtocol::svxreflector)
					{
						m_OutStreamTG.erase(module);
						continue;
					}
				}
			}

			// Look up TG for this module
			uint32_t tg = ModuleToTG(module);
			if (tg == 0)
				continue;

			// only one TG at a time: take it over unless it carries a talker
			if (tg != m_SelectedTG)
			{
				bool otherOut = false;
				for (const auto &[m, t] : m_OutStreamTG)
					if (m != module && t != tg) otherOut = true;
				if (m_InStream.open || otherOut)
				{
					std::cout << "SvxReflector: module " << module << " not sent to TG" << tg << ", TG" << m_SelectedTG << " is busy" << std::endl;
					m_OutStreamTG.erase(module);
					continue;
				}
				SelectTG(tg);
				if (m_SelectedTG != tg)
					continue;
			}
			m_OutStreamTG[module] = tg;
			m_OutActivity[module].start();
			// a fresh encoder per transmission: the state left from the last one
			// starts the next with a low bitrate and a level 6 to 9 dB too high
			opus_encoder_ctl(m_OpusEncoder, OPUS_RESET_STATE);
		}
		else if (packet->IsDvFrame())
		{
			auto it = m_OutStreamTG.find(module);
			if (it == m_OutStreamTG.end())
				continue;

			uint32_t tg = it->second;
			const CDvFramePacket &frame = (const CDvFramePacket &)*packet;

			if (packet->IsLastPacket())
			{
				// Send UDP flush to signal end of transmission
				if (tg == m_SelectedTG)
				{
					CBuffer flushBuf;
					BuildUdpHeader(flushBuf, SVX_UDP_MSG_FLUSH_SAMPLES);
					Send(flushBuf, m_ServerIp);
				}
				m_OutStreamTG.erase(it);
			}
			else if (tg == m_SelectedTG)
			{
				// Get PCM data and encode/send
				const int16_t *pcm = (const int16_t *)frame.GetCodecData(ECodecType::usrp);
				if (pcm)
				{
					int16_t out[160];
					memcpy(out, pcm, sizeof(out));
					CUtils::ApplyGain(out, 160, m_TxGainNum);
					EncodeAndSendAudio(out);
					m_TGActivityTimer.start();
					m_OutActivity[module].start();
				}
			}
		}
	}
}

void CSvxReflectorProtocol::BuildUdpHeader(CBuffer &buf, uint16_t type)
{
	// V2 UDP header: type(2) + client_id(2) + seq(2)
	uint8_t hdr[6];
	hdr[0] = (type >> 8) & 0xFF;
	hdr[1] = type & 0xFF;
	hdr[2] = (m_ClientId >> 8) & 0xFF;
	hdr[3] = m_ClientId & 0xFF;
	hdr[4] = (m_UdpSeq >> 8) & 0xFF;
	hdr[5] = m_UdpSeq & 0xFF;
	m_UdpSeq++;
	buf.Set(hdr, 6);
}

void CSvxReflectorProtocol::EncodeAndSendAudio(const int16_t *pcm)
{
	uint8_t opusBuf[512];
	int opusLen = opus_encode(m_OpusEncoder, pcm, 160, opusBuf, sizeof(opusBuf));
	if (opusLen <= 0)
	{
		std::cerr << "SvxReflector: opus_encode failed: " << opus_strerror(opusLen) << std::endl;
		return;
	}

	// V2 UDP audio: header(6) + opus_len(2) + opus_data
	CBuffer buf;
	BuildUdpHeader(buf, SVX_UDP_MSG_AUDIO);
	uint8_t lenHdr[2];
	lenHdr[0] = (opusLen >> 8) & 0xFF;
	lenHdr[1] = opusLen & 0xFF;
	buf.Append(lenHdr, 2);
	buf.Append(opusBuf, opusLen);

	Send(buf, m_ServerIp);
}

////////////////////////////////////////////////////////////////////////////////////////
// dynamic TG management (called from admin socket thread)

bool CSvxReflectorProtocol::AddDynamicTG(uint32_t tg, char module, int ttlSeconds)
{
	{
		std::lock_guard<std::mutex> lock(m_TGMutex);
		auto expiry = std::chrono::steady_clock::now() + std::chrono::seconds(ttlSeconds);

		// Check if TG already mapped
		auto tgIt = m_TGToModule.find(tg);
		if (tgIt != m_TGToModule.end())
		{
			// If static, reject
			if (m_DynTGs.find(tg) == m_DynTGs.end())
			{
				std::cerr << "SvxReflector: TG" << tg << " is statically mapped, cannot override" << std::endl;
				return false;
			}
			// Update existing dynamic TTL
			m_DynTGs[tg].expires = expiry;
			std::cout << "SvxReflector: refreshed dynamic TG" << tg << " TTL=" << ttlSeconds << "s" << std::endl;
			return true;
		}

		// Check if module has a primary TG
		auto modIt = m_ModuleToTG.find(module);
		if (modIt != m_ModuleToTG.end())
		{
			// Module has primary — add as secondary (inbound only)
			m_TGToModule[tg] = module;
			m_DynTGs[tg] = { expiry };
			std::cout << "SvxReflector: added secondary TG" << tg << " -> Module " << module
			          << " TTL=" << ttlSeconds << "s (inbound only)" << std::endl;
		}
		else
		{
			// Module is free — add as primary
			m_TGToModule[tg] = module;
			m_ModuleToTG[module] = tg;
			m_DynTGs[tg] = { expiry };
			std::cout << "SvxReflector: added dynamic TG" << tg << " -> Module " << module
			          << " TTL=" << ttlSeconds << "s (primary)" << std::endl;
		}
	}

	// the Task thread monitors it from now on
	m_TGsChanged = true;
	return true;
}

bool CSvxReflectorProtocol::RemoveDynamicTG(uint32_t tg)
{
	std::set<char> orphans;
	{
		std::lock_guard<std::mutex> lock(m_TGMutex);

		auto dynIt = m_DynTGs.find(tg);
		if (dynIt == m_DynTGs.end())
		{
			std::cerr << "SvxReflector: TG" << tg << " is not a dynamic entry" << std::endl;
			return false;
		}

		auto tgIt = m_TGToModule.find(tg);
		if (tgIt != m_TGToModule.end())
		{
			char removedMod = tgIt->second;
			auto modIt = m_ModuleToTG.find(removedMod);
			if (modIt != m_ModuleToTG.end() && modIt->second == tg)
				m_ModuleToTG.erase(modIt);
			m_TGToModule.erase(tgIt);
			std::cout << "SvxReflector: removed dynamic TG" << tg << " from Module " << removedMod << std::endl;

			bool hasTGsLeft = false;
			for (const auto &t : m_TGToModule)
				if (t.second == removedMod) { hasTGsLeft = true; break; }
			if (!hasTGsLeft)
				orphans.insert(removedMod);
		}
		m_DynTGs.erase(dynIt);
	}

	// client list after the TG lock, the Task thread takes them in this order too
	RemoveOrphanClients(orphans);
	m_TGsChanged = true;
	return true;
}

std::vector<CTGModuleMap::STGInfo> CSvxReflectorProtocol::GetTGMappings(void) const
{
	std::lock_guard<std::mutex> lock(m_TGMutex);
	std::vector<CTGModuleMap::STGInfo> result;
	auto now = std::chrono::steady_clock::now();

	for (const auto &pair : m_TGToModule)
	{
		CTGModuleMap::STGInfo info;
		info.tg = pair.first;
		info.module = pair.second;
		info.timeslot = 0;

		auto dynIt = m_DynTGs.find(pair.first);
		if (dynIt != m_DynTGs.end())
		{
			info.is_static = false;
			info.remainingSeconds = (int)std::chrono::duration_cast<std::chrono::seconds>(dynIt->second.expires - now).count();
		}
		else
		{
			info.is_static = true;
			info.remainingSeconds = -1;
		}

		// Determine if primary: check if m_ModuleToTG[module] == this TG
		auto modIt = m_ModuleToTG.find(pair.second);
		info.is_primary = (modIt != m_ModuleToTG.end() && modIt->second == pair.first);

		result.push_back(info);
	}

	return result;
}
