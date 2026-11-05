#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <sstream>

#include "Global.h"
#include "Notifier.h"
#include "DMRMMDVMClient.h"
#include "websocket_lws_notifier.h"

// an IP shown as far as the mode allows, empty when it is hidden: the last
// 1, 2 or 3 bytes of an IPv4 address, the last 1, 2 or 3 groups of an IPv6 one
static std::string MaskIP(const std::string &ip, const std::string &mode, const std::string &mc)
{
	if (mode == "ShowFullIP")
		return ip;
	if (mode != "ShowLast1ByteOfIP" && mode != "ShowLast2ByteOfIP" && mode != "ShowLast3ByteOfIP")
		return "";
	const int keep = mode[8] - '0';
	unsigned char b[16];
	if (inet_pton(AF_INET6, ip.c_str(), b) == 1)
	{
		// an IPv4 address mapped into IPv6 (::ffff:1.2.3.4) as the IPv4 address it is
		static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
		if (memcmp(b, mapped, 12) == 0)
		{
			char v4[INET_ADDRSTRLEN];
			if (!inet_ntop(AF_INET, b + 12, v4, sizeof(v4)))
				return "";
			return MaskIP(v4, mode, mc);
		}
		std::ostringstream out;
		out << std::hex;
		for (int i = 0; i < 8; i++)
		{
			if (i)
				out << ':';
			if (i < 8 - keep)
				out << mc;
			else
				out << ((unsigned(b[2 * i]) << 8) | b[2 * i + 1]);
		}
		return out.str();
	}
	if (inet_pton(AF_INET, ip.c_str(), b) == 1)
	{
		std::string out;
		for (int i = 0; i < 4; i++)
			out += (i ? "." : "") + (i < 4 - keep ? mc : std::to_string(b[i]));
		return out;
	}
	return "";
}

static void AddIP(nlohmann::json &j, const CIp &ip, const std::string &modeKey)
{
	const auto shown = MaskIP(ip.GetAddress(), g_Configure.GetString(modeKey), g_Configure.GetString(g_Keys.dashboard.ipmask));
	if (!shown.empty())
		j["ip"] = shown;
}

nlohmann::json NodeJson(const CClient &client)
{
	nlohmann::json j = {
		{ "callsign", client.GetCallsign().GetCS() },
		{ "protocol", client.GetProtocolName() },
		{ "reflector_module", std::string(1, client.GetReflectorModule()) },
		{ "connect_time", client.GetConnectTime() },
		{ "last_heard_time", client.GetLastHeardTime() } };
	// every module it is linked to: a DMR hotspot has one per timeslot
	std::string modules;
	for (char m = 'A'; m <= 'Z'; m++)
		if (client.IsLinkedTo(m))
			modules += m;
	j["modules"] = modules;
	if (const auto *dmr = dynamic_cast<const CDmrmmdvmClient *>(&client))
	{
		if (dmr->GetSlotModule(1) != ' ')
			j["ts1"] = std::string(1, dmr->GetSlotModule(1));
		if (dmr->GetSlotModule(2) != ' ')
			j["ts2"] = std::string(1, dmr->GetSlotModule(2));
	}
	AddIP(j, client.GetIp(), g_Keys.dashboard.linksip);
	return j;
}

nlohmann::json PeerJson(const CPeer &peer)
{
	nlohmann::json clients = nlohmann::json::array();
	for (auto cit = peer.cbegin(); cit != peer.cend(); ++cit)
		clients.push_back({
			{ "callsign", (*cit)->GetCallsign().GetCS() },
			{ "protocol", (*cit)->GetProtocolName() },
			{ "reflector_module", std::string(1, (*cit)->GetReflectorModule()) } });
	nlohmann::json j = {
		{ "callsign", peer.GetCallsign().GetCS() },
		{ "protocol", peer.GetProtocolName() },
		{ "modules", std::string(peer.GetReflectorModules()) },
		{ "connect_time", peer.GetConnectTime() },
		{ "last_heard_time", peer.GetLastHeardTime() },
		{ "clients", clients } };
	AddIP(j, peer.GetIp(), g_Keys.dashboard.peersip);
	return j;
}

nlohmann::json CEvent::ToJson() const
{
	nlohmann::json j;
	j["event_type"] = event_type;
	j["timestamp"] = timestamp;
	if (!reflector_name.empty())
		j["reflector_name"] = reflector_name;
	j["payload"] = payload.is_object() ? payload : nlohmann::json::object();
	return j;
}

CNotifierManager &CNotifierManager::Instance()
{
	static CNotifierManager instance;
	return instance;
}

CNotifierManager::CNotifierManager() {}

CNotifierManager::~CNotifierManager()
{
	Stop();
}

void CNotifierManager::Start(void)
{
	if (!g_Configure.Contains(g_Keys.ws.enable) || !g_Configure.GetBoolean(g_Keys.ws.enable))
	{
		std::cout << "WebSocket: disabled, the dashboard shows nothing without it" << std::endl;
		return;
	}
	m_ReflectorName = g_Reflector.GetCallsign().GetCS();
	m_ReflectorName.erase(m_ReflectorName.find_last_not_of(' ') + 1);	// GetCS() pads to 8
	const std::string bind = g_Configure.Contains(g_Keys.ws.bindaddress) ? g_Configure.GetString(g_Keys.ws.bindaddress) : "127.0.0.1";
	const auto port = (uint16_t)g_Configure.GetUnsigned(g_Keys.ws.port);
	m_Ws = std::make_unique<CWebsocketLwsNotifier>();
	if (m_Ws->Start(bind, port,
		(int)g_Configure.GetUnsigned(g_Keys.ws.maxclients),
		(int)g_Configure.GetUnsigned(g_Keys.ws.queuelen),
		(int)g_Configure.GetUnsigned(g_Keys.ws.pinginterval),
		(int)g_Configure.GetUnsigned(g_Keys.ws.idletimeout)))
	{
		m_DeferStop = false;
		m_DeferThread = std::thread(&CNotifierManager::DeferLoop, this);
		m_Running = true;
		std::cout << "WebSocket: listening on " << bind << ":" << port << std::endl;
	}
	else
	{
		std::cerr << "WebSocket: failed to listen on " << bind << ":" << port << std::endl;
		m_Ws.reset();
	}
}

void CNotifierManager::Stop(void)
{
	m_Running = false;
	{
		std::lock_guard<std::mutex> lk(m_DeferMutex);
		m_DeferStop = true;
		m_Deferred.clear();
	}
	m_DeferCv.notify_all();
	if (m_DeferThread.joinable())
		m_DeferThread.join();
	// the object stays until exit: a Dispatch() that already saw m_Running
	// still finds it, and Publish() on a stopped listener does nothing
	if (m_Ws)
		m_Ws->Stop();
}

#define DEFER_MAX 4096	// events waiting; more means the thread is stuck, a dashboard catches up with its next snapshot

void CNotifierManager::Defer(const std::string &type, std::function<nlohmann::json()> payload)
{
	{
		std::lock_guard<std::mutex> lk(m_DeferMutex);
		if (m_DeferStop)
			return;
		if (m_Deferred.size() >= DEFER_MAX)
		{
			if (!m_DeferFull)
				std::cerr << "WebSocket: " << DEFER_MAX << " events waiting, dropping " << type << " and what follows until there is room" << std::endl;
			m_DeferFull = true;
			return;
		}
		m_DeferFull = false;
		m_Deferred.emplace_back(type, std::move(payload));
	}
	m_DeferCv.notify_one();
}

void CNotifierManager::DeferLoop(void)
{
	while (true)
	{
		std::pair<std::string, std::function<nlohmann::json()>> next;
		{
			std::unique_lock<std::mutex> lk(m_DeferMutex);
			m_DeferCv.wait(lk, [this] { return m_DeferStop || !m_Deferred.empty(); });
			if (m_DeferStop)
				return;
			next = std::move(m_Deferred.front());
			m_Deferred.pop_front();
		}
		try
		{
			Send(CEvent(next.first, next.second(), m_ReflectorName));
		}
		catch (const std::exception &ex)
		{
			std::cerr << "WebSocket: cannot build " << next.first << ": " << ex.what() << std::endl;
		}
	}
}

void CNotifierManager::Send(const CEvent &ev)
{
	try
	{
		// callsigns come from the air: replace invalid UTF-8 instead of throwing
		m_Ws->Publish(ev.ToJson().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
	}
	catch (const std::exception &ex)
	{
		std::cerr << "WebSocket: cannot send " << ev.event_type << ": " << ex.what() << std::endl;
	}
}
