#include <iostream>

#include "Global.h"
#include "Notifier.h"
#include "websocket_lws_notifier.h"

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
