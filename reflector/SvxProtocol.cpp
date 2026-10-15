// SvxProtocol -- SvxReflector server accepting incoming SvxLink node connections
// Implements SvxReflector protocol V2 with HMAC-SHA1 auth and OPUS audio
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>

#include <cmath>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <nlohmann/json.hpp>

#include "SvxProtocol.h"
#include "SvxClient.h"
#include "Global.h"
#include "Utils.h"

////////////////////////////////////////////////////////////////////////////////////////
// constructor / destructor

CSvxProtocol::CSvxProtocol()
	: m_TcpListenFd(-1)
	, m_NextClientId(1)
	, m_StreamCounter(0)
	, m_RxGainNum(256)   // 0 dB default
	, m_TxGainNum(256)   // 0 dB default
{
}

CSvxProtocol::~CSvxProtocol()
{
	Close();
	for (auto &[mod, enc] : m_OutEncoders)
		opus_encoder_destroy(enc);
}

////////////////////////////////////////////////////////////////////////////////////////
// TG mapping

char CSvxProtocol::TGToModule(uint32_t tg) const
{
	std::lock_guard<std::mutex> lock(m_TGMutex);
	auto it = m_TGToModule.find(tg);
	return (it != m_TGToModule.end()) ? it->second : ' ';
}

uint32_t CSvxProtocol::ModuleToTG(char module) const
{
	std::lock_guard<std::mutex> lock(m_TGMutex);
	auto it = m_ModuleToTG.find(module);
	return (it != m_ModuleToTG.end()) ? it->second : 0;
}

void CSvxProtocol::RefreshActivityByModule(char module)
{
	std::lock_guard<std::mutex> lock(m_TGMutex);
	auto modIt = m_ModuleToTG.find(module);
	if (modIt == m_ModuleToTG.end()) return;
	auto dynIt = m_DynTGs.find(modIt->second);
	if (dynIt != m_DynTGs.end())
	{
		auto newExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(900);
		if (newExpiry > dynIt->second.expires)
			dynIt->second.expires = newExpiry;
	}
}

void CSvxProtocol::LoadTGMap(void)
{
	m_TGToModule.clear();
	m_ModuleToTG.clear();
	const auto &jdata = g_Configure.GetData();
	for (auto it = jdata.begin(); it != jdata.end(); ++it)
	{
		const std::string &key = it.key();
		if (key.substr(0, 6) == "svxsTG")
		{
			try {
				uint32_t tg = std::stoul(key.substr(6));
				auto modStr = it.value().get<std::string>();
				if (modStr.empty()) continue;
				char mod = modStr[0];
				m_TGToModule[tg] = mod;
				m_ModuleToTG[mod] = tg;
				std::cout << "SVXServer: TG mapping: TG" << tg << " <-> Module " << mod << std::endl;
			} catch (...) {}
		}
	}
}

void CSvxProtocol::ReloadStaticTGMap(void)
{
	std::lock_guard<std::mutex> lock(m_TGMutex);

	auto savedDynTGs = m_DynTGs;
	std::unordered_map<uint32_t, char> savedDynMappings;
	for (auto &[tg, dynInfo] : m_DynTGs)
	{
		auto it = m_TGToModule.find(tg);
		if (it != m_TGToModule.end())
			savedDynMappings[tg] = it->second;
	}

	m_TGToModule.clear();
	m_ModuleToTG.clear();
	const auto &jdata = g_Configure.GetData();
	for (auto it = jdata.begin(); it != jdata.end(); ++it)
	{
		const std::string &key = it.key();
		if (key.substr(0, 6) != "svxsTG") continue;
		try {
			uint32_t tg = std::stoul(key.substr(6));
			char mod = it.value().get<std::string>()[0];
			m_TGToModule[tg] = mod;
			m_ModuleToTG[mod] = tg;
		} catch (...) {}
	}

	m_DynTGs.clear();
	for (auto &[tg, dynInfo] : savedDynTGs)
	{
		if (m_TGToModule.find(tg) != m_TGToModule.end()) continue;
		auto mapIt = savedDynMappings.find(tg);
		if (mapIt == savedDynMappings.end()) continue;
		char mod = mapIt->second;
		m_TGToModule[tg] = mod;
		if (m_ModuleToTG.find(mod) == m_ModuleToTG.end())
			m_ModuleToTG[mod] = tg;
		m_DynTGs[tg] = dynInfo;
	}

	std::cout << "SVXServer: TG mappings reloaded (" << m_TGToModule.size() << " total)" << std::endl;
}

std::vector<std::string> CSvxProtocol::GetConnectedNodeList(void) const
{
	std::vector<std::string> nodes;
	for (const auto &[id, peer] : m_Peers)
	{
		if (peer.authState == SSvxPeer::EAuthState::connected)
			nodes.push_back(peer.callsign);
	}
	return nodes;
}

////////////////////////////////////////////////////////////////////////////////////////
// TCP server

bool CSvxProtocol::TcpListen(void)
{
	m_TcpListenFd = socket(AF_INET, SOCK_STREAM, 0);
	if (m_TcpListenFd < 0)
	{
		std::cerr << "SVXServer: socket() failed: " << strerror(errno) << std::endl;
		return false;
	}

	int optval = 1;
	setsockopt(m_TcpListenFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	struct sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(m_Port);

	if (bind(m_TcpListenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		std::cerr << "SVXServer: bind() port " << m_Port << " failed: " << strerror(errno) << std::endl;
		close(m_TcpListenFd);
		m_TcpListenFd = -1;
		return false;
	}

	if (listen(m_TcpListenFd, 5) < 0)
	{
		std::cerr << "SVXServer: listen() failed: " << strerror(errno) << std::endl;
		close(m_TcpListenFd);
		m_TcpListenFd = -1;
		return false;
	}

	// non-blocking for accept in Task() loop
	fcntl(m_TcpListenFd, F_SETFL, O_NONBLOCK);

	std::cout << "SVXServer: TCP listening on port " << m_Port << std::endl;
	return true;
}

void CSvxProtocol::TcpAccept(void)
{
	struct pollfd pfd = { m_TcpListenFd, POLLIN, 0 };
	if (poll(&pfd, 1, 0) <= 0) return;

	struct sockaddr_in clientAddr{};
	socklen_t addrLen = sizeof(clientAddr);
	int fd = accept(m_TcpListenFd, (struct sockaddr *)&clientAddr, &addrLen);
	if (fd < 0) return;

	char ipstr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &clientAddr.sin_addr, ipstr, sizeof(ipstr));

	// only a few connections may be waiting for their login at a time
	size_t pending = 0;
	for (const auto &[id, p] : m_Peers)
		if (p.authState != SSvxPeer::EAuthState::connected)
			pending++;
	if (pending >= SVXS_MAX_PENDING)
	{
		std::cerr << "SVXServer: too many connections without login, refusing " << ipstr << std::endl;
		close(fd);
		return;
	}

	// Set non-blocking
	fcntl(fd, F_SETFL, O_NONBLOCK);

	// Assign client ID; it goes into the 16 bit UDP header, 0 stands for urfd as talker
	uint32_t clientId = m_NextClientId++;
	if ((clientId & 0xFFFF) == 0)
		clientId = m_NextClientId++;

	SSvxPeer peer;
	peer.tcpFd = fd;
	peer.tcpAddr = CIp(AF_INET, 0, ipstr);
	peer.connectTimer.start();
	peer.clientId = clientId & 0xFFFF;
	peer.authState = SSvxPeer::EAuthState::expect_proto_ver;
	peer.tcpLastRecvTimer.start();
	peer.tcpHeartbeatTimer.start();

	m_FdToClientId[fd] = peer.clientId;
	m_Peers[peer.clientId] = std::move(peer);

	std::cout << "SVXServer: new TCP connection from " << ipstr << " (clientId=" << (clientId & 0xFFFF) << ")" << std::endl;
}

bool CSvxProtocol::TcpSendFrame(SSvxPeer &peer, const std::vector<uint8_t> &msg)
{
	if (peer.sendFailed)
		return false;
	if (!SvxTcpSendFrame(peer.tcpFd, msg.data(), (uint32_t)msg.size()))
	{
		// dropped by the Task loop, not here: callers iterate over m_Peers
		peer.sendFailed = true;
		return false;
	}
	return true;
}

void CSvxProtocol::TcpBroadcast(const std::vector<uint8_t> &msg, uint32_t exceptClientId)
{
	for (auto &[id, peer] : m_Peers)
	{
		if (id == exceptClientId) continue;
		if (peer.authState != SSvxPeer::EAuthState::connected) continue;
		TcpSendFrame(peer, msg);
	}
}

void CSvxProtocol::NotifyTalker(uint16_t type, uint32_t tg, const std::string &callsign)
{
	std::vector<uint8_t> msg;
	SvxPack16(msg, type);
	SvxPack32(msg, tg);
	SvxPackString(msg, callsign);
	for (auto &[id, peer] : m_Peers)
	{
		if (peer.authState != SSvxPeer::EAuthState::connected) continue;
		if (peer.selectedTG != tg && peer.monitoredTGs.count(tg) == 0) continue;
		TcpSendFrame(peer, msg);
	}
}

void CSvxProtocol::TcpReceiveFromPeer(uint32_t clientId)
{
	auto it = m_Peers.find(clientId);
	if (it == m_Peers.end()) return;
	SSvxPeer &peer = it->second;

	// Read available data into buffer
	uint8_t tmp[4096];
	ssize_t n = recv(peer.tcpFd, tmp, sizeof(tmp), 0);
	if (n == 0)
	{
		// Graceful disconnect
		std::cout << "SVXServer: peer " << peer.callsign << " (clientId=" << clientId << ") disconnected" << std::endl;
		DisconnectPeer(clientId);
		return;
	}
	if (n < 0)
	{
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		std::cerr << "SVXServer: recv error from clientId=" << clientId << ": " << strerror(errno) << std::endl;
		DisconnectPeer(clientId);
		return;
	}

	peer.tcpRecvBuf.insert(peer.tcpRecvBuf.end(), tmp, tmp + n);
	peer.tcpLastRecvTimer.start();

	// Process complete frames
	std::vector<uint8_t> payload;
	int r;
	while ((r = SvxTcpTakeFrame(peer.tcpRecvBuf, payload)) != 0)
	{
		if (r < 0)
		{
			std::cerr << "SVXServer: oversized frame from clientId=" << clientId << std::endl;
			DisconnectPeer(clientId);
			return;
		}

		if (payload.size() < 2) continue;
		uint16_t type = ((uint16_t)payload[0] << 8) | payload[1];

		switch (peer.authState)
		{
		case SSvxPeer::EAuthState::expect_proto_ver:
			if (type == SVX_TCP_MSG_PROTO_VER)
				OnPeerProtoVer(clientId, payload);
			break;
		case SSvxPeer::EAuthState::expect_auth_response:
			if (type == SVX_TCP_MSG_AUTH_RESPONSE)
				OnPeerAuthResponse(clientId, payload);
			break;
		case SSvxPeer::EAuthState::connected:
			switch (type)
			{
			case SVX_TCP_MSG_HEARTBEAT:
				break; // timer already reset
			case SVX_TCP_MSG_NODE_INFO:
				OnPeerNodeInfo(clientId, payload);
				break;
			case SVX_TCP_MSG_SELECT_TG:
				OnPeerSelectTG(clientId, payload);
				break;
			case SVX_TCP_MSG_TG_MONITOR:
				OnPeerTgMonitor(clientId, payload);
				break;
			default:
				break;
			}
			break;
		}

		// Re-check if peer still exists (may have been disconnected in handler)
		if (m_Peers.find(clientId) == m_Peers.end())
			return;
	}
}

void CSvxProtocol::DisconnectPeer(uint32_t clientId)
{
	auto it = m_Peers.find(clientId);
	if (it == m_Peers.end()) return;

	// its talk ends first: urfd stream, relay flush, talker stop
	ClosePeerInStream(clientId);
	SSvxPeer &peer = it->second;

	// Broadcast NODE_LEFT if was connected
	if (peer.authState == SSvxPeer::EAuthState::connected && !peer.callsign.empty())
	{
		std::vector<uint8_t> msg;
		SvxPack16(msg, SVX_TCP_MSG_NODE_LEFT);
		SvxPackString(msg, peer.callsign);
		TcpBroadcast(msg, clientId);

		// drop its CClient entry
		uint32_t oldTG = peer.selectedTG;
		peer.selectedTG = 0;
		UpdatePeerClient(peer, oldTG);
	}

	// Cleanup TCP
	if (peer.tcpFd >= 0)
	{
		m_FdToClientId.erase(peer.tcpFd);
		close(peer.tcpFd);
	}

	std::cout << "SVXServer: peer " << peer.callsign << " (clientId=" << clientId << ") removed" << std::endl;
	m_Peers.erase(it);
}

////////////////////////////////////////////////////////////////////////////////////////
// TCP message handlers

void CSvxProtocol::OnPeerProtoVer(uint32_t clientId, const std::vector<uint8_t> &payload)
{
	auto it = m_Peers.find(clientId);
	if (it == m_Peers.end()) return;
	SSvxPeer &peer = it->second;

	// payload: type(2) + major(2) + minor(2)
	if (payload.size() < 6) { DisconnectPeer(clientId); return; }
	size_t pos = 2;
	uint16_t major = SvxUnpack16(payload, pos);
	uint16_t minor = SvxUnpack16(payload, pos);

	if (major < 2)
	{
		std::cerr << "SVXServer: client proto version " << major << "." << minor << " too old, need >= 2.0" << std::endl;
		std::vector<uint8_t> err;
		SvxPack16(err, SVX_TCP_MSG_ERROR);
		SvxPackString(err, "unsupported protocol version");
		TcpSendFrame(peer, err);
		DisconnectPeer(clientId);
		return;
	}
	if (major > 2)
	{
		// protocol 3 needs TLS with certificates, which this server doesn't do;
		// like SvxReflector, ask for a downgrade (current SvxLink nodes refuse it and log why)
		std::cerr << "SVXServer: clientId=" << clientId << " uses protocol " << major << "." << minor
		          << ", only 2.0 is supported, asking for a downgrade" << std::endl;
		std::vector<uint8_t> msg;
		SvxPack16(msg, SVX_TCP_MSG_PROTO_VER_DOWNGRADE);
		SvxPack16(msg, 2);
		SvxPack16(msg, 0);
		TcpSendFrame(peer, msg);
		return;
	}

	std::cout << "SVXServer: clientId=" << clientId << " proto version " << major << "." << minor << std::endl;

	// random 20-byte challenge
	if (RAND_bytes(peer.challenge, sizeof(peer.challenge)) != 1)
	{
		std::cerr << "SVXServer: no random bytes for the challenge" << std::endl;
		DisconnectPeer(clientId);
		return;
	}

	// Send MsgAuthChallenge: type(2) + vector_length(2) + nonce(20)
	std::vector<uint8_t> msg;
	SvxPack16(msg, SVX_TCP_MSG_AUTH_CHALLENGE);
	SvxPack16(msg, 20); // vector<uint8_t> length prefix
	msg.insert(msg.end(), peer.challenge, peer.challenge + 20);
	TcpSendFrame(peer, msg);

	peer.authState = SSvxPeer::EAuthState::expect_auth_response;
}

void CSvxProtocol::OnPeerAuthResponse(uint32_t clientId, const std::vector<uint8_t> &payload)
{
	auto it = m_Peers.find(clientId);
	if (it == m_Peers.end()) return;
	SSvxPeer &peer = it->second;

	// payload: type(2) + callsign(string) + digest_vector_len(2) + digest(20)
	if (payload.size() < 6) { DisconnectPeer(clientId); return; }

	size_t pos = 2;
	std::string callsign = SvxUnpackString(payload, pos);
	if (pos + 22 > payload.size()) { DisconnectPeer(clientId); return; }
	uint16_t digestLen = SvxUnpack16(payload, pos);
	if (digestLen != 20 || pos + 20 > payload.size()) { DisconnectPeer(clientId); return; }
	const uint8_t *clientDigest = &payload[pos];

	// Look up password for this callsign (thread-safe)
	// Try exact match first, then extract base callsign
	std::string password;
	{
		std::lock_guard<std::mutex> lock(m_PasswordMutex);
		auto pwIt = m_Passwords.find(callsign);
		if (pwIt == m_Passwords.end())
		{
			std::string baseCs = SvxExtractCallsign(callsign);
			pwIt = m_Passwords.find(baseCs);
		}
		if (pwIt != m_Passwords.end())
			password = pwIt->second;
	}
	if (password.empty())
	{
		std::cerr << "SVXServer: unknown callsign '" << callsign << "'" << std::endl;
		std::vector<uint8_t> err;
		SvxPack16(err, SVX_TCP_MSG_ERROR);
		SvxPackString(err, "unknown callsign");
		TcpSendFrame(peer, err);
		DisconnectPeer(clientId);
		return;
	}

	// Verify HMAC-SHA1(password, challenge)
	unsigned int hmacLen = 20;
	uint8_t expectedDigest[20];
	HMAC(EVP_sha1(),
	     password.c_str(), (int)password.size(),
	     peer.challenge, 20,
	     expectedDigest, &hmacLen);

	if (CRYPTO_memcmp(clientDigest, expectedDigest, 20) != 0)
	{
		std::cerr << "SVXServer: auth failed for '" << callsign << "'" << std::endl;
		std::vector<uint8_t> err;
		SvxPack16(err, SVX_TCP_MSG_ERROR);
		SvxPackString(err, "authentication failed");
		TcpSendFrame(peer, err);
		DisconnectPeer(clientId);
		return;
	}

	// Check GateKeeper
	CCallsign cs;
	cs.SetCallsign(SvxExtractCallsign(callsign), false);
	if (!g_GateKeeper.MayLink(cs, peer.tcpAddr, EProtocol::svx))
	{
		std::cerr << "SVXServer: access denied for '" << callsign << "' by GateKeeper" << std::endl;
		std::vector<uint8_t> err;
		SvxPack16(err, SVX_TCP_MSG_ERROR);
		SvxPackString(err, "access denied");
		TcpSendFrame(peer, err);
		DisconnectPeer(clientId);
		return;
	}

	// OPUS decoder for this peer
	peer.rx = std::make_unique<CSvxOpusRx>();
	if (!peer.rx->Init())
	{
		std::cerr << "SVXServer: opus_decoder_create failed for " << callsign << std::endl;
		DisconnectPeer(clientId);
		return;
	}

	// Auth successful
	peer.callsign = callsign;
	peer.authState = SSvxPeer::EAuthState::connected;
	peer.connectedSince = time(nullptr);

	// Send MsgAuthOk
	{
		std::vector<uint8_t> msg;
		SvxPack16(msg, SVX_TCP_MSG_AUTH_OK);
		TcpSendFrame(peer, msg);
	}

	// MsgServerInfo: type(2) + reserved(2) + client_id(2) + nodes(vector<string>) + codecs(vector<string>)
	{
		std::vector<uint8_t> msg;
		SvxPack16(msg, SVX_TCP_MSG_SERVER_INFO);
		SvxPack16(msg, 0);
		SvxPack16(msg, (uint16_t)clientId);

		auto nodes = GetConnectedNodeList();
		SvxPack16(msg, (uint16_t)nodes.size());
		for (const auto &n : nodes) SvxPackString(msg, n);

		std::vector<std::string> codecs = {"OPUS"};
		SvxPack16(msg, (uint16_t)codecs.size());
		for (const auto &c : codecs) SvxPackString(msg, c);

		TcpSendFrame(peer, msg);
	}

	// Broadcast NODE_JOINED to all other peers
	{
		std::vector<uint8_t> msg;
		SvxPack16(msg, SVX_TCP_MSG_NODE_JOINED);
		SvxPackString(msg, callsign);
		TcpBroadcast(msg, clientId);
	}

	std::cout << "SVXServer: '" << callsign << "' authenticated (clientId=" << clientId << ")" << std::endl;
}

void CSvxProtocol::OnPeerNodeInfo(uint32_t clientId, const std::vector<uint8_t> &payload)
{
	auto it = m_Peers.find(clientId);
	if (it == m_Peers.end()) return;
	SSvxPeer &peer = it->second;

	// payload: type(2) + json(string)
	if (payload.size() < 4) return;
	size_t pos = 2;
	std::string json = SvxUnpackString(payload, pos);
	std::cout << "SVXServer: node info from " << peer.callsign << ": " << json << std::endl;

	// Parse JSON and store node info
	try
	{
		auto j = nlohmann::json::parse(json);
		// the first string among keys, "" if none is there
		auto str = [](const nlohmann::json &o, std::initializer_list<const char *> keys) -> std::string
		{
			for (const char *k : keys)
				if (o.contains(k) && o[k].is_string())
					return o[k].get<std::string>();
			return "";
		};

		// SvxLink adds "sw" and "swVer" to the contents of its NODE_INFO_FILE
		std::string sw = str(j, {"sw"});
		std::string swVer = str(j, {"swVer", "sw_ver"});
		peer.nodeInfo.software = (sw.empty() || swVer.empty()) ? sw + swVer : sw + " " + swVer;
		peer.nodeInfo.location = str(j, {"nodeLocation", "Location", "location"});
		peer.nodeInfo.qth = str(j, {"Locator", "locator"});
		// node_info.json of SvxLink: "qth" is an array of sites, each with "rx"
		// and "tx" objects keyed by receiver/transmitter id
		if (j.contains("qth") && j["qth"].is_array() && !j["qth"].empty() && j["qth"][0].is_object())
		{
			const auto &site = j["qth"][0];
			if (peer.nodeInfo.qth.empty())
				peer.nodeInfo.qth = str(site, {"name"});
			auto firstName = [&str](const nlohmann::json &o) -> std::string
			{
				if (o.is_object())
					for (const auto &el : o)
						if (el.is_object())
							return str(el, {"name"});
				return "";
			};
			if (site.contains("rx"))
				peer.nodeInfo.rxSiteName = firstName(site["rx"]);
			if (site.contains("tx"))
				peer.nodeInfo.txSiteName = firstName(site["tx"]);
		}
		else if (peer.nodeInfo.qth.empty())
			peer.nodeInfo.qth = str(j, {"qth"});
		if (peer.nodeInfo.rxSiteName.empty())
			peer.nodeInfo.rxSiteName = str(j, {"rx_name"});
		if (peer.nodeInfo.txSiteName.empty())
			peer.nodeInfo.txSiteName = str(j, {"tx_name"});
		if (j.contains("logics") && j["logics"].is_array())
		{
			peer.nodeInfo.logics.clear();
			for (const auto &l : j["logics"])
				if (l.is_string()) peer.nodeInfo.logics.push_back(l.get<std::string>());
		}
		peer.nodeInfo.populated = true;
	}
	catch (const std::exception &e)
	{
		std::cerr << "SVXServer: failed to parse node info JSON from " << peer.callsign << ": " << e.what() << std::endl;
	}
}

// keeps the CClient of a peer on the module of its selected TG; called without
// m_TGMutex held (the client list is locked first)
void CSvxProtocol::UpdatePeerClient(SSvxPeer &peer, uint32_t oldTG)
{
	char oldMod = oldTG ? TGToModule(oldTG) : ' ';
	char newMod = peer.selectedTG ? TGToModule(peer.selectedTG) : ' ';
	if (oldMod == newMod)
		return;

	CIp ip = peer.udpDiscovered ? peer.udpEndpoint : CIp();
	CCallsign cs;
	cs.SetCallsign(SvxExtractCallsign(peer.callsign), false);

	CClients *clients = g_Reflector.GetClients();
	if (oldMod != ' ')
	{
		cs.SetCSModule(oldMod);
		auto client = clients->FindClient(cs, oldMod, ip, EProtocol::svx);
		if (client)
			clients->RemoveClient(client);
	}
	if (newMod != ' ')
	{
		cs.SetCSModule(newMod);
		if (!clients->FindClient(cs, newMod, ip, EProtocol::svx))
			clients->AddClient(std::make_shared<CSvxClient>(cs, ip, newMod));
	}
	g_Reflector.ReleaseClients();
}

void CSvxProtocol::OnPeerSelectTG(uint32_t clientId, const std::vector<uint8_t> &payload)
{
	auto it = m_Peers.find(clientId);
	if (it == m_Peers.end()) return;
	SSvxPeer &peer = it->second;

	// payload: type(2) + tg(4)
	if (payload.size() < 6) return;
	size_t pos = 2;
	uint32_t tg = SvxUnpack32(payload, pos);
	if (tg == peer.selectedTG)
		return;

	// like SvxReflector: talking on the old TG ends there, one TG is selected at a time
	ClosePeerInStream(clientId);
	uint32_t oldTG = peer.selectedTG;
	peer.selectedTG = tg;
	UpdatePeerClient(peer, oldTG);

	if (tg == 0)
		std::cout << "SVXServer: " << peer.callsign << " selected no TG" << std::endl;
	else
	{
		char module = TGToModule(tg);
		if (module == ' ')
			std::cout << "SVXServer: " << peer.callsign << " selected TG" << tg << " (not mapped to a module, nodes only)" << std::endl;
		else
			std::cout << "SVXServer: " << peer.callsign << " selected TG" << tg << " -> Module " << module << std::endl;
	}
}

void CSvxProtocol::OnPeerTgMonitor(uint32_t clientId, const std::vector<uint8_t> &payload)
{
	auto it = m_Peers.find(clientId);
	if (it == m_Peers.end()) return;
	SSvxPeer &peer = it->second;

	// payload: type(2) + set<uint32_t> (count(2) + elements)
	size_t pos = 2;
	uint16_t count = SvxUnpack16(payload, pos);
	if (pos + 4 * (size_t)count > payload.size())
		return;
	peer.monitoredTGs.clear();
	for (uint16_t i = 0; i < count; i++)
		peer.monitoredTGs.insert(SvxUnpack32(payload, pos));
	std::cout << "SVXServer: " << peer.callsign << " monitors " << count << " TG(s)" << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////
// UDP handling

void CSvxProtocol::OnUdpPacket(const CBuffer &buffer, const CIp &ip)
{
	if (buffer.size() < 6) return;

	uint16_t type = ((uint16_t)buffer.data()[0] << 8) | buffer.data()[1];
	uint16_t peerClientId = ((uint16_t)buffer.data()[2] << 8) | buffer.data()[3];
	uint16_t seq = ((uint16_t)buffer.data()[4] << 8) | buffer.data()[5];

	// Find peer by clientId
	auto it = m_Peers.find((uint32_t)peerClientId);
	if (it == m_Peers.end()) return;
	SSvxPeer &peer = it->second;

	// Only accept UDP from authenticated peers, and only from where they are:
	// the client id is a small counter, anyone could guess it
	if (peer.authState != SSvxPeer::EAuthState::connected) return;
	if (peer.udpDiscovered)
	{
		if (ip != peer.udpEndpoint) return;
	}
	else
	{
		CIp from(ip);
		from.SetPort(peer.tcpAddr.GetPort());
		if (from != peer.tcpAddr) return;
	}

	// UDP endpoint discovery
	if (!peer.udpDiscovered)
	{
		// the CClient moves from the address-less entry to the real one
		uint32_t tg = peer.selectedTG;
		peer.selectedTG = 0;
		UpdatePeerClient(peer, tg);
		peer.udpEndpoint = ip;
		peer.udpDiscovered = true;
		peer.selectedTG = tg;
		UpdatePeerClient(peer, 0);
		std::cout << "SVXServer: discovered UDP endpoint for " << peer.callsign << std::endl;
	}

	peer.udpLastRecvTimer.start();

	// every message counts for the sequence, a late audio packet is dropped
	if (!peer.rx->CheckSeq(seq))
		return;

	switch (type)
	{
	case SVX_UDP_MSG_HEARTBEAT:
		// Send heartbeat reply with client's own ID so it recognizes the packet
		SendUdpToPeer(peer, SVX_UDP_MSG_HEARTBEAT);
		break;
	case SVX_UDP_MSG_AUDIO:
		OnPeerUdpAudio(peer.clientId, buffer);
		break;
	case SVX_UDP_MSG_FLUSH_SAMPLES:
		OnPeerUdpFlush(peer.clientId);
		break;
	case SVX_UDP_MSG_ALL_FLUSHED:
		break;
	default:
		break;
	}
}

void CSvxProtocol::BuildUdpHeader(CBuffer &buf, uint16_t type, uint16_t clientId, uint16_t &seq)
{
	uint8_t hdr[6];
	hdr[0] = (type >> 8) & 0xFF;
	hdr[1] = type & 0xFF;
	hdr[2] = (clientId >> 8) & 0xFF;
	hdr[3] = clientId & 0xFF;
	hdr[4] = (seq >> 8) & 0xFF;
	hdr[5] = seq & 0xFF;
	seq++;
	buf.Set(hdr, 6);
}

// a UDP message to a peer, with its own client id and sequence number
void CSvxProtocol::SendUdpToPeer(SSvxPeer &peer, uint16_t type, const uint8_t *payload, size_t len)
{
	if (!peer.udpDiscovered)
		return;
	CBuffer buf;
	BuildUdpHeader(buf, type, (uint16_t)peer.clientId, peer.udpSeqSend);
	if (payload && len)
		buf.Append(payload, (int)len);
	Send(buf, peer.udpEndpoint);
}

void CSvxProtocol::OnPeerUdpAudio(uint32_t clientId, const CBuffer &buffer)
{
	auto it = m_Peers.find(clientId);
	if (it == m_Peers.end()) return;
	SSvxPeer &peer = it->second;

	// audio goes to the selected TG
	uint32_t tg = peer.selectedTG;
	if (tg == 0) return;

	// V2 UDP audio: type(2) + client_id(2) + seq(2) + opus_len(2) + opus_data
	if (buffer.size() < 9) return;
	uint16_t opusLen = ((uint16_t)buffer.data()[6] << 8) | buffer.data()[7];
	if ((size_t)(8 + opusLen) > buffer.size()) return;

	// one talker per TG, as on SvxReflector; urfd audio to the TG counts as one
	auto talker = m_TgTalker.find(tg);
	if (talker != m_TgTalker.end() && talker->second != clientId)
		return;

	if (talker == m_TgTalker.end())
	{
		m_TgTalker[tg] = clientId;
		peer.inStream = SSvxPeer::SInStream();
		peer.inStream.tg = tg;
		peer.inStream.module = TGToModule(tg);
		NotifyTalker(SVX_TCP_MSG_TALKER_START, tg, peer.callsign);
		std::cout << "SVXServer: talker " << peer.callsign << " on TG" << tg;
		if (peer.inStream.module != ' ')
			std::cout << " Module " << peer.inStream.module;
		std::cout << std::endl;
	}
	peer.inStream.audioTimer.start();

	// the other nodes on the TG get the packet as it came
	for (auto &[id, other] : m_Peers)
	{
		if (id == clientId) continue;
		if (other.authState != SSvxPeer::EAuthState::connected) continue;
		if (other.selectedTG != tg) continue;
		SendUdpToPeer(other, SVX_UDP_MSG_AUDIO, buffer.data() + 6, 2 + opusLen);
	}

	// and its module gets it decoded
	if (peer.inStream.module == ' ')
		return;
	RefreshActivityByModule(peer.inStream.module);

	std::vector<SvxPcmFrame> frames;
	if (!peer.rx->Decode(buffer.data() + 8, opusLen, frames))
		return;
	for (auto &f : frames)
	{
		CUtils::ApplyGain(f.data(), 160, m_RxGainNum);
		PushPeerFrame(peer, f.data(), false);
	}
}

void CSvxProtocol::OnPeerUdpFlush(uint32_t clientId)
{
	ClosePeerInStream(clientId);

	// Send ALL_FLUSHED back to the peer
	auto it = m_Peers.find(clientId);
	if (it != m_Peers.end())
		SendUdpToPeer(it->second, SVX_UDP_MSG_ALL_FLUSHED);
}

// opens the urfd stream on the first frame of the talker, and again when urfd closed
// it meanwhile (stream timeout); false if the module is busy with someone else
bool CSvxProtocol::EnsurePeerInStream(SSvxPeer &peer)
{
	auto &in = peer.inStream;
	if (in.open && (in.rejected || m_Streams.count(in.streamId)))
		return !in.rejected;

	in.streamId = 0x5D00 | m_StreamCounter++;

	CCallsign my;
	my.SetCallsign(SvxExtractCallsign(peer.callsign), true);
	CCallsign rpt1(g_Reflector.GetCallsign());
	rpt1.SetCSModule(in.module);
	CCallsign rpt2 = m_ReflectorCallsign;
	rpt2.SetCSModule(in.module);
	auto header = std::unique_ptr<CDvHeaderPacket>(
		new CDvHeaderPacket(my, CCallsign("CQCQCQ"), rpt1, rpt2, in.streamId, true));
	OnDvHeaderPacketIn(header, peer.udpDiscovered ? peer.udpEndpoint : CIp());
	in.open = true;

	// module busy: no urfd stream for this talker, its audio still reaches the nodes
	if (!m_Streams.count(in.streamId))
	{
		in.rejected = true;
		std::cout << "SVXServer: module " << in.module << " busy, " << peer.callsign << " reaches only the nodes" << std::endl;
		return false;
	}
	auto stream = g_Reflector.GetStream(in.module);
	if (stream)
		stream->SetSourceTG(in.tg);
	std::cout << "SVXServer: stream opened for " << peer.callsign << " on TG" << in.tg << " Module " << in.module << std::endl;
	return true;
}

void CSvxProtocol::PushPeerFrame(SSvxPeer &peer, const int16_t *pcm, bool last)
{
	if (!last && !EnsurePeerInStream(peer))
		return;
	auto sit = m_Streams.find(peer.inStream.streamId);
	if (sit == m_Streams.end() || !sit->second)
		return;
	auto frame = std::unique_ptr<CDvFramePacket>(
		new CDvFramePacket(pcm, peer.inStream.streamId, last, ECodecType::svx));
	frame->SetPacketModule(peer.inStream.module);
	sit->second->Push(std::move(frame));
}

void CSvxProtocol::ClosePeerInStream(uint32_t clientId)
{
	auto it = m_Peers.find(clientId);
	if (it == m_Peers.end()) return;
	SSvxPeer &peer = it->second;
	auto &in = peer.inStream;
	if (in.tg == 0) return;

	// urfd: what the decoder still holds, then a silent last frame
	if (in.open)
	{
		SvxPcmFrame rest;
		if (peer.rx->Flush(rest))
			PushPeerFrame(peer, rest.data(), false);
		int16_t silence[160] = {};
		PushPeerFrame(peer, silence, true);
		auto sit = m_Streams.find(in.streamId);
		if (sit != m_Streams.end() && sit->second)
		{
			g_Reflector.CloseStream(sit->second);
			m_Streams.erase(sit);
		}
	}
	else if (peer.rx)
	{
		SvxPcmFrame rest;
		peer.rx->Flush(rest);
	}

	// the nodes on the TG: end of audio, talker stop
	for (auto &[id, other] : m_Peers)
	{
		if (id == clientId) continue;
		if (other.authState != SSvxPeer::EAuthState::connected) continue;
		if (other.selectedTG != in.tg) continue;
		SendUdpToPeer(other, SVX_UDP_MSG_FLUSH_SAMPLES);
	}
	NotifyTalker(SVX_TCP_MSG_TALKER_STOP, in.tg, peer.callsign);

	auto talker = m_TgTalker.find(in.tg);
	if (talker != m_TgTalker.end() && talker->second == clientId)
		m_TgTalker.erase(talker);
	in = SSvxPeer::SInStream();
}

////////////////////////////////////////////////////////////////////////////////////////
// stream helpers

void CSvxProtocol::OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip)
{
	auto stream = GetStream(Header->GetStreamId(), &Ip);
	if (stream)
	{
		stream->Tickle();
		return;
	}

	CCallsign my(Header->GetMyCallsign());
	CCallsign rpt1(Header->GetRpt1Callsign());
	CCallsign rpt2(Header->GetRpt2Callsign());
	char module = Header->GetRpt2Module();

	CClients *clients = g_Reflector.GetClients();
	CCallsign cs;
	cs.SetCallsign(SvxExtractCallsign(my.GetCS()), false);
	cs.SetCSModule(module);
	std::shared_ptr<CClient> client = clients->FindClient(cs, module, Ip, EProtocol::svx);
	if (!client)
	{
		clients->AddClient(std::make_shared<CSvxClient>(cs, Ip, module));
		client = clients->FindClient(cs, module, Ip, EProtocol::svx);
	}

	if (client)
	{
		if (client->IsAMaster())
			client->NotAMaster();
		client->Alive();
		client->SetReflectorModule(module);
		if ((stream = g_Reflector.OpenStream(Header, client)) != nullptr)
			m_Streams[stream->GetStreamId()] = stream;
	}

	g_Reflector.ReleaseClients();
	g_Reflector.GetUsers()->Hearing(my, rpt1, rpt2, "SVX");
	g_Reflector.ReleaseUsers();
}

////////////////////////////////////////////////////////////////////////////////////////
// outgoing audio path: urfd streams -> SVX peers

void CSvxProtocol::HandleQueue(void)
{
	while (!m_Queue.IsEmpty())
	{
		auto packet = m_Queue.Pop();
		const char module = packet->GetPacketModule();

		if (packet->IsDvHeader())
		{
			// a node's own talk reaches the other nodes directly (OnPeerUdpAudio)
			{
				auto stream = g_Reflector.GetStream(module);
				if (stream)
				{
					auto owner = stream->GetOwnerClient();
					if (owner && owner->GetProtocol() == EProtocol::svx)
					{
						EndOutStream(module);
						continue;
					}
				}
			}

			// a stream of this module that never got its last packet ends now
			EndOutStream(module);

			uint32_t tg = ModuleToTG(module);
			if (tg == 0)
				continue;

			// a node talking on the TG has it, as the first talker on SvxReflector
			auto talker = m_TgTalker.find(tg);
			if (talker != m_TgTalker.end() && talker->second != SVXS_URFD_TALKER)
			{
				std::cout << "SVXServer: module " << module << " not sent to TG" << tg << ", a node is talking" << std::endl;
				EndOutStream(module);
				continue;
			}
			m_TgTalker[tg] = SVXS_URFD_TALKER;

			// the talker is the caller, not the hotspot or node it came through
			const CDvHeaderPacket &header = (const CDvHeaderPacket &)*packet;
			SOutStream out;
			out.tg = tg;
			out.talker = header.GetMyCallsign().GetBase();
			m_OutStreams[module] = out;

			// a fresh encoder state per transmission
			auto &enc = m_OutEncoders[module];
			if (!enc)
				enc = SvxCreateEncoder();
			else
				opus_encoder_ctl(enc, OPUS_RESET_STATE);

			NotifyTalker(SVX_TCP_MSG_TALKER_START, tg, out.talker);
		}
		else if (packet->IsDvFrame())
		{
			auto outIt = m_OutStreams.find(module);
			if (outIt == m_OutStreams.end()) continue;
			const uint32_t tg = outIt->second.tg;

			if (packet->IsLastPacket())
			{
				EndOutStream(module);
				continue;
			}
			outIt->second.lastFrame.start();

			const CDvFramePacket &frame = (const CDvFramePacket &)*packet;
			const int16_t *pcm = (const int16_t *)frame.GetCodecData(ECodecType::usrp);
			auto enc = m_OutEncoders[module];
			if (!pcm || !enc) continue;

			int16_t out[160];
			memcpy(out, pcm, sizeof(out));
			CUtils::ApplyGain(out, 160, m_TxGainNum);

			// encoded once, the same packet for every node on the TG
			uint8_t payload[2 + 512];
			int opusLen = opus_encode(enc, out, 160, payload + 2, 512);
			if (opusLen <= 0) continue;
			payload[0] = (opusLen >> 8) & 0xFF;
			payload[1] = opusLen & 0xFF;

			for (auto &[id, peer] : m_Peers)
			{
				if (peer.authState != SSvxPeer::EAuthState::connected) continue;
				if (peer.selectedTG != tg) continue;
				SendUdpToPeer(peer, SVX_UDP_MSG_AUDIO, payload, 2 + opusLen);
			}
		}
	}
}

// end of urfd audio to a TG: flush + talker stop to the nodes, the TG is free again
void CSvxProtocol::EndOutStream(char module)
{
	auto outIt = m_OutStreams.find(module);
	if (outIt == m_OutStreams.end())
		return;
	const uint32_t tg = outIt->second.tg;
	for (auto &[id, peer] : m_Peers)
	{
		if (peer.authState != SSvxPeer::EAuthState::connected) continue;
		if (peer.selectedTG != tg) continue;
		SendUdpToPeer(peer, SVX_UDP_MSG_FLUSH_SAMPLES);
	}
	NotifyTalker(SVX_TCP_MSG_TALKER_STOP, tg, outIt->second.talker);

	auto talker = m_TgTalker.find(tg);
	if (talker != m_TgTalker.end() && talker->second == SVXS_URFD_TALKER)
		m_TgTalker.erase(talker);
	m_OutStreams.erase(outIt);
}

////////////////////////////////////////////////////////////////////////////////////////
// initialization

bool CSvxProtocol::Initialize(const char *type, const EProtocol ptype,
	const uint16_t port, const bool has_ipv4, const bool has_ipv6)
{
	// Load per-user passwords from svxsUser_<CALLSIGN> keys
	{
		const auto &jdata = g_Configure.GetData();
		for (auto it = jdata.begin(); it != jdata.end(); ++it)
		{
			const std::string &key = it.key();
			if (key.substr(0, 9) == "svxsUser_")
			{
				std::string callsign = key.substr(9);
				m_Passwords[callsign] = it.value().get<std::string>();
				std::cout << "SVXServer: password configured for " << callsign << std::endl;
			}
		}
	}
	if (m_Passwords.empty())
	{
		std::cerr << "SVXServer: no users configured in [SVXServer] section" << std::endl;
		return false;
	}

	// Parse BlockProtocols
	if (g_Configure.Contains(g_Keys.svxs.blockprotocols))
	{
		const std::map<std::string, EProtocol> protoMap = {
			{"MMDVMClient", EProtocol::mmdvmclient}, {"SvxReflector", EProtocol::svxreflector},
			{"DExtra", EProtocol::dextra}, {"DPlus", EProtocol::dplus},
			{"DCS", EProtocol::dcs}, {"DMRPlus", EProtocol::dmrplus},
			{"MMDVM", EProtocol::dmrmmdvm}, {"YSF", EProtocol::ysf},
			{"M17", EProtocol::m17}, {"NXDN", EProtocol::nxdn},
			{"P25", EProtocol::p25}, {"USRP", EProtocol::usrp},
			{"URF", EProtocol::urf}, {"XLXPeer", EProtocol::xlxpeer}, {"G3", EProtocol::g3},
		};
		std::istringstream ss(g_Configure.GetString(g_Keys.svxs.blockprotocols));
		std::string token;
		while (std::getline(ss, token, ','))
		{
			token.erase(0, token.find_first_not_of(" \t"));
			token.erase(token.find_last_not_of(" \t") + 1);
			auto it = protoMap.find(token);
			if (it != protoMap.end())
			{
				m_BlockedSources.insert(it->second);
				std::cout << "SVXServer: blocking protocol " << token << std::endl;
			}
		}
	}
	SaveBlockDefaults();

	// gain in dB, -40 to +40
	if (g_Configure.Contains(g_Keys.svxs.rxgain))
	{
		try {
			int db = std::stoi(g_Configure.GetString(g_Keys.svxs.rxgain));
			m_RxGainNum = CUtils::GainFromDb(db);
			std::cout << "SVXServer: RxGain = " << db << " dB" << std::endl;
		} catch (const std::exception &e) {
			std::cerr << "SVXServer: invalid RxGain value: " << e.what() << std::endl;
		}
	}
	if (g_Configure.Contains(g_Keys.svxs.txgain))
	{
		try {
			int db = std::stoi(g_Configure.GetString(g_Keys.svxs.txgain));
			m_TxGainNum = CUtils::GainFromDb(db);
			std::cout << "SVXServer: TxGain = " << db << " dB" << std::endl;
		} catch (const std::exception &e) {
			std::cerr << "SVXServer: invalid TxGain value: " << e.what() << std::endl;
		}
	}

	LoadTGMap();

	// Initialize base protocol (opens UDP socket on port)
	if (!CProtocol::Initialize(type, ptype, port, has_ipv4, has_ipv6))
		return false;

	// Open TCP listen socket on same port
	if (!TcpListen())
		return false;

	std::cout << "SVXServer: initialized on port " << m_Port << " with " << m_Passwords.size() << " user(s)" << std::endl;
	return true;
}

void CSvxProtocol::Close(void)
{
	// Disconnect all peers
	std::vector<uint32_t> peerIds;
	for (const auto &[id, peer] : m_Peers)
		peerIds.push_back(id);
	for (uint32_t id : peerIds)
		DisconnectPeer(id);

	if (m_TcpListenFd >= 0)
	{
		close(m_TcpListenFd);
		m_TcpListenFd = -1;
	}

	CProtocol::Close();
}

////////////////////////////////////////////////////////////////////////////////////////
// Task loop

void CSvxProtocol::Task(void)
{
	// 1. Accept new TCP connections
	TcpAccept();

	// 2. Receive TCP data from all peers
	{
		std::vector<struct pollfd> pfds;
		std::vector<uint32_t> clientIds;
		for (const auto &[id, peer] : m_Peers)
		{
			if (peer.tcpFd >= 0)
			{
				pfds.push_back({peer.tcpFd, POLLIN, 0});
				clientIds.push_back(id);
			}
		}
		if (!pfds.empty())
		{
			int ret = poll(pfds.data(), pfds.size(), 0);
			if (ret > 0)
			{
				for (size_t i = 0; i < pfds.size(); i++)
				{
					if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP))
						TcpReceiveFromPeer(clientIds[i]);
				}
			}
		}
	}

	// 3. Receive all waiting UDP packets (each node sends 50 a second while talking)
	{
		CBuffer buffer;
		CIp ip;
		if (Receive4(buffer, ip, 20))
		{
			do {
				OnUdpPacket(buffer, ip);
			} while (Receive4(buffer, ip, 0));
		}
	}

	// 4. Keepalives: send TCP heartbeats, check timeouts
	{
		std::vector<uint32_t> deadPeers;
		for (auto &[id, peer] : m_Peers)
		{
			if (peer.sendFailed)
			{
				std::cerr << "SVXServer: TCP send to " << peer.callsign << " failed" << std::endl;
				deadPeers.push_back(id);
				continue;
			}
			if (peer.authState == SSvxPeer::EAuthState::connected)
			{
				// TCP heartbeat send
				if (peer.tcpHeartbeatTimer.time() >= SVXS_TCP_KEEPALIVE_PERIOD)
				{
					std::vector<uint8_t> hb;
					SvxPack16(hb, SVX_TCP_MSG_HEARTBEAT);
					TcpSendFrame(peer, hb);
					peer.tcpHeartbeatTimer.start();
				}

				// TCP timeout
				if (peer.tcpLastRecvTimer.time() >= SVXS_TCP_KEEPALIVE_TIMEOUT)
				{
					std::cerr << "SVXServer: TCP timeout for " << peer.callsign << std::endl;
					deadPeers.push_back(id);
					continue;
				}

				// UDP timeout (only if endpoint was discovered)
				if (peer.udpDiscovered && peer.udpLastRecvTimer.time() >= SVXS_UDP_KEEPALIVE_TIMEOUT)
				{
					std::cerr << "SVXServer: UDP timeout for " << peer.callsign << std::endl;
					deadPeers.push_back(id);
					continue;
				}
			}
			else
			{
				// a fixed time from connecting to logging in, sending bytes doesn't extend it
				if (peer.connectTimer.time() >= SVXS_AUTH_TIMEOUT)
				{
					std::cerr << "SVXServer: auth timeout for clientId=" << id << std::endl;
					deadPeers.push_back(id);
				}
			}
		}
		for (uint32_t id : deadPeers)
			DisconnectPeer(id);
	}

	// 5. a talker whose flush got lost ends after a short silence
	{
		std::vector<uint32_t> silent;
		for (auto &[id, peer] : m_Peers)
			if (peer.inStream.tg && peer.inStream.audioTimer.time() >= SVX_TALKER_TIMEOUT)
				silent.push_back(id);
		for (uint32_t id : silent)
			ClosePeerInStream(id);

		// and urfd audio that stopped without its last packet
		std::vector<char> ended;
		for (auto &[mod, out] : m_OutStreams)
			if (out.lastFrame.time() >= SVX_TALKER_TIMEOUT)
				ended.push_back(mod);
		for (char mod : ended)
			EndOutStream(mod);
	}

	// 6. Handle outbound audio queue
	HandleQueue();

	// 7. Check stream timeouts
	CheckStreamsTimeout();

	// 8. Peer list for the admin socket
	if (m_SnapshotTimer.time() >= 1)
	{
		UpdatePeerSnapshot();
		m_SnapshotTimer.start();
	}

	// 9. the CClients of connected nodes stay alive
	if (m_AliveTimer.time() >= 5)
	{
		std::vector<std::pair<CCallsign, CIp>> alive;
		for (auto &[id, peer] : m_Peers)
		{
			if (peer.authState != SSvxPeer::EAuthState::connected || peer.selectedTG == 0)
				continue;
			char mod = TGToModule(peer.selectedTG);
			if (mod == ' ')
				continue;
			CCallsign cs;
			cs.SetCallsign(SvxExtractCallsign(peer.callsign), false);
			cs.SetCSModule(mod);
			alive.emplace_back(cs, peer.udpDiscovered ? peer.udpEndpoint : CIp());
		}
		CClients *clients = g_Reflector.GetClients();
		for (auto &[cs, ip] : alive)
		{
			auto client = clients->FindClient(cs, cs.GetCSModule(), ip, EProtocol::svx);
			if (client)
				client->Alive();
		}
		g_Reflector.ReleaseClients();
		m_AliveTimer.start();
	}

	// 10. Expire dynamic TGs
	std::set<char> orphans;
	{
		auto now = std::chrono::steady_clock::now();
		std::lock_guard<std::mutex> lock(m_TGMutex);
		for (auto dynIt = m_DynTGs.begin(); dynIt != m_DynTGs.end(); )
		{
			if (now >= dynIt->second.expires)
			{
				uint32_t tg = dynIt->first;
				char mod = ' ';
				auto tgIt = m_TGToModule.find(tg);
				if (tgIt != m_TGToModule.end())
				{
					mod = tgIt->second;
					auto modIt = m_ModuleToTG.find(mod);
					if (modIt != m_ModuleToTG.end() && modIt->second == tg)
						m_ModuleToTG.erase(modIt);
					m_TGToModule.erase(tgIt);
					bool hasTGsLeft = false;
					for (const auto &t : m_TGToModule)
						if (t.second == mod) { hasTGsLeft = true; break; }
					if (!hasTGsLeft)
						orphans.insert(mod);
				}
				std::cout << "SVXServer: dynamic TG" << tg << " expired (Module " << mod << ")" << std::endl;
				dynIt = m_DynTGs.erase(dynIt);
			}
			else
			{
				++dynIt;
			}
		}
	}
	if (!orphans.empty())
	{
		CClients *clients = g_Reflector.GetClients();
		for (char mod : orphans)
		{
			for (auto cit = clients->begin(); cit != clients->end(); )
			{
				if ((*cit)->GetProtocol() == EProtocol::svx && (*cit)->GetReflectorModule() == mod)
				{
					clients->RemoveClient(*cit);
					cit = clients->begin();
				}
				else
					++cit;
			}
		}
		g_Reflector.ReleaseClients();
	}
}

void CSvxProtocol::HandleKeepalives(void)
{
}

////////////////////////////////////////////////////////////////////////////////////////
// dynamic TG management (called from admin socket thread)

bool CSvxProtocol::AddDynamicTG(uint32_t tg, char module, int ttlSeconds)
{
	std::lock_guard<std::mutex> lock(m_TGMutex);
	auto expiry = std::chrono::steady_clock::now() + std::chrono::seconds(ttlSeconds);

	auto tgIt = m_TGToModule.find(tg);
	if (tgIt != m_TGToModule.end())
	{
		if (m_DynTGs.find(tg) == m_DynTGs.end())
		{
			std::cerr << "SVXServer: TG" << tg << " is statically mapped, cannot override" << std::endl;
			return false;
		}
		m_DynTGs[tg].expires = expiry;
		return true;
	}

	auto modIt = m_ModuleToTG.find(module);
	if (modIt != m_ModuleToTG.end())
	{
		// Module has primary — add as secondary (inbound only)
		m_TGToModule[tg] = module;
		m_DynTGs[tg] = { expiry };
		std::cout << "SVXServer: added secondary TG" << tg << " -> Module " << module << std::endl;
	}
	else
	{
		// Module is free — add as primary
		m_TGToModule[tg] = module;
		m_ModuleToTG[module] = tg;
		m_DynTGs[tg] = { expiry };
		std::cout << "SVXServer: added dynamic TG" << tg << " -> Module " << module << std::endl;
	}

	return true;
}

bool CSvxProtocol::RemoveDynamicTG(uint32_t tg)
{
	char orphan = ' ';
	{
		std::lock_guard<std::mutex> lock(m_TGMutex);

		auto dynIt = m_DynTGs.find(tg);
		if (dynIt == m_DynTGs.end())
			return false;

		auto tgIt = m_TGToModule.find(tg);
		if (tgIt != m_TGToModule.end())
		{
			char removedMod = tgIt->second;
			auto modIt = m_ModuleToTG.find(removedMod);
			if (modIt != m_ModuleToTG.end() && modIt->second == tg)
				m_ModuleToTG.erase(modIt);
			m_TGToModule.erase(tgIt);

			bool hasTGsLeft = false;
			for (const auto &t : m_TGToModule)
				if (t.second == removedMod) { hasTGsLeft = true; break; }
			if (!hasTGsLeft)
				orphan = removedMod;
		}
		m_DynTGs.erase(dynIt);
	}

	// Remove orphaned SVX clients, the client list after the TG lock
	if (orphan != ' ')
	{
		CClients *clients = g_Reflector.GetClients();
		for (auto cit = clients->begin(); cit != clients->end(); )
		{
			if ((*cit)->GetProtocol() == EProtocol::svx && (*cit)->GetReflectorModule() == orphan)
			{
				clients->RemoveClient(*cit);
				cit = clients->begin();
			}
			else
				++cit;
		}
		g_Reflector.ReleaseClients();
	}

	return true;
}

std::vector<CTGModuleMap::STGInfo> CSvxProtocol::GetTGMappings(void) const
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

		auto modIt = m_ModuleToTG.find(pair.second);
		info.is_primary = (modIt != m_ModuleToTG.end() && modIt->second == pair.first);

		result.push_back(info);
	}

	return result;
}

////////////////////////////////////////////////////////////////////////////////////////
// user management (called from admin socket thread)

bool CSvxProtocol::AddUser(const std::string &callsign, const std::string &password)
{
	{
		std::lock_guard<std::mutex> lock(m_PasswordMutex);
		m_Passwords[callsign] = password;
	}
	IniAddUser(callsign, password);
	std::cout << "SVXServer: user '" << callsign << "' added" << std::endl;
	return true;
}

bool CSvxProtocol::RemoveUser(const std::string &callsign)
{
	{
		std::lock_guard<std::mutex> lock(m_PasswordMutex);
		auto it = m_Passwords.find(callsign);
		if (it == m_Passwords.end())
			return false;
		m_Passwords.erase(it);
	}
	IniRemoveUser(callsign);
	std::cout << "SVXServer: user '" << callsign << "' removed" << std::endl;
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////
// INI persistence

bool CSvxProtocol::IniAddUser(const std::string &callsign, const std::string &password)
{
	const std::string &path = g_Reflector.GetConfigPath();
	if (path.empty()) return false;

	// Read INI file
	std::ifstream in(path);
	if (!in.is_open()) return false;
	std::vector<std::string> lines;
	std::string line;
	while (std::getline(in, line))
		lines.push_back(line);
	in.close();

	// Find [SVXServer] section and insert/update user line
	bool inSection = false;
	int insertPos = -1;
	for (size_t i = 0; i < lines.size(); i++)
	{
		std::string trimmed = lines[i];
		// trim leading whitespace
		size_t start = trimmed.find_first_not_of(" \t");
		if (start != std::string::npos) trimmed = trimmed.substr(start);

		if (!trimmed.empty() && trimmed[0] == '[')
		{
			if (inSection) { insertPos = (int)i; break; } // next section
			if (trimmed.find("[SVXServer]") == 0) inSection = true;
			continue;
		}
		if (!inSection) continue;

		// Check if this callsign already exists (key = value)
		auto eq = trimmed.find('=');
		if (eq != std::string::npos)
		{
			std::string key = trimmed.substr(0, eq);
			// trim trailing whitespace from key
			size_t end = key.find_last_not_of(" \t");
			if (end != std::string::npos) key = key.substr(0, end + 1);
			if (key == callsign)
			{
				// Update existing line
				lines[i] = callsign + " = " + password;
				// Write back
				std::ofstream out(path);
				if (!out.is_open()) return false;
				for (const auto &l : lines) out << l << "\n";
				return true;
			}
		}
		if (eq != std::string::npos)
			insertPos = (int)i + 1; // track last key=value line in section
	}

	// Not found — insert new line after last key=value in section
	if (insertPos < 0) insertPos = (int)lines.size();
	lines.insert(lines.begin() + insertPos, callsign + " = " + password);

	std::ofstream out(path);
	if (!out.is_open()) return false;
	for (const auto &l : lines) out << l << "\n";
	return true;
}

bool CSvxProtocol::IniRemoveUser(const std::string &callsign)
{
	const std::string &path = g_Reflector.GetConfigPath();
	if (path.empty()) return false;

	std::ifstream in(path);
	if (!in.is_open()) return false;
	std::vector<std::string> lines;
	std::string line;
	while (std::getline(in, line))
		lines.push_back(line);
	in.close();

	bool inSection = false;
	bool found = false;
	for (auto it = lines.begin(); it != lines.end(); )
	{
		std::string trimmed = *it;
		size_t start = trimmed.find_first_not_of(" \t");
		if (start != std::string::npos) trimmed = trimmed.substr(start);

		if (!trimmed.empty() && trimmed[0] == '[')
		{
			if (inSection) break; // left section
			if (trimmed.find("[SVXServer]") == 0) inSection = true;
			++it;
			continue;
		}
		if (inSection && !trimmed.empty() && trimmed[0] != '#')
		{
			auto eq = trimmed.find('=');
			if (eq != std::string::npos)
			{
				std::string key = trimmed.substr(0, eq);
				size_t end = key.find_last_not_of(" \t");
				if (end != std::string::npos) key = key.substr(0, end + 1);
				if (key == callsign)
				{
					it = lines.erase(it);
					found = true;
					continue;
				}
			}
		}
		++it;
	}

	if (!found) return false;

	std::ofstream out(path);
	if (!out.is_open()) return false;
	for (const auto &l : lines) out << l << "\n";
	return true;
}

std::vector<std::string> CSvxProtocol::GetUsers(void) const
{
	std::lock_guard<std::mutex> lock(m_PasswordMutex);
	std::vector<std::string> users;
	for (const auto &[cs, pw] : m_Passwords)
		users.push_back(cs);
	return users;
}

void CSvxProtocol::UpdatePeerSnapshot(void)
{
	std::vector<SConnectedPeer> result;
	for (const auto &[id, peer] : m_Peers)
	{
		if (peer.authState != SSvxPeer::EAuthState::connected) continue;
		SConnectedPeer cp;
		cp.callsign = peer.callsign;
		cp.ip = peer.udpDiscovered ? peer.udpEndpoint.GetAddress() : "";
		if (peer.selectedTG)
			cp.subscribedTGs.insert(peer.selectedTG);
		cp.monitoredTGs = peer.monitoredTGs;
		cp.udpDiscovered = peer.udpDiscovered;
		cp.connectedSince = peer.connectedSince;
		cp.nodeInfo = peer.nodeInfo;
		result.push_back(std::move(cp));
	}
	std::lock_guard<std::mutex> lock(m_SnapshotMutex);
	m_PeerSnapshot = std::move(result);
}

// called from the admin socket thread, at most a second old
std::vector<CSvxProtocol::SConnectedPeer> CSvxProtocol::GetConnectedPeers() const
{
	std::lock_guard<std::mutex> lock(m_SnapshotMutex);
	return m_PeerSnapshot;
}
