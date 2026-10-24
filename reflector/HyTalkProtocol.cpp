// HyTalkProtocol -- a HyTalk PoC server for Hytera radios
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "HyTalkProtocol.h"
#include "HyTalkClient.h"
#include "Global.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>

////////////////////////////////////////////////////////////////////////////////////////
// helpers

// KCP's clock, milliseconds
static uint32_t KcpNow(void)
{
	static const auto start = std::chrono::steady_clock::now();
	return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

static std::string RandomHex(size_t n)
{
	static std::mt19937 rng(std::random_device{}());
	static const char *hex = "0123456789abcdef";
	std::string s;
	for (size_t i = 0; i < n; i++)
		s += hex[rng() & 15];
	return s;
}

// the user part of an account, as the radio computes its digest with it:
// before the '@', the last 7 digits of a 12 digit number, else all of it
static std::string AccountUser(const std::string &number)
{
	auto at = number.find('@');
	if (at != std::string::npos)
		return number.substr(0, at);
	if (number.size() == 12 && number.find_first_not_of("0123456789") == std::string::npos)
		return number.substr(5);
	return number;
}

// a group number with or without "@realm"
static std::string GroupNumber(const std::string &gdn)
{
	auto at = gdn.find('@');
	return (at == std::string::npos) ? gdn : gdn.substr(0, at);
}

// a group number as a radio knows it: with the realm of the radio's own number
// (its enterprise id), "26363" -> "26363@26363"; a radio looks a group up in its
// list by that and rejects an unknown one ("group not exist")
static std::string GroupForRadio(const std::string &gdn, const std::string &radioNumber)
{
	const std::string g = GroupNumber(gdn);
	const auto at = radioNumber.find('@');
	return (at == std::string::npos) ? g : g + radioNumber.substr(at);
}

// where an urfd transmission comes from, as the D-Star slow data text has it:
// "via DMR TG91", "via M17", "via YSF DG16"; the TG only once the stream knows it
static std::string ViaText(CPacketStream &stream, char module, bool &hasTg)
{
	hasTg = false;
	auto owner = stream.GetOwnerClient();
	if (!owner)
		return std::string();
	std::string name;
	bool tgBased = false;
	switch (owner->GetProtocol())
	{
	case EProtocol::mmdvmclient: case EProtocol::dmrmmdvm: name = "DMR"; tgBased = true; break;
	case EProtocol::dmrplus:     name = "DMR+"; break;
	case EProtocol::svxreflector: case EProtocol::svx: name = "SVX"; tgBased = true; break;
	case EProtocol::ysf: case EProtocol::ysfclient: name = "YSF"; break;
	case EProtocol::m17:         name = "M17"; break;
	case EProtocol::p25:         name = "P25"; break;
	case EProtocol::nxdn:        name = "NXDN"; break;
	case EProtocol::usrp:        name = "AllStar"; break;
	case EProtocol::dcsclient:   name = "DCS"; break;
	case EProtocol::dextraclient: name = "DExtra"; break;
	case EProtocol::dplusclient: name = "DPlus"; break;
	default:                     name = owner->GetProtocolName(); break;
	}
	std::string via = "via " + name;
	if (owner->GetProtocol() == EProtocol::ysf)
		via += " DG" + std::to_string(10 + (module - 'A'));
	if (tgBased && stream.GetSourceTG())
	{
		via += " TG" + std::to_string(stream.GetSourceTG());
		hasTg = true;
	}
	return via;
}

static std::string Trim(const std::string &s)
{
	auto b = s.find_first_not_of(" \t");
	if (b == std::string::npos)
		return std::string();
	auto e = s.find_last_not_of(" \t");
	return s.substr(b, e - b + 1);
}

////////////////////////////////////////////////////////////////////////////////////////
// constructor

CHyTalkProtocol::CHyTalkProtocol()
{
	std::mt19937 rng(std::random_device{}());
	m_NextCallId = (rng() & 0x7FFFFFFF) | 1;
}

CHyTalkProtocol::~CHyTalkProtocol()
{
	for (auto &[id, peer] : m_Peers)
		if (peer->kcp)
			ikcp_release(peer->kcp);
}

////////////////////////////////////////////////////////////////////////////////////////
// initialization

bool CHyTalkProtocol::Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6)
{
	m_Realm = g_Configure.GetString(g_Keys.hytalk.realm);

	// the app encrypts its signalling with keys built into it: from the operator's file
	{
		std::string err;
		if (!PnasPackage::LoadKeys(g_Configure.GetString(g_Keys.hytalk.keyfile), err))
		{
			std::cerr << "HyTalk: " << err << ", see docs/HYTALK.md" << std::endl;
			return false;
		}
	}
	m_Address = g_Configure.GetString(g_Keys.hytalk.address);
	m_RtpPort = (uint16_t)g_Configure.GetUnsigned(g_Keys.hytalk.rtpport);
	m_HttpPort = (uint16_t)g_Configure.GetUnsigned(g_Keys.hytalk.httpport);
	m_GisPort = (uint16_t)g_Configure.GetUnsigned(g_Keys.hytalk.gisport);
	m_GisInterval = (uint16_t)g_Configure.GetUnsigned(g_Keys.hytalk.gisinterval);
	m_HangTime = (unsigned)g_Configure.GetUnsigned(g_Keys.hytalk.hangtime);
	m_Debug = g_Configure.Contains(g_Keys.hytalk.debug) && g_Configure.GetBoolean(g_Keys.hytalk.debug);
	struct in_addr a;
	if (inet_pton(AF_INET, m_Address.c_str(), &a) != 1)
	{
		std::cerr << "HyTalk: Address " << m_Address << " is not an IPv4 address" << std::endl;
		return false;
	}
	m_AddressBe = a.s_addr;

	// accounts and groups
	const auto &jdata = g_Configure.GetData();
	for (auto it = jdata.begin(); it != jdata.end(); ++it)
	{
		const std::string &key = it.key();
		if (key.substr(0, 11) == "hytalkUser_")
		{
			std::string val = it.value().get<std::string>();
			SAccount acc;
			auto comma = val.find(',');
			acc.password = Trim(val.substr(0, comma));
			const std::string user = AccountUser(key.substr(11));
			acc.callsign = (comma == std::string::npos) ? user : Trim(val.substr(comma + 1));
			m_Accounts[user] = acc;
		}
		else if (key.substr(0, 11) == "hytalkGroup")
		{
			std::string val = it.value().get<std::string>();
			SGroup g;
			g.gdn = key.substr(11);
			g.module = val[0];
			auto comma = val.find(',');
			g.name = (comma == std::string::npos) ? (std::string("Module ") + g.module) : Trim(val.substr(comma + 1));
			m_Groups.push_back(g);
			std::cout << "HyTalk: group " << g.gdn << " \"" << g.name << "\" on module " << g.module << std::endl;
		}
	}

	// The radio keeps its group list and fetches it again only when the version
	// changes, so the version follows the groups: number, name and module (FNV-1a)
	{
		uint32_t h = 2166136261u;
		for (const auto &g : m_Groups)
			for (char c : g.gdn + "," + g.name + "," + g.module + ";")
				h = (h ^ (uint8_t)c) * 16777619u;
		m_GroupVersion = 1 + h % 9999;
	}

	ParseBlockProtocols(g_Keys.hytalk.blockprotocols, "HyTalk");
	SaveBlockDefaults();

	// signalling: the base opens the UDP socket on port
	if (!CProtocol::Initialize(type, ptype, port, has_ipv4, has_ipv6))
		return false;

	// voice
	CIp rtpIp(AF_INET, m_RtpPort, g_Configure.GetString(g_Keys.ip.ipv4bind).c_str());
	if (!m_RtpSocket.Open(rtpIp))
	{
		std::cerr << "HyTalk: can't open the RTP port " << m_RtpPort << std::endl;
		return false;
	}
	CIp rtcpIp(AF_INET, uint16_t(m_RtpPort + 1), g_Configure.GetString(g_Keys.ip.ipv4bind).c_str());
	if (!m_RtcpSocket.Open(rtcpIp))
	{
		std::cerr << "HyTalk: can't open the RTCP port " << m_RtpPort + 1 << std::endl;
		return false;
	}

	// the radios ask for their user profile on TCP at the PNAS port before
	// they log in, and fetch their groups from the presence server, both HTTPS
	const std::string bind4 = g_Configure.GetString(g_Keys.ip.ipv4bind);
	if (!m_Profile.Open(CIp(AF_INET, m_Port, bind4.c_str()),
	        [this](const std::string &line, const std::string &body, const std::string &addr) { return ProfileAnswer(line, body, addr); }, "user profile")
	 || !m_Presence.Open(CIp(AF_INET, m_HttpPort, bind4.c_str()),
	        [this](const std::string &line, const std::string &, const std::string &) { return HttpAnswer(line); }, "group list"))
		return false;

	m_GisFd = TcpListen(m_GisPort, "GIS");
	if (m_GisFd < 0)
		return false;

	std::cout << "HyTalk: initialized, PNAS on UDP " << m_Port << " (user profile on TCP " << m_Port << "), RTP on UDP " << m_RtpPort << ", groups on HTTP(S) " << m_HttpPort << ", GPS on TCP " << m_GisPort
	          << ", " << m_Accounts.size() << " account(s), " << m_Groups.size() << " group(s), radios reach us at " << m_Address << std::endl;
	return true;
}

void CHyTalkProtocol::Close(void)
{
	for (auto &[id, peer] : m_Peers)
	{
		UpdatePeerClient(*peer, true);
		if (peer->kcp)
		{
			ikcp_release(peer->kcp);
			peer->kcp = nullptr;
		}
	}
	m_Peers.clear();
	m_Profile.Close();
	m_Presence.Close();
	for (auto &c : m_GisConns)
		close(c.fd);
	m_GisConns.clear();
	if (m_GisFd >= 0)
	{
		close(m_GisFd);
		m_GisFd = -1;
	}
	m_RtpSocket.Close();
	m_RtcpSocket.Close();
	CProtocol::Close();
}

////////////////////////////////////////////////////////////////////////////////////////
// transport: KCP over UDP

int CHyTalkProtocol::KcpOutput(const char *buf, int len, ikcpcb *, void *user)
{
	SPeer *peer = (SPeer *)user;
	peer->proto->m_Socket4.Send((const uint8_t *)buf, (size_t)len, peer->addr);
	return 0;
}

CHyTalkProtocol::SPeer *CHyTalkProtocol::NewPeer(const CIp &ip, uint32_t conv, const uint8_t *appid)
{
	auto peer = std::make_unique<SPeer>();
	peer->id = m_NextPeerId++;
	peer->proto = this;
	peer->addr = ip;
	peer->conv = conv;
	peer->kcp = ikcp_create(conv, peer.get());
	if (!peer->kcp)
		return nullptr;
	memcpy(peer->kcp->appid, appid, 8);
	ikcp_setoutput(peer->kcp, KcpOutput);
	ikcp_wndsize(peer->kcp, 32, 32);
	ikcp_nodelay(peer->kcp, 1, 10, 2, 1);
	ikcp_setmtu(peer->kcp, 1400);
	SPeer *p = peer.get();
	m_Peers[p->id] = std::move(peer);
	std::cout << "HyTalk: new radio session from " << ip << ", conv " << conv << std::endl;
	return p;
}

void CHyTalkProtocol::OnDatagram(const uint8_t *data, size_t len, const CIp &ip)
{
	if (len < 36)
		return;
	const uint32_t conv = (uint32_t)data[8] | ((uint32_t)data[9] << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
	const uint8_t cmd = data[12];

	SPeer *peer = nullptr;
	for (auto &[id, p] : m_Peers)
		if (p->conv == conv && p->addr == ip && !p->dead)
		{
			peer = p.get();
			break;
		}
	if (!peer)
	{
		// a session starts with its first data segment (sn 0); anything else
		// belongs to one we no longer know, e.g. from before an urfd restart:
		// the radio would go on in it and never log in again, so reset it
		const uint32_t sn = (uint32_t)data[20] | ((uint32_t)data[21] << 8) | ((uint32_t)data[22] << 16) | ((uint32_t)data[23] << 24);
		if (cmd != 0x51 || sn != 0)
		{
			const std::string key = std::string(ip.GetAddress()) + ":" + std::to_string(ip.GetPort()) + "/" + std::to_string(conv);
			auto it = m_ResetSent.find(key);
			if (it == m_ResetSent.end() || it->second.time() >= 1.0)
			{
				uint8_t rst[36];
				memcpy(rst, data, 36);
				rst[12] = 0x55;			// cmd: reset
				rst[13] = 0;			// frg
				memset(rst + 32, 0, 4);	// len
				m_Socket4.Send(rst, sizeof(rst), ip);
				m_ResetSent[key].start();
				if (m_ResetSent.size() > 256)
					m_ResetSent.clear();
				std::cout << "HyTalk: reset an unknown session from " << ip << ", conv " << conv << std::endl;
			}
			return;
		}
		peer = NewPeer(ip, conv, data);
		if (!peer)
			return;
	}

	int r = ikcp_input(peer->kcp, (const char *)data, (long)len);
	if (r == -4)
	{
		DropPeer(*peer, "reset by the radio");
		return;
	}
	if (r < 0)
	{
		if (m_Debug)
			std::cout << "HyTalk debug: ikcp_input " << r << " for conv " << conv << " from " << ip << std::endl;
		return;
	}
	peer->lastRx.start();

	// whole messages out of the stream
	char buf[0x10000];
	int n;
	while ((n = ikcp_recv(peer->kcp, buf, sizeof(buf))) > 0)
		peer->rx.insert(peer->rx.end(), buf, buf + n);
	while (!peer->dead)
	{
		std::vector<uint8_t> raw;
		uint8_t encType = 0, keyId = 0;
		int t = PnasPackage::Take(peer->rx, raw, encType, keyId);
		if (t == 0)
			break;
		if (t < 0)
		{
			DropPeer(*peer, "garbage in its stream");
			break;
		}
		peer->encType = encType;
		peer->keyId = keyId;
		CPnasMsg msg;
		if (msg.Parse(raw.data(), raw.size()))
		{
			if (m_Debug)
				std::cout << "HyTalk debug: " << (peer->callsign.empty() ? std::string(ip.GetAddress()) : peer->callsign) << " conv " << conv
				          << " <- 0x" << std::hex << int(msg.type) << std::dec << " tid " << msg.tid << std::endl;
			OnMessage(*peer, msg);
		}
		else
			std::cerr << "HyTalk: broken message from " << peer->addr << std::endl;
	}
}

void CHyTalkProtocol::Send(SPeer &peer, const CPnasWriter &msg)
{
	if (!peer.kcp || peer.dead)
		return;
	std::vector<uint8_t> unit;
	if (!PnasPackage::Wrap(msg.Finish(peer.txSeq++), peer.encType, peer.keyId, unit))
	{
		std::cerr << "HyTalk: can't encrypt for " << peer.addr << " (type " << int(peer.encType) << ")" << std::endl;
		return;
	}
	const int r = ikcp_send(peer.kcp, (const char *)unit.data(), (int)unit.size());
	ikcp_flush(peer.kcp);
	if (m_Debug)
		std::cout << "HyTalk debug: " << (peer.callsign.empty() ? std::string(peer.addr.GetAddress()) : peer.callsign) << " conv " << peer.conv
		          << " -> 0x" << std::hex << int(msg.Type()) << std::dec << " tid " << msg.Tid() << (r < 0 ? " (ikcp_send failed)" : "")
		          << ", queued " << peer.kcp->nsnd_que << "+" << peer.kcp->nsnd_buf << std::endl;
}

// every 10 s: per radio what KCP has not got rid of, and how its link looks
void CHyTalkProtocol::DebugKcpState(void)
{
	for (const auto &[id, p] : m_Peers)
	{
		if (!p->kcp || p->dead)
			continue;
		const ikcpcb *k = p->kcp;
		std::cout << "HyTalk debug: " << (p->callsign.empty() ? std::string(p->addr.GetAddress()) : p->callsign) << " conv " << p->conv
		          << " from " << p->addr << ": snd_que " << k->nsnd_que << " snd_buf " << k->nsnd_buf
		          << " una/nxt " << k->snd_una << "/" << k->snd_nxt << " rcv_nxt " << k->rcv_nxt
		          << " rcv_buf/que " << k->nrcv_buf << "/" << k->nrcv_que << " rmt_wnd " << k->rmt_wnd << " cwnd " << k->cwnd
		          << " rto " << k->rx_rto << " srtt " << k->rx_srtt << " xmit " << k->xmit << " state " << int(k->state)
		          << " last rx " << int(p->lastRx.time()) << " s ago" << std::endl;
	}
}

void CHyTalkProtocol::DropPeer(SPeer &peer, const char *why)
{
	if (peer.dead)
		return;
	std::cout << "HyTalk: " << (peer.callsign.empty() ? std::string(peer.addr.GetAddress()) : peer.callsign) << " dropped, " << why << std::endl;
	peer.dead = true;
	ClosePeerStream(peer);
	for (auto &g : m_Groups)
		if (g.talker == peer.id)
			TalkerEnd(g);
	UpdatePeerClient(peer, true);
}

////////////////////////////////////////////////////////////////////////////////////////
// signalling

void CHyTalkProtocol::OnMessage(SPeer &peer, const CPnasMsg &msg)
{
	// a session that did not log in yet may only ping, log in and out: no
	// group, call, floor or text before the password was right
	if (peer.callsign.empty() && msg.type != PNAS_HEARTBEAT_PING && msg.type != PNAS_REGISTER_REQUEST && msg.type != PNAS_DEREGISTER_REQUEST)
	{
		if (m_Debug)
			std::cout << "HyTalk debug: message 0x" << std::hex << int(msg.type) << std::dec << " from " << peer.addr << " before the login ignored" << std::endl;
		return;
	}
	switch (msg.type)
	{
	case PNAS_HEARTBEAT_PING:
	{
		// The radio pings again as soon as the pong to its last ping arrives, so
		// an immediate pong makes it ping every round trip (~80 ms). Hold the pong
		// a little under its interval instead; it drops the link after 3 periods
		// without one. Only the last ping's tid counts.
		const uint32_t interval = msg.GetUint(PNAS_IE_HEARTBEAT_INTERVAL, 0) >> 1;
		double delay = (interval > 5) ? double(interval - 5) : double(HYTALK_PONG_DELAY);
		peer.pongDelay = std::min(delay, double(HYTALK_PONG_DELAY));
		peer.pongTid = msg.tid;
		peer.pongPending = true;
		peer.pongTimer.start();
		break;
	}
	case PNAS_REGISTER_REQUEST:
		OnRegister(peer, msg);
		break;
	case PNAS_REGISTER_COMPLETE:
		if (!peer.online && !peer.callsign.empty())
		{
			peer.online = true;
			peer.onlineSince = time(nullptr);
			std::cout << "HyTalk: " << peer.callsign << " (" << peer.number << ") online from " << peer.addr << std::endl;
			// the radio fetches its groups from our HTTP server once told they changed
			CPnasWriter notify(PNAS_GROUP_INFO_NOTIFY_REQ, (uint32_t)(rand() & 0xFF) + 1);
			notify.AddUint32(PNAS_IE_GROUP_UPDATE_ID, m_GroupVersion);
			notify.AddUint8(PNAS_IE_GROUP_LIST_DISPLAY, 1);
			notify.AddString(PNAS_IE_SUBSCRIBER_NUMBER, peer.number);
			Send(peer, notify);
		}
		break;
	case PNAS_DEREGISTER_REQUEST:
	{
		CPnasWriter acc(PNAS_DEREGISTER_ACCEPT, msg.tid);
		acc.AddUint8(PNAS_IE_CAUSE, 0);
		Send(peer, acc);
		DropPeer(peer, "logged out");
		break;
	}
	case PNAS_GROUP_AFFI_NOTIFY_REQ:
		OnAffiliate(peer, msg);
		break;
	case PNAS_MESSAGE_REQUEST:
		OnMessageRequest(peer, msg);
		break;
	case PNAS_MESSAGE_RESPONSE:
		break;	// a radio took a message we forwarded
	case PNAS_GROUP_INFO_UPDATE_REQ:
		SendCommonResponse(peer, msg, PNAS_GROUP_INFO_UPDATE_RSP);
		break;
	case PNAS_GROUP_UPDATE_REPORT_REQ:
		SendCommonResponse(peer, msg, PNAS_GROUP_UPDATE_REPORT_RSP);
		break;
	case PNAS_UE_CAPABILITY_NOTIFY_REQ:
		SendCommonResponse(peer, msg, PNAS_UE_CAPABILITY_NOTIFY_RSP);
		break;
	case PNAS_CALL_REQUEST:
		OnCallRequest(peer, msg);
		break;
	case PNAS_CALL_ACCEPT:
		OnCallAccept(peer, msg);
		break;
	case PNAS_CALL_COMPLETE:
		if (msg.GetUint(PNAS_IE_CALL_ID) == peer.callId)
			peer.callUp = true;
		break;
	case PNAS_CALL_RELEASE_REQUEST:
	case PNAS_CALL_REJECT:
		OnCallRelease(peer, msg);
		break;
	case PNAS_FLOOR_REQUEST:
		OnFloorRequest(peer, msg);
		break;
	case PNAS_FLOOR_RELEASE:
		OnFloorRelease(peer, msg);
		break;
	case PNAS_HEARTBEAT_RESPONSE:
	case PNAS_GROUP_INFO_NOTIFY_RSP:
	case PNAS_USER_CONFIG_NOTIFY_RSP:
	case PNAS_CALL_RELEASE_RESPONSE:
	case PNAS_FLOOR_GRANT_ACK:
	case PNAS_FLOOR_RELEASE_ACK:
		break;
	default:
		std::cout << "HyTalk: message 0x" << std::hex << int(msg.type) << std::dec << " from " << peer.addr << " not handled" << std::endl;
		break;
	}
}

void CHyTalkProtocol::SendCommonResponse(SPeer &peer, const CPnasMsg &msg, uint8_t type)
{
	CPnasWriter rsp(type, msg.tid);
	rsp.AddUint8(PNAS_IE_CAUSE, 0);
	rsp.AddString(PNAS_IE_SUBSCRIBER_NUMBER, peer.number);
	Send(peer, rsp);
}

void CHyTalkProtocol::OnRegister(SPeer &peer, const CPnasMsg &msg)
{
	const std::string number = msg.GetString(PNAS_IE_SUBSCRIBER_NUMBER);
	const std::string user = AccountUser(number);
	SAccount account;
	bool known;
	{
		std::lock_guard<std::mutex> lock(m_AccountMutex);
		auto it = m_Accounts.find(user);
		known = (it != m_Accounts.end());
		if (known)
			account = it->second;
	}
	bool dbDown = false;
	if (!known)
	{
		// not in urfd.ini, maybe in the credential database
		SCredential cred;
		const auto r = g_Credentials.Lookup(ECredService::hytalk, user, cred);
		known = (CCredentialStore::EResult::found == r && !cred.password.empty());
		dbDown = (CCredentialStore::EResult::error == r);
		if (known)
			account = { cred.password, cred.callsign.empty() ? user : cred.callsign };
	}
	if (!known)
	{
		std::cout << "HyTalk: login of unknown account " << number << " from " << peer.addr << (dbDown ? " (credential database unavailable)" : "") << std::endl;
		CPnasWriter rej(PNAS_REGISTER_REJECT, msg.tid);
		rej.AddUint8(PNAS_IE_CAUSE, 1);
		Send(peer, rej);
		return;
	}

	const std::string response = msg.GetString(PNAS_IE_AUTH_RESPONSE);
	if (peer.nonce.empty() || response.empty())
	{
		// first the challenge
		peer.nonce = RandomHex(32);
		CPnasWriter ch(PNAS_UNAUTHENTICATED, msg.tid);
		ch.AddString(PNAS_IE_AUTH_CHALLENGE, peer.nonce, 32);
		Send(peer, ch);
		return;
	}

	// the realm the radio hashed with: ours, the one after the '@', or HyTalk's default
	std::vector<std::string> realms = { m_Realm, "hytera.com" };
	auto at = number.find('@');
	if (at != std::string::npos)
		realms.push_back(number.substr(at + 1));
	bool ok = false;
	for (const auto &realm : realms)
		if (PnasDigest(user, realm, account.password, peer.nonce) == response)
		{
			ok = true;
			break;
		}
	if (!ok)
	{
		std::cout << "HyTalk: wrong password for " << number << " from " << peer.addr << std::endl;
		peer.nonce.clear();
		CPnasWriter rej(PNAS_REGISTER_REJECT, msg.tid);
		rej.AddUint8(PNAS_IE_CAUSE, 2);
		Send(peer, rej);
		return;
	}

	// a radio that logs in again from somewhere else replaces its old session
	for (auto &[id, other] : m_Peers)
		if (other.get() != &peer && !other->dead && other->number == number)
			DropPeer(*other, "logged in again");

	peer.number = number;
	peer.callsign = account.callsign;
	peer.nonce.clear();

	const uint8_t *a = (const uint8_t *)&m_AddressBe;
	const uint8_t media[8] = { 0x00, uint8_t(m_RtpPort >> 8), uint8_t(m_RtpPort), 0x01, a[0], a[1], a[2], a[3] };
	// the radio connects to the GIS server's port + 2000
	const uint16_t gisUri = uint16_t(m_GisPort - 2000);
	const uint8_t gis[8] = { 0x00, uint8_t(gisUri >> 8), uint8_t(gisUri), 0x01, a[0], a[1], a[2], a[3] };
	CPnasWriter accept(PNAS_REGISTER_ACCEPT, msg.tid);
	accept.AddString(PNAS_IE_USER_NAME, peer.callsign, 52);
	accept.AddString(PNAS_IE_SUBSCRIBER_NUMBER, number);
	accept.AddString(PNAS_IE_REALM, m_Realm);
	accept.AddIpv4(PNAS_IE_MASTER_IP, m_AddressBe);
	accept.AddUint16(PNAS_IE_MASTER_PORT, m_Port);
	accept.AddBytes(PNAS_IE_AUDIO_RTP_RES, media, sizeof(media));
	accept.AddUint16(PNAS_IE_KEEPALIVE_TIME, HYTALK_KEEPALIVE_TIME);
	accept.AddIpv4(PNAS_IE_PRESENCE_SERVER, m_AddressBe);
	accept.AddUint16(PNAS_IE_HTTP_PORT, m_HttpPort);
	// the radio fetches its groups over HTTPS; without this port it tries port 0 and crashes
	accept.AddUint16(PNAS_IE_HTTPS_PORT, m_HttpPort);
	accept.AddBytes(PNAS_IE_GIS_SERVER, gis, sizeof(gis));
	Send(peer, accept);
	std::cout << "HyTalk: " << peer.callsign << " (" << number << ") accepted" << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////
// messages: text (and status, location) to a group or a radio

// IEs of a MessageRequest (0x42)
#define PNAS_IE_CALLED_NUMBER     0x0007
#define PNAS_IE_CALLER_NUMBER     0x0008
#define PNAS_IE_MSG_CONTAINER     0x001e
#define PNAS_IE_MSG_TYPE          0x001f   // 244 text, 245 long text, 240..243 picture/voice/video/document
#define PNAS_IE_MSG_REFERENCE     0x0036   // echoed in the response

// the text of a message container: UTF-8 with a 2 byte sequence number at the end,
// maybe behind a 2 byte length
static std::string MessageText(const std::vector<uint8_t> &v)
{
	size_t b = 0, e = v.size();
	if (e >= 2 && size_t((v[0] << 8) | v[1]) == e - 2)
		b = 2;
	if (e >= b + 2)
		e -= 2;
	std::string t;
	for (size_t i = b; i < e; i++)
		t += (v[i] >= 0x20) ? char(v[i]) : ' ';
	return t;
}

// The radio sends its message to us; we answer it and pass a copy, all IEs as they
// came (sender, group, container, time), to every other logged-in radio of the
// group, or to the radio it is for. Media (picture, voice, video, document) need a
// file server we do not have, so they fail on the radio instead of hanging.
void CHyTalkProtocol::OnMessageRequest(SPeer &peer, const CPnasMsg &msg)
{
	const uint32_t type = msg.GetUint(PNAS_IE_MSG_TYPE, 0);
	const bool media = (type >= 240 && type <= 243);

	CPnasWriter rsp(PNAS_MESSAGE_RESPONSE, msg.tid);
	rsp.AddUint8(PNAS_IE_CAUSE, media ? 1 : 0);
	if (const SPnasIe *ref = msg.Find(PNAS_IE_MSG_REFERENCE))
		rsp.AddBytes(PNAS_IE_MSG_REFERENCE, ref->value);
	Send(peer, rsp);
	if (media)
	{
		std::cout << "HyTalk: " << peer.callsign << " sent a media message (type " << type << "), not supported" << std::endl;
		return;
	}

	const SPnasIe *group = msg.Find(PNAS_IE_GROUP_NUMBER);
	const SPnasIe *called = msg.Find(PNAS_IE_CALLED_NUMBER);
	const std::string gdn = group ? GroupNumber(std::string(group->value.begin(), group->value.end())) : std::string();
	const std::string to = called ? GroupNumber(std::string(called->value.begin(), called->value.end())) : std::string();

	int n = 0;
	for (auto &[id, p] : m_Peers)
	{
		if (p.get() == &peer || p->dead || !p->online)
			continue;
		bool target = false;
		if (!gdn.empty())
			target = std::find(p->affiliated.begin(), p->affiliated.end(), gdn) != p->affiliated.end();
		else if (!to.empty())
			target = (GroupNumber(p->number) == to) || (AccountUser(p->number) == AccountUser(to));
		if (!target)
			continue;
		CPnasWriter fwd(PNAS_MESSAGE_REQUEST, (uint32_t)(rand() & 0xFF) + 1);
		for (const auto &ie : msg.ies)
		{
			if (ie.iei == PNAS_IE_GROUP_NUMBER)
				fwd.AddString(PNAS_IE_GROUP_NUMBER, GroupForRadio(gdn, p->number));
			else
				fwd.AddBytes(ie.iei, ie.value);
		}
		Send(*p, fwd);
		n++;
	}

	// the group of the echo module sends a text back to its sender
	const SGroup *eg = gdn.empty() ? nullptr : FindGroup(gdn);
	if (eg && eg->module == g_Reflector.GetEchoModule())
	{
		CPnasWriter echo(PNAS_MESSAGE_REQUEST, (uint32_t)(rand() & 0xFF) + 1);
		for (const auto &ie : msg.ies)
			echo.AddBytes(ie.iei, ie.value);
		Send(peer, echo);
		n++;
	}

	const SPnasIe *container = msg.Find(PNAS_IE_MSG_CONTAINER);
	std::cout << "HyTalk: " << (type == 244 || type == 245 ? "text" : "message type " + std::to_string(type)) << " from " << peer.callsign
	          << (gdn.empty() ? " to " + to : " to group " + gdn) << " for " << n << " radio(s)"
	          << (container && (type == 244 || type == 245) ? ": " + MessageText(container->value) : std::string()) << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////
// groups

CHyTalkProtocol::SGroup *CHyTalkProtocol::FindGroup(const std::string &gdn)
{
	const std::string n = GroupNumber(gdn);
	for (auto &g : m_Groups)
		if (g.gdn == n)
			return &g;
	return nullptr;
}

CHyTalkProtocol::SGroup *CHyTalkProtocol::FindGroup(char module)
{
	for (auto &g : m_Groups)
		if (g.module == module)
			return &g;
	return nullptr;
}

void CHyTalkProtocol::OnAffiliate(SPeer &peer, const CPnasMsg &msg)
{
	// GroupAffiNumList: group numbers, each as IE 0x0010
	std::vector<SPnasIe> list;
	const SPnasIe *ie = msg.Find(PNAS_IE_GROUP_AFFI_LIST);
	if (ie)
		CPnasMsg::ParseIes(ie->value.data(), ie->value.size(), list);

	peer.affiliated.clear();
	CPnasWriter rsp(PNAS_GROUP_AFFI_NOTIFY_RSP, msg.tid);
	CPnasWriter accepted(0, 0);
	for (const auto &g : list)
	{
		if (g.iei != PNAS_IE_GROUP_NUMBER)
			continue;
		const std::string gdn(g.value.begin(), g.value.end());
		if (!FindGroup(gdn))
			continue;
		peer.affiliated.push_back(GroupNumber(gdn));
		accepted.AddBytes(PNAS_IE_GROUP_NUMBER, g.value);
	}
	rsp.AddBytes(PNAS_IE_GROUP_AFFI_LIST, accepted.Body());
	rsp.AddUint8(PNAS_IE_CAUSE, 0);
	Send(peer, rsp);

	std::string s;
	for (const auto &a : peer.affiliated)
		s += (s.empty() ? "" : ",") + a;
	std::cout << "HyTalk: " << peer.callsign << " affiliated to " << (s.empty() ? "no group" : s) << std::endl;
	UpdatePeerClient(peer);

	// late entry: a group that is talking right now
	for (const auto &a : peer.affiliated)
	{
		SGroup *group = FindGroup(a);
		if (group && group->callId && peer.callGdn.empty())
		{
			InviteToCall(peer, *group);
			break;
		}
	}
}

void CHyTalkProtocol::UpdatePeerClient(SPeer &peer, bool remove)
{
	char mod = ' ';
	if (!remove && peer.online && !peer.affiliated.empty())
	{
		SGroup *g = FindGroup(peer.affiliated.front());
		if (g)
			mod = g->module;
	}

	CClients *clients = g_Reflector.GetClients();
	if (peer.client && (peer.clientModule != mod || !clients->IsClient(peer.client)))
	{
		clients->RemoveClient(peer.client);
		peer.client.reset();
	}
	if (mod != ' ')
	{
		if (peer.client)
			peer.client->Alive();
		else
		{
			CCallsign cs;
			cs.SetCallsign(peer.callsign, false);
			cs.SetCSModule(mod);
			peer.client = std::make_shared<CHyTalkClient>(cs, peer.addr, mod);
			clients->AddClient(peer.client);
		}
	}
	g_Reflector.ReleaseClients();
	peer.clientModule = mod;
}

////////////////////////////////////////////////////////////////////////////////////////
// group calls

// our offer and answer: AMBE+2, 60 ms per packet, payload type 87, to our RTP port
std::vector<uint8_t> CHyTalkProtocol::AudioDescription(void) const
{
	CPnasWriter d(0, 0);
	d.AddIpv4(PNAS_IE_IP_ADDRESS, m_AddressBe);
	d.AddUint16(PNAS_IE_PORT, m_RtpPort);
	d.AddUint8(PNAS_IE_CODEC_LIST, PNAS_CODEC_AMBE);
	const uint8_t ambe[2] = { 0x04, HYTALK_AMBE_PT };
	d.AddBytes(PNAS_IE_AMBE_PARAM, ambe, 2);
	d.AddUint8(PNAS_IE_CODEC_PRIORITY, 7);
	return d.Body();
}

// the RTP address in a radio's audio description, until its RTP shows the real one
void CHyTalkProtocol::LearnRtpAddress(SPeer &peer, const CPnasMsg &msg)
{
	const SPnasIe *ad = msg.Find(PNAS_IE_AUDIO_DESCRIPTION);
	if (!ad || peer.rtpKnown)
		return;
	std::vector<SPnasIe> sub;
	CPnasMsg::ParseIes(ad->value.data(), ad->value.size(), sub);
	uint16_t port = 0;
	for (const auto &s : sub)
	{
		if (s.iei == PNAS_IE_PORT && s.value.size() == 2)
			port = (uint16_t)((s.value[0] << 8) | s.value[1]);
		if (s.iei == PNAS_IE_CODEC_LIST && !s.value.empty() && !(s.value[0] & PNAS_CODEC_AMBE))
			std::cout << "HyTalk: " << peer.callsign << " did not take AMBE (codecs 0x" << std::hex << int(s.value[0]) << std::dec << ")" << std::endl;
	}
	if (peer.natRtpKnown)
		peer.rtpAddr = peer.natRtp;	// its public RTP address, from its keepalives
	else if (port)
	{
		// its signalled address is often a private one: the address it signals from is better
		peer.rtpAddr = CIp(AF_INET, port, peer.addr.GetAddress());
	}
}

void CHyTalkProtocol::EnsureGroupCall(SGroup &group)
{
	if (group.callId == 0)
	{
		group.callId = m_NextCallId++;
		if (m_NextCallId == 0)
			m_NextCallId = 1;
	}
	group.idleTimer.start();
}

// a server call request: the radio auto accepts a group call and answers with its RTP address
void CHyTalkProtocol::InviteToCall(SPeer &peer, SGroup &group)
{
	peer.callGdn = group.gdn;
	peer.callId = group.callId;
	peer.callTid = (uint32_t)(rand() & 0xFF) + 1;
	peer.callByRadio = false;
	peer.radioCallId = 0;
	peer.callUp = false;
	CPnasWriter req(PNAS_CALL_REQUEST, peer.callTid);
	req.AddUint32(PNAS_IE_CALL_ID, group.callId);
	req.AddUint8(PNAS_IE_CALL_TYPE, 3);
	const uint8_t attr[2] = { 0, 0 };
	req.AddBytes(PNAS_IE_CALL_ATTRIBUTE, attr, 2);
	req.AddString(PNAS_IE_GROUP_NUMBER, GroupForRadio(group.gdn, peer.number));
	req.AddString(PNAS_IE_CALLER_NUMBER, group.urfdTalking ? group.urfdTalker : group.gdn);
	req.AddBytes(PNAS_IE_AUDIO_DESCRIPTION, AudioDescription());
	Send(peer, req);
	FloorInformPeer(peer, group);
}

// who has the floor of a group, to one radio
void CHyTalkProtocol::FloorInformPeer(SPeer &peer, SGroup &group)
{
	CPnasWriter inf(PNAS_FLOOR_INFORM, (uint32_t)(rand() & 0xFF) + 1);
	inf.AddUint32(PNAS_IE_CALL_ID, group.callId);
	std::string number, name;
	if (group.talker)
	{
		auto it = m_Peers.find(group.talker);
		if (it != m_Peers.end())
		{
			number = it->second->number;
			name = it->second->callsign;
		}
	}
	else if (group.urfdTalking)
	{
		number = group.urfdTalker;
		name = group.urfdVia.empty() ? group.urfdTalker : group.urfdTalker + " " + group.urfdVia;
	}

	if (number.empty())
		inf.AddUint8(PNAS_IE_FLOOR_INFORM_TYPE, 1);	// idle
	else
	{
		inf.AddUint8(PNAS_IE_FLOOR_INFORM_TYPE, 0);	// taken
		inf.AddString(PNAS_IE_SUBSCRIBER_NUMBER, number);
		inf.AddString(PNAS_IE_USER_NAME, name, 52);
	}
	Send(peer, inf);
}

// the floor to everybody in the group call, invites the affiliated radios not yet in it
void CHyTalkProtocol::FloorInform(SGroup &group, uint32_t exceptPeer)
{
	for (auto &[id, p] : m_Peers)
	{
		SPeer &peer = *p;
		if (peer.dead || !peer.online || id == exceptPeer)
			continue;
		if (peer.callGdn == group.gdn)
			FloorInformPeer(peer, group);
		else if (peer.callGdn.empty() && std::find(peer.affiliated.begin(), peer.affiliated.end(), group.gdn) != peer.affiliated.end())
			InviteToCall(peer, group);
	}
}

// the talker of a group stopped: the floor is free
void CHyTalkProtocol::TalkerEnd(SGroup &group)
{
	if (group.talker)
	{
		auto it = m_Peers.find(group.talker);
		if (it != m_Peers.end())
			ClosePeerStream(*it->second);
	}
	group.talker = 0;
	group.urfdTalking = false;
	group.urfdTalker.clear();
	group.npending = 0;
	group.idleTimer.start();
	if (group.callId)
		FloorInform(group);
}

// the hang time is over: everybody leaves the group call
void CHyTalkProtocol::ReleaseGroupCall(SGroup &group)
{
	for (auto &[id, p] : m_Peers)
	{
		SPeer &peer = *p;
		if (peer.callGdn != group.gdn)
			continue;
		if (!peer.dead)
		{
			// The radio finds a call by transaction id and role, else by call id; a
			// release from us is a request in the server role, so for a call the radio
			// set up only its own call id from its CALL_REQUEST matches, not ours. It
			// answers with a release response; like any end it did not do itself, it
			// plays its call end tone (a setting on the radio).
			const uint32_t tid = peer.callTid ? peer.callTid : (uint32_t)(rand() & 0xFF) + 1;
			CPnasWriter rel(PNAS_CALL_RELEASE_REQUEST, tid);
			rel.AddUint32(PNAS_IE_CALL_ID, (peer.callByRadio && peer.radioCallId) ? peer.radioCallId : group.callId);
			rel.AddUint8(PNAS_IE_CAUSE, 0);
			Send(peer, rel);
		}
		peer.callGdn.clear();
		peer.callId = 0;
		peer.callTid = 0;
		peer.callByRadio = false;
		peer.radioCallId = 0;
		peer.callUp = false;
		peer.rtpKnown = false;
	}
	group.callId = 0;
}

// the radio pressed PTT on a group that has no call with it yet
void CHyTalkProtocol::OnCallRequest(SPeer &peer, const CPnasMsg &msg)
{
	const uint32_t radioCallId = msg.GetUint(PNAS_IE_CALL_ID);
	SGroup *group = FindGroup(msg.GetString(PNAS_IE_GROUP_NUMBER));
	if (!peer.online || !group)
	{
		CPnasWriter rej(PNAS_CALL_REJECT, msg.tid);
		rej.AddUint32(PNAS_IE_CALL_ID, radioCallId);
		rej.AddUint8(PNAS_IE_CAUSE, 28);	// group does not exist
		Send(peer, rej);
		return;
	}

	EnsureGroupCall(*group);
	peer.callGdn = group->gdn;
	peer.callId = group->callId;
	peer.callTid = msg.tid;
	peer.callByRadio = true;
	peer.radioCallId = radioCallId;
	peer.callUp = false;
	LearnRtpAddress(peer, msg);

	const bool wantsFloor = true;
	const bool free = (group->talker == 0 && !group->urfdTalking);
	CPnasWriter acc(PNAS_CALL_ACCEPT, msg.tid);
	acc.AddUint32(PNAS_IE_CALL_ID, group->callId);	// the radio takes our call id
	acc.AddUint8(PNAS_IE_CALL_TYPE, 3);
	if (wantsFloor && free)
		acc.AddUint8(PNAS_IE_FLOOR_STATUS, 0);	// granted: the radio transmits at once
	acc.AddBytes(PNAS_IE_AUDIO_DESCRIPTION, AudioDescription());
	Send(peer, acc);

	if (free)
	{
		group->talker = peer.id;
		std::cout << "HyTalk: " << peer.callsign << " talks on group " << group->gdn << " (module " << group->module << ")" << std::endl;
		FloorInform(*group, peer.id);
	}
	else
		FloorInformPeer(peer, *group);
}

// the radio answers our call request
void CHyTalkProtocol::OnCallAccept(SPeer &peer, const CPnasMsg &msg)
{
	if (msg.GetUint(PNAS_IE_CALL_ID) != peer.callId)
		return;
	peer.callUp = true;
	LearnRtpAddress(peer, msg);
}

void CHyTalkProtocol::OnCallRelease(SPeer &peer, const CPnasMsg &msg)
{
	const uint32_t callId = msg.GetUint(PNAS_IE_CALL_ID);
	if (msg.type == PNAS_CALL_RELEASE_REQUEST)
	{
		CPnasWriter rsp(PNAS_CALL_RELEASE_RESPONSE, msg.tid);
		rsp.AddUint32(PNAS_IE_CALL_ID, callId);
		Send(peer, rsp);
	}
	if (callId != peer.callId)
		return;
	SGroup *group = FindGroup(peer.callGdn);
	if (group && group->talker == peer.id)
		TalkerEnd(*group);
	peer.callGdn.clear();
	peer.callId = 0;
	peer.callTid = 0;
	peer.callByRadio = false;
	peer.radioCallId = 0;
	peer.callUp = false;
	peer.rtpKnown = false;
}

// PTT inside a running call
void CHyTalkProtocol::OnFloorRequest(SPeer &peer, const CPnasMsg &msg)
{
	const uint32_t callId = msg.GetUint(PNAS_IE_CALL_ID);
	SGroup *group = FindGroup(peer.callGdn);
	if (!group || callId != group->callId)
	{
		CPnasWriter rej(PNAS_FLOOR_REJECT, msg.tid);
		rej.AddUint32(PNAS_IE_CALL_ID, callId);
		rej.AddUint8(PNAS_IE_CAUSE, 1);
		Send(peer, rej);
		return;
	}
	if (group->talker == 0 && !group->urfdTalking)
	{
		CPnasWriter grant(PNAS_FLOOR_GRANT, msg.tid);
		grant.AddUint32(PNAS_IE_CALL_ID, callId);
		Send(peer, grant);
		group->talker = peer.id;
		group->idleTimer.start();
		std::cout << "HyTalk: " << peer.callsign << " talks on group " << group->gdn << " (module " << group->module << ")" << std::endl;
		FloorInform(*group, peer.id);
	}
	else if (group->talker != peer.id)
	{
		CPnasWriter rej(PNAS_FLOOR_REJECT, msg.tid);
		rej.AddUint32(PNAS_IE_CALL_ID, callId);
		rej.AddUint8(PNAS_IE_CAUSE, 1);
		Send(peer, rej);
	}
}

// PTT up
void CHyTalkProtocol::OnFloorRelease(SPeer &peer, const CPnasMsg &msg)
{
	const uint32_t callId = msg.GetUint(PNAS_IE_CALL_ID);
	CPnasWriter ack(PNAS_FLOOR_RELEASE_ACK, msg.tid);
	ack.AddUint32(PNAS_IE_CALL_ID, callId);
	Send(peer, ack);

	SGroup *group = FindGroup(peer.callGdn);
	if (group && group->talker == peer.id)
		TalkerEnd(*group);	// informs everybody, the releasing radio included (it waits for that)
	else if (group)
		FloorInformPeer(peer, *group);
}

////////////////////////////////////////////////////////////////////////////////////////
// media: RTP with AMBE+2, three 9 byte frames per packet

void CHyTalkProtocol::SendRtp(SPeer &peer, const uint8_t frames[][9], int n, bool marker)
{
	if (peer.rtpAddr.GetPort() == 0)
		return;
	if (peer.ssrc == 0)
	{
		std::mt19937 rng(std::random_device{}());
		peer.ssrc = rng() | 1;
		peer.rtpSeq = (uint16_t)rng();
		peer.rtpTs = rng();
	}
	uint8_t pkt[12 + HYTALK_AMBE_FRAMES * 9];
	pkt[0] = 0x80;
	pkt[1] = (marker ? 0x80 : 0x00) | HYTALK_AMBE_PT;
	pkt[2] = peer.rtpSeq >> 8;
	pkt[3] = peer.rtpSeq & 0xFF;
	for (int i = 0; i < 4; i++)
	{
		pkt[4 + i] = (peer.rtpTs >> (24 - 8 * i)) & 0xFF;
		pkt[8 + i] = (peer.ssrc >> (24 - 8 * i)) & 0xFF;
	}
	for (int i = 0; i < n; i++)
		memcpy(pkt + 12 + 9 * i, frames[i], 9);
	m_RtpSocket.Send(pkt, 12 + 9 * n, peer.rtpAddr);
	peer.rtpSeq++;
	peer.rtpTs += 160 * n;
}

// The radio keeps its carrier NAT open with 16 byte RTP packets of payload
// type 127 ("NAT") to the RTP and the RTCP port and wants them back; without
// the echo it repeats them, drops the call and logs in again after 120 s.
bool CHyTalkProtocol::EchoNatKeepalive(CUdpSocket &socket, const uint8_t *data, size_t len, const CIp &ip)
{
	if (len < 12 || len > 64 || (data[0] & 0xC0) != 0x80 || (data[1] & 0x7F) != 127)
		return false;
	socket.Send(data, len, ip);
	return true;
}

// A radio behind a carrier NAT signals its private RTP address; voice to it only
// gets through to the public address and port its keepalives come from. Taken
// for the radio with the same public IP, if only one is logged in from it or it
// had this address already.
void CHyTalkProtocol::LearnNatAddress(const CIp &ip)
{
	SPeer *match = nullptr;
	int n = 0;
	for (auto &[id, p] : m_Peers)
	{
		if (p->dead || !p->online || strcmp(p->addr.GetAddress(), ip.GetAddress()))
			continue;
		if (p->natRtpKnown && p->natRtp == ip)
			return;	// known already
		match = p.get();
		n++;
	}
	if (n != 1)
		return;
	match->natRtp = ip;
	match->natRtpKnown = true;
	if (!match->rtpKnown)
		match->rtpAddr = ip;	// a call that is up gets it at once
}

void CHyTalkProtocol::OnRtp(const uint8_t *data, size_t len, const CIp &ip)
{
	if (len < 12 || (data[0] & 0xC0) != 0x80)
		return;
	if (EchoNatKeepalive(m_RtpSocket, data, len, ip))
	{
		LearnNatAddress(ip);
		return;
	}
	// the radio: the one whose address it comes from, by IP if it changed ports
	SPeer *peer = nullptr;
	for (auto &[id, p] : m_Peers)
		if (!p->dead && p->online && p->rtpKnown && p->rtpAddr == ip)
		{
			peer = p.get();
			break;
		}
	if (!peer)
	{
		// first RTP of a radio: one in a call with this IP. Several radios can share
		// an address (carrier NAT), then the talker of its group, or the signalled port
		SPeer *byPort = nullptr, *any = nullptr;
		for (auto &[id, p] : m_Peers)
		{
			if (p->dead || !p->online || p->rtpKnown || p->callGdn.empty() || strcmp(p->addr.GetAddress(), ip.GetAddress()))
				continue;
			SGroup *g = FindGroup(p->callGdn);
			if (g && g->talker == p->id)
			{
				peer = p.get();
				break;
			}
			if (p->rtpAddr.GetPort() == ip.GetPort())
				byPort = p.get();
			if (!any)
				any = p.get();
		}
		if (!peer)
			peer = byPort ? byPort : any;
		if (!peer)
			return;
		peer->rtpAddr = ip;	// symmetric RTP: answer where it sends from
		peer->rtpKnown = true;
	}

	const size_t hdr = 12 + 4 * (data[0] & 0x0F) + ((data[0] & 0x10) ? 4 + 4 * ((data[12 + 4 * (data[0] & 0x0F) + 2] << 8) | data[12 + 4 * (data[0] & 0x0F) + 3]) : 0);
	if ((data[1] & 0x7F) != HYTALK_AMBE_PT || len < hdr)
		return;
	const uint8_t *payload = data + hdr;
	const size_t plen = len - hdr;
	if (plen == 0 || plen % 9)
		return;

	SGroup *group = FindGroup(peer->callGdn);
	if (!group || group->talker != peer->id)
		return;
	peer->lastRtp.start();

	const int n = (int)(plen / 9);
	// to the other radios in the call, as it came
	for (auto &[id, p] : m_Peers)
		if (p.get() != peer && !p->dead && p->callGdn == group->gdn)
			SendRtp(*p, (const uint8_t (*)[9])payload, n, false);

	// into urfd
	if (!OpenPeerStream(*peer, *group))
		return;
	auto sit = m_Streams.find(peer->streamId);
	if (sit == m_Streams.end() || !sit->second)
		return;
	for (int i = 0; i < n; i++)
	{
		auto frame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(payload + 9 * i, peer->streamId, peer->pid, false));
		peer->pid = (peer->pid + 1) % 6;
		frame->SetPacketModule(group->module);
		sit->second->Push(std::move(frame));
	}
}

bool CHyTalkProtocol::OpenPeerStream(SPeer &peer, SGroup &group)
{
	if (peer.streamId && (peer.rejected || m_Streams.count(peer.streamId)))
		return !peer.rejected;

	peer.streamId = 0x4800 | m_StreamCounter++;
	peer.streamModule = group.module;
	peer.rejected = false;
	peer.pid = 0;
	CCallsign my;
	my.SetCallsign(peer.callsign, true);
	CCallsign rpt1(g_Reflector.GetCallsign());
	rpt1.SetCSModule(group.module);
	CCallsign rpt2 = m_ReflectorCallsign;
	rpt2.SetCSModule(group.module);
	auto header = std::unique_ptr<CDvHeaderPacket>(new CDvHeaderPacket(my, CCallsign("CQCQCQ"), rpt1, rpt2, peer.streamId, true));
	OnDvHeaderPacketIn(header, peer.addr);
	if (peer.clientModule != group.module)
	{
		// OnDvHeaderPacketIn made a client for this module; ClosePeerStream removes it again
		CCallsign cs;
		cs.SetCallsign(my.GetBase(), false);
		cs.SetCSModule(group.module);
		CClients *clients = g_Reflector.GetClients();
		peer.talkClient = clients->FindClient(cs, group.module, peer.addr, EProtocol::hytalk);
		g_Reflector.ReleaseClients();
	}
	if (!m_Streams.count(peer.streamId))
	{
		peer.rejected = true;
		std::cout << "HyTalk: module " << group.module << " busy, " << peer.callsign << " reaches only the radios" << std::endl;
		return false;
	}
	std::cout << "HyTalk: stream opened for " << peer.callsign << " on module " << group.module << std::endl;
	return true;
}

void CHyTalkProtocol::ClosePeerStream(SPeer &peer)
{
	if (peer.streamId == 0)
		return;
	auto sit = m_Streams.find(peer.streamId);
	if (sit != m_Streams.end() && sit->second)
	{
		static const uint8_t silence[9] = { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };
		auto frame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(silence, peer.streamId, peer.pid, true));
		frame->SetPacketModule(peer.streamModule);
		sit->second->Push(std::move(frame));
		g_Reflector.CloseStream(sit->second);
		m_Streams.erase(sit);
	}
	if (peer.talkClient)
	{
		CClients *clients = g_Reflector.GetClients();
		clients->RemoveClient(peer.talkClient);
		g_Reflector.ReleaseClients();
		peer.talkClient.reset();
	}
	peer.streamId = 0;
	peer.rejected = false;
}

void CHyTalkProtocol::OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip)
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
	cs.SetCallsign(my.GetBase(), false);
	cs.SetCSModule(module);
	std::shared_ptr<CClient> client = clients->FindClient(cs, module, Ip, EProtocol::hytalk);
	if (!client)
	{
		clients->AddClient(std::make_shared<CHyTalkClient>(cs, Ip, module));
		client = clients->FindClient(cs, module, Ip, EProtocol::hytalk);
	}
	if (client)
	{
		client->Alive();
		client->SetReflectorModule(module);
		if ((stream = g_Reflector.OpenStream(Header, client)) != nullptr)
			m_Streams[stream->GetStreamId()] = stream;
	}
	g_Reflector.ReleaseClients();
	g_Reflector.GetUsers()->Hearing(my, rpt1, rpt2, "HyTalk");
	g_Reflector.ReleaseUsers();
}

// urfd audio to the radios of the module's group
void CHyTalkProtocol::HandleQueue(void)
{
	while (!m_Queue.IsEmpty())
	{
		auto packet = m_Queue.Pop();
		SGroup *group = FindGroup(packet->GetPacketModule());
		if (!group)
			continue;

		if (packet->IsDvHeader())
		{
			// our own radios' streams reach the other radios directly
			auto stream = g_Reflector.GetStream(group->module);
			if (stream)
			{
				auto owner = stream->GetOwnerClient();
				if (owner && owner->GetProtocol() == EProtocol::hytalk)
					continue;
			}
			if (group->talker)
				continue;	// a radio has the floor
			const CDvHeaderPacket &header = (const CDvHeaderPacket &)*packet;
			group->urfdTalking = true;
			group->urfdTalker = header.GetMyCallsign().GetBase();
			group->urfdVia = stream ? ViaText(*stream, group->module, group->urfdViaTg) : std::string();
			group->npending = 0;
			group->lastFrame.start();
			EnsureGroupCall(*group);
			FloorInform(*group);
		}
		else if (packet->IsDvFrame() && group->urfdTalking)
		{
			// the TG of a DMR or SVX stream is often set after its header: show it then
			if (!group->urfdViaTg && !group->urfdVia.empty())
			{
				auto stream = g_Reflector.GetStream(group->module);
				bool hasTg = false;
				const std::string via = stream ? ViaText(*stream, group->module, hasTg) : std::string();
				if (hasTg)
				{
					group->urfdVia = via;
					group->urfdViaTg = true;
					FloorInform(*group);
				}
			}
			group->lastFrame.start();
			group->idleTimer.start();
			const CDvFramePacket &frame = (const CDvFramePacket &)*packet;
			const uint8_t *ambe = frame.GetCodecData(ECodecType::dmr);
			if (ambe)
				memcpy(group->pending[group->npending++], ambe, 9);
			if (group->npending == HYTALK_AMBE_FRAMES || (packet->IsLastPacket() && group->npending))
			{
				for (auto &[id, p] : m_Peers)
					if (!p->dead && p->callGdn == group->gdn)
						SendRtp(*p, group->pending, group->npending, false);
				group->npending = 0;
			}
			if (packet->IsLastPacket())
				TalkerEnd(*group);
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// presence HTTP: the radios fetch their group list here

// a non-blocking TCP listener, -1 if the port can't be had
int CHyTalkProtocol::TcpListen(uint16_t port, const char *what) const
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	CIp ip(AF_INET, port, g_Configure.GetString(g_Keys.ip.ipv4bind).c_str());
	if (bind(fd, ip.GetCPointer(), ip.GetSize()) != 0 || listen(fd, 8) != 0)
	{
		std::cerr << "HyTalk: can't listen on " << what << " port " << port << std::endl;
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

// the user profile: where to log in (tcf_server_1, "ip:port" of PNAS on UDP)
// and ppoc_registers_switch 1, which turns a radio that asked with its serial
// number (application/sn+json) to the account login; with 0 it would log in
// by serial number, which we do not do. RedundMasterHttpsServer must stay
// empty, else the radio replaces the server it is programmed with.
std::string CHyTalkProtocol::ProfileAnswer(const std::string &request, const std::string &body, const std::string &addr) const
{
	std::cout << "HyTalk: user profile request from " << addr << ": " << body.substr(0, 200) << std::endl;
	if (request.find("/MPTTVS/mptt-user-profile") == std::string::npos)
		return "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	const std::string port = std::to_string(m_Port);
	const std::string json = "{\"serverIP\":{\"tcf_server_1\":\"" + m_Address + ":" + port + "\","
		"\"dmshttpport\":" + port + ",\"dmshttpsport\":" + port + ","
		"\"RedundMasterHttpsServer\":\"\",\"RedundSlaveHttpsServer\":\"\","
		"\"localrealm\":\"\",\"peerrealm\":\"\",\"app_version_server\":\"\",\"Wssa_server\":\"\","
		"\"ppoc_registers_switch\":1},\"versionOptionFeatures\":[]}";
	return "HTTP/1.1 200 OK\r\nContent-Type: application/json;charset=UTF-8\r\nContent-Length: "
		+ std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
}

// the version list: per group "<len 2 digits><gdn><attribute 1 digit><version 4 digits>",
// the group list: GDN, name and the property digits type/attribute/encryption/dynamic
std::string CHyTalkProtocol::HttpAnswer(const std::string &request) const
{
	std::string body;
	if (request.find("/groupversionlist/") != std::string::npos)
	{
		std::string list;
		char ver[8];
		snprintf(ver, sizeof(ver), "%04u", m_GroupVersion % 10000);
		for (const auto &g : m_Groups)
		{
			char len[4];
			snprintf(len, sizeof(len), "%02u", (unsigned)g.gdn.size());
			list += std::string(len) + g.gdn + "0" + ver;
		}
		body = "{\"groupversionlist\":{\"version\":\"" + std::to_string(m_GroupVersion) + "\",\"grouplist\":\"" + list + "\"}}";
	}
	else if (request.find("/groupinfolist/") != std::string::npos)
	{
		// the groups belong to the radio's realm, the part after the '@' of the
		// user in the path (the radio's enterprise id), else ours
		std::string realm = m_Realm;
		const auto at = request.find('@');
		if (at != std::string::npos)
		{
			const auto end = request.find_first_of("/ ?", at + 1);
			std::string r = request.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
			if (!r.empty() && r.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-_") == std::string::npos)
				realm = r;
		}
		body = "{\"grouplist\":{\"realmlist\":[{\"realm\":\"" + realm + "\",\"group\":[";
		bool first = true;
		for (const auto &g : m_Groups)
		{
			std::string name;
			for (char c : g.name)
				if (c != '"' && c != '\\')
					name += c;
			body += std::string(first ? "" : ",") + "{\"GDN\":\"" + g.gdn + "\",\"GName\":\"" + name + "\",\"GPrivilege\":\"\",\"GrpProper\":\"0000\",\"Creator\":\"\"}";
			first = false;
		}
		body += "]}]}}";
	}
	else
		return "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	return "HTTP/1.1 200 OK\r\nContent-Type: application/json;charset=UTF-8\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

////////////////////////////////////////////////////////////////////////////////////////
// GTRP: the radios log in, keep alive and report their GPS position here

// the online radio of a GTRP number: the GTRP number is the business id (may
// be empty) followed by the subscriber number without its domain
CHyTalkProtocol::SPeer *CHyTalkProtocol::FindPeerByNumber(const std::string &number)
{
	for (auto &[id, p] : m_Peers)
	{
		if (p->dead || !p->online)
			continue;
		for (const std::string &own : { p->number.substr(0, p->number.find('@')), AccountUser(p->number) })
			if (!own.empty() && number.size() >= own.size() && number.compare(number.size() - own.size(), own.size(), own) == 0)
				return p.get();
	}
	return nullptr;
}

void CHyTalkProtocol::SendGis(SGisConn &conn, const std::vector<uint8_t> &m)
{
	if (send(conn.fd, m.data(), m.size(), MSG_NOSIGNAL) < 0) {}
}

// false closes the connection
bool CHyTalkProtocol::OnGisMessage(SGisConn &conn, const SGtrpMsg &msg)
{
	if (m_Debug)
	{
		std::cout << "HyTalk debug: GIS <- 0x" << std::hex << int(msg.type) << std::dec << " tid " << msg.tid
		          << (msg.own.empty() ? "" : " number " + msg.own) << (msg.result >= 0 ? " result " + std::to_string(msg.result) : "");
		if (msg.hasPosition)
			std::cout << " position " << msg.pos.lat << "," << msg.pos.lon << (msg.pos.valid ? " (fix)" : " (no fix)");
		std::cout << std::endl;
	}
	switch (msg.type)
	{
	case GTRP_REGISTER_REQUEST:
	{
		// the logged-in radio, else its account: the radio reconnects its GIS
		// channel on its own, also before its PNAS login (after an urfd restart)
		std::string callsign;
		if (SPeer *peer = FindPeerByNumber(msg.own))
			callsign = peer->callsign;
		else
		{
			std::lock_guard<std::mutex> lock(m_AccountMutex);
			for (const auto &[user, account] : m_Accounts)
				if (!user.empty() && msg.own.size() >= user.size() && msg.own.compare(msg.own.size() - user.size(), user.size(), user) == 0)
				{
					callsign = account.callsign;
					break;
				}
		}
		if (callsign.empty())
		{
			SCredential cred;
			const std::string user = AccountUser(msg.own);
			if (CCredentialStore::EResult::found == g_Credentials.Lookup(ECredService::hytalk, user, cred) && !cred.password.empty())
				callsign = cred.callsign.empty() ? user : cred.callsign;
		}
		if (callsign.empty())
		{
			std::cout << "HyTalk: GIS login of unknown number " << msg.own << " rejected" << std::endl;
			SendGis(conn, CGtrp::BaseResponse(GTRP_REGISTER_RESPONSE, msg.tid, 1));
			return false;
		}
		conn.number = msg.own;
		conn.callsign = callsign;
		SendGis(conn, CGtrp::BaseResponse(GTRP_REGISTER_RESPONSE, msg.tid, 0));
		// the radio reports periodically only once the server asks for it
		SendGis(conn, CGtrp::GpsConfig(m_GisTid++, m_GisInterval, true));
		std::cout << "HyTalk: " << conn.callsign << " logged in to GIS, position every " << m_GisInterval << " s" << std::endl;
		return true;
	}
	case GTRP_KEEP_ALIVE:
		SendGis(conn, CGtrp::BaseResponse(GTRP_KEEP_ALIVE, msg.tid, 0));
		return true;
	case GTRP_DEREGISTER_REQUEST:
		SendGis(conn, CGtrp::BaseResponse(GTRP_DEREGISTER_RESPONSE, msg.tid, 0));
		return false;
	case GTRP_LOCATION_REPORTING:
	case GTRP_LOCATION_RESPONSE:
	{
		if (conn.callsign.empty() || !msg.hasPosition || !msg.pos.valid)
			return true;
		// GTRP has no password: a position counts only from the address of the
		// radio that logged in with that number over PNAS
		SPeer *peer = FindPeerByNumber(conn.number);
		if (!peer || conn.addr != peer->addr.GetAddress())
		{
			if (!conn.warned)
				std::cout << "HyTalk: GIS positions of " << conn.number << " from " << conn.addr << " ignored, no radio logged in with it from there" << std::endl;
			conn.warned = true;
			return true;
		}
		conn.warned = false;
		peer->locator = CUser::Locator(msg.pos.lat, msg.pos.lon);
		g_Reflector.GetUsers()->Location(CCallsign(conn.callsign), msg.pos.lat, msg.pos.lon);
		g_Reflector.ReleaseUsers();
		return true;
	}
	default:
		return true;  // acks of our config request and anything else
	}
}

void CHyTalkProtocol::GisTask(void)
{
	if (m_GisFd >= 0)
	{
		int fd;
		struct sockaddr_storage ss;
		socklen_t sl = sizeof(ss);
		while ((fd = accept(m_GisFd, (struct sockaddr *)&ss, &sl)) >= 0)
		{
			fcntl(fd, F_SETFL, O_NONBLOCK);
			char host[INET6_ADDRSTRLEN] = "";
			if (ss.ss_family == AF_INET)
				inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, host, sizeof(host));
			else if (ss.ss_family == AF_INET6)
				inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, host, sizeof(host));
			m_GisConns.push_back({ fd, {}, CTimer(), {}, {}, host, false });
			sl = sizeof(ss);
		}
	}
	for (auto it = m_GisConns.begin(); it != m_GisConns.end(); )
	{
		uint8_t buf[2048];
		ssize_t n;
		bool closeIt = false;
		while ((n = recv(it->fd, buf, sizeof(buf), 0)) > 0)
		{
			it->in.insert(it->in.end(), buf, buf + n);
			it->lastRx.start();
		}
		if (n == 0 || it->in.size() > 65536)
			closeIt = true;
		long size;
		while (!closeIt && (size = CGtrp::MessageSize(it->in.data(), it->in.size())) != 0)
		{
			SGtrpMsg msg;
			if (size < 0 || !CGtrp::Decode(it->in.data(), size_t(size), msg) || !OnGisMessage(*it, msg))
				closeIt = true;
			else
				it->in.erase(it->in.begin(), it->in.begin() + size);
		}
		if (closeIt || it->lastRx.time() > HYTALK_GIS_TIMEOUT)
		{
			close(it->fd);
			it = m_GisConns.erase(it);
		}
		else
			++it;
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// task

void CHyTalkProtocol::Task(void)
{
	// signalling and voice, waiting at most 10 ms (KCP's interval)
	struct pollfd pfd[3] = { { m_Socket4.GetSocket(), POLLIN, 0 }, { m_RtpSocket.GetSocket(), POLLIN, 0 }, { m_RtcpSocket.GetSocket(), POLLIN, 0 } };
	if (poll(pfd, 3, 10) > 0)
	{
		uint8_t buf[2048];
		struct sockaddr_storage sa;
		socklen_t salen;
		ssize_t n;
		for (int s = 0; s < 3; s++)
		{
			if (!(pfd[s].revents & POLLIN))
				continue;
			for (int i = 0; i < 64; i++)
			{
				salen = sizeof(sa);
				n = recvfrom(pfd[s].fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&sa, &salen);
				if (n <= 0)
					break;
				CIp ip;
				memcpy(ip.GetPointer(), &sa, std::min((size_t)salen, sizeof(sa)));
				if (s == 0)
					OnDatagram(buf, (size_t)n, ip);
				else if (s == 1)
					OnRtp(buf, (size_t)n, ip);
				else
					EchoNatKeepalive(m_RtcpSocket, buf, (size_t)n, ip);	// RTCP reports are not needed
			}
		}
	}

	const uint32_t now = KcpNow();
	for (auto &[id, peer] : m_Peers)
		if (peer->kcp && !peer->dead)
			ikcp_update(peer->kcp, now);

	// held heartbeat pongs that are due
	for (auto &[id, p] : m_Peers)
		if (p->pongPending && !p->dead && p->pongTimer.time() >= p->pongDelay)
		{
			p->pongPending = false;
			CPnasWriter pong(PNAS_HEARTBEAT_PONG, p->pongTid);
			pong.AddUint8(PNAS_IE_CAUSE, 0);
			Send(*p, pong);
		}

	m_Profile.Task();
	m_Presence.Task();
	GisTask();
	if (m_Debug && m_DebugTimer.time() >= 10)
	{
		m_DebugTimer.start();
		DebugKcpState();
	}

	// talkers that went silent, urfd transmissions without their last frame, hang time
	for (auto &g : m_Groups)
	{
		if (g.talker)
		{
			auto it = m_Peers.find(g.talker);
			if (it == m_Peers.end() || it->second->dead || (it->second->streamId && it->second->lastRtp.time() > HYTALK_TALKER_TIMEOUT))
				TalkerEnd(g);
		}
		if (g.urfdTalking && g.lastFrame.time() > HYTALK_URFD_TIMEOUT)
			TalkerEnd(g);
		if (g.callId && !g.talker && !g.urfdTalking && g.idleTimer.time() > m_HangTime)
			ReleaseGroupCall(g);
	}

	// timeouts, dead sessions
	for (auto it = m_Peers.begin(); it != m_Peers.end(); )
	{
		SPeer &peer = *it->second;
		if (!peer.dead && peer.lastRx.time() > HYTALK_PEER_TIMEOUT)
			DropPeer(peer, "timeout");
		if (!peer.dead && !peer.online && peer.since.time() > HYTALK_LOGIN_TIMEOUT)
			DropPeer(peer, "no login");
		if (peer.dead)
		{
			if (peer.kcp)
				ikcp_release(peer.kcp);
			it = m_Peers.erase(it);
		}
		else
			++it;
	}

	HandleQueue();
	CheckStreamsTimeout();

	if (m_SnapshotTimer.time() >= 1)
	{
		UpdateSnapshot();
		m_SnapshotTimer.start();
	}

	if (m_AliveTimer.time() >= 5)
	{
		for (auto &[id, peer] : m_Peers)
			if (peer->online)
				UpdatePeerClient(*peer);
		m_AliveTimer.start();
	}
}

void CHyTalkProtocol::HandleKeepalives(void)
{
}

////////////////////////////////////////////////////////////////////////////////////////
// admin: accounts, groups, connected radios

// an account in urfd.ini stays there, a new one goes to the credential database if there is one
bool CHyTalkProtocol::AddAccount(const std::string &number, const std::string &password, const std::string &callsign)
{
	const std::string user = AccountUser(number);
	if (user.empty() || password.empty())
		return false;
	bool inIni;
	{
		std::lock_guard<std::mutex> lock(m_AccountMutex);
		inIni = (m_Accounts.count(user) > 0);
	}
	if (!inIni && g_Credentials.Enabled())
	{
		if (!g_Credentials.Store(ECredService::hytalk, user, { password, callsign }))
			return false;
		std::cout << "HyTalk: account " << user << " added to the credential database" << std::endl;
		return true;
	}
	{
		std::lock_guard<std::mutex> lock(m_AccountMutex);
		m_Accounts[user] = { password, callsign.empty() ? user : callsign };
	}
	const std::string value = callsign.empty() ? password : (password + "," + callsign);
	IniWriteAccount(user, &value);
	std::cout << "HyTalk: account " << user << " added" << std::endl;
	return true;
}

// removes the account from urfd.ini and from the credential database
int CHyTalkProtocol::RemoveAccount(const std::string &number)
{
	const std::string user = AccountUser(number);
	bool inIni;
	{
		std::lock_guard<std::mutex> lock(m_AccountMutex);
		inIni = (m_Accounts.erase(user) > 0);
	}
	if (inIni)
	{
		IniWriteAccount(user, nullptr);
		std::cout << "HyTalk: account " << user << " removed" << std::endl;
	}
	const int inDb = g_Credentials.Remove(ECredService::hytalk, user);
	if (inDb > 0)
		std::cout << "HyTalk: account " << user << " removed from the credential database" << std::endl;
	if (inDb < 0)
		return -1;	// it may still be in the database
	return (inIni || inDb > 0) ? 1 : 0;
}

// urfd.ini's accounts, then the database's that urfd.ini does not have (it wins at the login)
std::vector<CHyTalkProtocol::SAccountInfo> CHyTalkProtocol::GetAccounts(void) const
{
	std::vector<SAccountInfo> v;
	{
		std::lock_guard<std::mutex> lock(m_AccountMutex);
		for (const auto &[user, acc] : m_Accounts)
			v.push_back({ user, acc.callsign, "ini" });
	}
	std::vector<std::pair<std::string, std::string>> dbUsers;
	g_Credentials.List(ECredService::hytalk, dbUsers);
	for (const auto &u : dbUsers)
		if (std::none_of(v.begin(), v.end(), [&u](const SAccountInfo &a) { return a.number == u.first; }))
			v.push_back({ u.first, u.second.empty() ? u.first : u.second, "db" });
	return v;
}

// the group list is fixed at start
std::vector<CHyTalkProtocol::SGroupInfo> CHyTalkProtocol::GetGroups(void) const
{
	std::vector<SGroupInfo> v;
	for (const auto &g : m_Groups)
		v.push_back({ g.gdn, g.name, g.module });
	return v;
}

std::vector<CHyTalkProtocol::SConnectedRadio> CHyTalkProtocol::GetConnectedRadios(void) const
{
	std::lock_guard<std::mutex> lock(m_SnapshotMutex);
	return m_Snapshot;
}

// m_Peers belongs to the protocol thread, the admin socket reads this copy
void CHyTalkProtocol::UpdateSnapshot(void)
{
	std::vector<SConnectedRadio> v;
	for (const auto &[id, p] : m_Peers)
	{
		if (p->dead || !p->online)
			continue;
		SConnectedRadio r;
		r.callsign = p->callsign;
		r.number = p->number;
		r.ip = p->addr.GetAddress();
		r.groups = p->affiliated;
		r.callGroup = p->callGdn;
		r.talking = false;
		for (const auto &g : m_Groups)
			if (g.talker == id)
				r.talking = true;
		r.connectedSince = p->onlineSince;
		r.location = p->locator;
		v.push_back(r);
	}
	std::lock_guard<std::mutex> lock(m_SnapshotMutex);
	m_Snapshot.swap(v);
}

// sets (value) or removes (nullptr) the account line "<number> = <value>" in [HyTalk]
bool CHyTalkProtocol::IniWriteAccount(const std::string &number, const std::string *value)
{
	const std::string &path = g_Reflector.GetConfigPath();
	if (path.empty())
		return false;
	std::ifstream in(path);
	if (!in.is_open())
		return false;
	std::vector<std::string> lines;
	std::string line;
	while (std::getline(in, line))
		lines.push_back(line);
	in.close();

	bool inSection = false, found = false;
	int insertPos = -1;
	for (size_t i = 0; i < lines.size(); i++)
	{
		const std::string t = Trim(lines[i]);
		if (!t.empty() && t[0] == '[')
		{
			if (inSection)
				break;
			inSection = (t.find("[HyTalk]") == 0);
			// a section without keys gets the account right below its header
			if (inSection)
				insertPos = (int)i + 1;
			continue;
		}
		if (!inSection || t.empty() || t[0] == '#' || t[0] == ';')
			continue;
		auto eq = t.find('=');
		if (eq == std::string::npos)
			continue;
		insertPos = (int)i + 1;
		// keys as Initialize reads them: "123456789012" and "1234567@realm" are account 1234567 too
		if (AccountUser(Trim(t.substr(0, eq))) == number)
		{
			found = true;
			if (value)
				lines[i] = number + " = " + *value;
			else
				lines.erase(lines.begin() + i);
			break;
		}
	}
	if (!found)
	{
		if (!value)
			return false;
		if (insertPos < 0)
		{
			lines.push_back("");
			lines.push_back("[HyTalk]");
			insertPos = (int)lines.size();
		}
		lines.insert(lines.begin() + insertPos, number + " = " + *value);
	}
	std::ofstream out(path);
	if (!out.is_open())
		return false;
	for (const auto &l : lines)
		out << l << "\n";
	return true;
}
