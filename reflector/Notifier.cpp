#include <iostream>
#include <vector>

#include "Global.h"
#include "Notifier.h"
#include "DMRMMDVMClient.h"
#include "websocket_lws_notifier.h"

// an IP shown as far as the mode allows, empty when it is hidden
static std::string MaskIP(const std::string &ip, const std::string &mode, const std::string &mc)
{
	if (mode == "ShowFullIP")
		return ip;
	if (mode != "ShowLast1ByteOfIP" && mode != "ShowLast2ByteOfIP" && mode != "ShowLast3ByteOfIP")
		return "";
	const int keep = mode[8] - '0';
	// an IPv4 mapped into IPv6 (::ffff:1.2.3.4) as the IPv4 it is
	if (ip.find('.') != std::string::npos && ip.find(':') != std::string::npos)
		return MaskIP(ip.substr(ip.rfind(':') + 1), mode, mc);
	if (ip.find(':') == std::string::npos)
	{
		// IPv4: the last <keep> bytes
		std::vector<std::string> bytes;
		std::string::size_type start = 0, dot;
		while ((dot = ip.find('.', start)) != std::string::npos)
		{
			bytes.push_back(ip.substr(start, dot - start));
			start = dot + 1;
		}
		bytes.push_back(ip.substr(start));
		if (bytes.size() != 4)
			return "";
		std::string out;
		for (int i = 0; i < 4; i++)
			out += (i ? "." : "") + (i < 4 - keep ? mc : bytes[i]);
		return out;
	}
	// IPv6: the last two groups
	const auto colon = ip.rfind(':');
	const auto before = (colon == std::string::npos || colon == 0) ? std::string::npos : ip.rfind(':', colon - 1);
	if (before == std::string::npos)
		return "";
	std::string out;
	for (int i = 0; i < 6; i++)
		out += mc + ":";
	auto group = [](const std::string &g) { return g.empty() ? std::string("0") : g; };
	return out + group(ip.substr(before + 1, colon - before - 1)) + ":" + group(ip.substr(colon + 1));
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
		std::cout << "WebSocket: disabled" << std::endl;
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
	// the object stays until exit: a Dispatch() that already saw m_Running
	// still finds it, and Publish() on a stopped listener does nothing
	if (m_Ws)
		m_Ws->Stop();
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
