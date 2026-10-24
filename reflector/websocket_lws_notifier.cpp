#include <algorithm>
#include <cstring>
#include <ctime>
#include <iostream>
#include <optional>
#include <vector>
#include <nlohmann/json.hpp>

#include "Global.h"
#include "Notifier.h"
#include "MmdvmClientConfig.h"
#include "NXDNProtocol.h"
#include "websocket_lws_notifier.h"

////////////////////////////////////////////////////////////////////////////////////////
// what a new client gets first: reflector, modules and protocols, then the state

static nlohmann::json BuildInitialConnectPayload()
{
	const auto now = std::time(nullptr);
	const auto started = g_Reflector.GetStartedAt();

	nlohmann::json payload = nlohmann::json::object();
	payload["server_started"] = started;
	payload["current_time"] = now;
	payload["uptime_seconds"] = static_cast<std::int64_t>(now - started);

	payload["reflector"] = nlohmann::json::object();
	payload["reflector"]["callsign"] = g_Configure.GetString(g_Keys.names.callsign);
	if (g_Configure.Contains(g_Keys.names.country))
		payload["reflector"]["country"] = g_Configure.GetString(g_Keys.names.country);
	if (g_Configure.Contains(g_Keys.names.sponsor))
		payload["reflector"]["sponsor"] = g_Configure.GetString(g_Keys.names.sponsor);
	if (g_Configure.Contains(g_Keys.names.url))
		payload["reflector"]["dashboard_url"] = g_Configure.GetString(g_Keys.names.url);

	// the nodes per module in one pass under the clients lock
	int nodeCount[26] = {};
	auto *clients = g_Reflector.GetClients();
	for (auto cit = clients->cbegin(); cit != clients->cend(); ++cit)
	{
		if (!(*cit)->IsNode() || (*cit)->IsLink())
			continue;
		for (char m = 'A'; m <= 'Z'; m++)
		{
			if ((*cit)->IsLinkedTo(m))
				nodeCount[m - 'A']++;
		}
	}
	g_Reflector.ReleaseClients();

	payload["modules"] = nlohmann::json::array();
	auto modules = g_Configure.GetString(g_Keys.modules.modules);
	for (char module : modules)
	{
		if (module < 'A' || module > 'Z')
			continue;

		nlohmann::json moduleEntry = nlohmann::json::object();
		moduleEntry["name"] = std::string(1, module);
		auto descKey = g_Keys.modules.descriptor[module - 'A'];
		if (g_Configure.Contains(descKey))
			moduleEntry["description"] = g_Configure.GetString(descKey);

		moduleEntry["echo"] = g_Configure.Contains(g_Keys.echo.module) && g_Configure.GetString(g_Keys.echo.module)[0] == module;

		moduleEntry["linked_nodes"] = nodeCount[module - 'A'];
		moduleEntry["mappings"] = g_Reflector.GetModuleMappings(module);

		auto stream = g_Reflector.GetStream(module);
		if (stream && stream->IsOpen())
		{
			moduleEntry["on_air"] = stream->GetUserCallsign().GetCS();
			moduleEntry["on_air_name"] = CUser::OperatorName(stream->GetUserCallsign());
		}
		else
			moduleEntry["on_air"] = nullptr;

		auto tcmods = g_Configure.GetString(g_Keys.tc.modules);
		moduleEntry["transcoded"] = (tcmods.find(module) != std::string::npos);

		int modIdx = module - 'A';
		if (!g_Configure.Contains(g_Keys.dmrplus.enable) || g_Configure.GetBoolean(g_Keys.dmrplus.enable))
			moduleEntry["DMRplus"] = 4001 + modIdx;
		if (!g_Configure.Contains(g_Keys.ysf.enable) || g_Configure.GetBoolean(g_Keys.ysf.enable))
			moduleEntry["YSFDGID"] = 10 + modIdx;
		if (!g_Configure.Contains(g_Keys.nxdn.enable) || g_Configure.GetBoolean(g_Keys.nxdn.enable))
		{
			uint8_t ran = CNXDNProtocol::ModuleToRAN(module);
			if (ran != 0)
				moduleEntry["NXDNRAN"] = static_cast<int>(ran);
			uint16_t dstid = CNXDNProtocol::ModuleToDstId(module);
			if (dstid != 0)
				moduleEntry["NXDNDstId"] = static_cast<int>(dstid);
		}

		payload["modules"].push_back(moduleEntry);
	}

	payload["protocols"] = nlohmann::json::array();

	auto addProtocol = [&](const std::string &name, const auto &enableKey, std::optional<uint32_t> port = std::nullopt)
	{
		const bool enabled = !g_Configure.Contains(enableKey) || g_Configure.GetBoolean(enableKey);
		if (!enabled)
			return;

		nlohmann::json entry = nlohmann::json::object();
		entry["name"] = name;
		if (port.has_value())
			entry["port"] = *port;
		payload["protocols"].push_back(entry);
	};

	addProtocol("DExtra", g_Keys.dextra.enable, g_Configure.Contains(g_Keys.dextra.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.dextra.port)) : std::nullopt);
	addProtocol("DPlus", g_Keys.dplus.enable, g_Configure.Contains(g_Keys.dplus.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.dplus.port)) : std::nullopt);
	addProtocol("DCS", g_Keys.dcs.enable, g_Configure.Contains(g_Keys.dcs.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.dcs.port)) : std::nullopt);
	addProtocol("MMDVM", g_Keys.mmdvm.enable, g_Configure.Contains(g_Keys.mmdvm.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.mmdvm.port)) : std::nullopt);
	addProtocol("DMRPlus", g_Keys.dmrplus.enable, g_Configure.Contains(g_Keys.dmrplus.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.dmrplus.port)) : std::nullopt);
	addProtocol("M17", g_Keys.m17.enable, g_Configure.Contains(g_Keys.m17.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.m17.port)) : std::nullopt);
	addProtocol("YSF", g_Keys.ysf.enable, g_Configure.Contains(g_Keys.ysf.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.ysf.port)) : std::nullopt);
	addProtocol("P25", g_Keys.p25.enable, g_Configure.Contains(g_Keys.p25.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.p25.port)) : std::nullopt);
	addProtocol("NXDN", g_Keys.nxdn.enable, g_Configure.Contains(g_Keys.nxdn.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.nxdn.port)) : std::nullopt);
	addProtocol("URF", g_Keys.urf.enable, g_Configure.Contains(g_Keys.urf.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.urf.port)) : std::nullopt);
	addProtocol("XLXPeer", g_Keys.xlxpeer.enable, g_Configure.Contains(g_Keys.xlxpeer.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.xlxpeer.port)) : std::nullopt);
	addProtocol("USRP", g_Keys.usrp.enable, g_Configure.Contains(g_Keys.usrp.rxport) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.usrp.rxport)) : std::nullopt);
	addProtocol("SvxReflector", g_Keys.svx.enable, g_Configure.Contains(g_Keys.svx.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.svx.port)) : std::nullopt);
	addProtocol("SVX", g_Keys.svxs.enable, g_Configure.Contains(g_Keys.svxs.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.svxs.port)) : std::nullopt);
	addProtocol("HyTalk", g_Keys.hytalk.enable, g_Configure.Contains(g_Keys.hytalk.port) ? std::optional<uint32_t>(g_Configure.GetUnsigned(g_Keys.hytalk.port)) : std::nullopt);
	addProtocol("G3", g_Keys.g3.enable, std::nullopt);
	for (unsigned n = 1; n <= MMDVMCLI_MAX_INSTANCES; n++)
	{
		const auto ck = MmdvmClientKeys(n);
		if (n > 1 && !g_Configure.Contains(ck.enable))
			continue;
		addProtocol(MmdvmClientName(n), ck.enable, g_Configure.Contains(ck.localport) ? std::optional<uint32_t>(g_Configure.GetUnsigned(ck.localport)) : std::nullopt);
	}
	addProtocol("DCSClient", g_Keys.dcsclient.enable, std::nullopt);
	addProtocol("DExtraClient", g_Keys.dextraclient.enable, std::nullopt);
	addProtocol("DPlusClient", g_Keys.dplusclient.enable, std::nullopt);
	addProtocol("YSFClient", g_Keys.ysfclient.enable, std::nullopt);

	return payload;
}

static nlohmann::json BuildReflectorSnapshot()
{
	const auto now = std::time(nullptr);
	const auto started = g_Reflector.GetStartedAt();

	nlohmann::json snapshot = nlohmann::json::object();
	snapshot["server_started"] = started;
	snapshot["current_time"] = now;
	snapshot["uptime_seconds"] = static_cast<std::int64_t>(now - started);
	snapshot["links"] = nlohmann::json::array();
	snapshot["last_heard"] = nlohmann::json::array();
	snapshot["open_channels"] = nlohmann::json::array();
	snapshot["peers"] = nlohmann::json::array();

	// the open channels from the clients that master a stream: the stream's
	// owner pointer is reset outside any lock when it closes
	auto *clients = g_Reflector.GetClients();
	for (const auto &client : *clients)
	{
		// links: the real nodes only, like the XML file; the pseudo clients of
		// connectors and the clients of peers still count for open_channels
		if (client->IsNode() && !client->IsLink())
		{
			nlohmann::json entry;
			entry["callsign"] = client->GetCallsign().GetCS();
			entry["on_module"] = std::string(1, client->GetReflectorModule());
			entry["protocol"] = client->GetProtocolName();
			if (client->GetConnectTime() > 0)
				entry["connect_time"] = client->GetConnectTime();
			snapshot["links"].push_back(entry);
		}

		const char module = client->GetModuleMastered();
		if (module == ' ')
			continue;
		auto stream = g_Reflector.GetStream(module);
		if (!stream || !stream->IsOpen())
			continue;
		snapshot["open_channels"].push_back({
			{ "module", std::string(1, module) },
			{ "stream_id", stream->GetStreamId() },
			{ "user", stream->GetUserCallsign().GetCS() },
			{ "name", CUser::OperatorName(stream->GetUserCallsign()) },
			{ "callsign", client->GetCallsign().GetCS() },
			{ "protocol", client->GetProtocolName() },
			{ "linked_module", std::string(1, client->GetReflectorModule()) } });
	}
	g_Reflector.ReleaseClients();

	auto *peers = g_Reflector.GetPeers();
	for (auto pit = peers->cbegin(); pit != peers->cend(); ++pit)
	{
		nlohmann::json peerClients = nlohmann::json::array();
		for (auto cit = (*pit)->cbegin(); cit != (*pit)->cend(); ++cit)
			peerClients.push_back({
				{ "callsign", (*cit)->GetCallsign().GetCS() },
				{ "protocol", (*cit)->GetProtocolName() },
				{ "reflector_module", std::string(1, (*cit)->GetReflectorModule()) } });
		snapshot["peers"].push_back({
			{ "callsign", (*pit)->GetCallsign().GetCS() },
			{ "protocol", (*pit)->GetProtocolName() },
			{ "connect_time", (*pit)->GetConnectTime() },
			{ "clients", peerClients } });
	}
	g_Reflector.ReleasePeers();

	auto *users = g_Reflector.GetUsers();
	for (const auto &user : *users)
	{
		nlohmann::json entry;
		entry["callsign"] = user.GetCallsign();
		const auto name = user.GetName();
		if (!name.empty())
			entry["name"] = name;
		entry["on_module"] = std::string(1, user.GetOnModule());
		entry["via_peer"] = user.GetViaPeer();
		entry["via_node"] = user.GetViaNode();
		entry["protocol"] = user.GetProtocol();
		entry["last_heard_time"] = user.GetLastHeardTime();
		if (!user.GetLocator().empty())
			entry["locator"] = user.GetLocator();
		snapshot["last_heard"].push_back(entry);
	}
	g_Reflector.ReleaseUsers();

	return snapshot;
}


////////////////////////////////////////////////////////////////////////////////////////
// listener

CWebsocketLwsNotifier::CWebsocketLwsNotifier()
	: m_Ctx(nullptr), m_Running(false), m_MaxClients(64), m_PerClientQueueLen(256)
{
	memset(m_Protocols, 0, sizeof(m_Protocols));
	m_Protocols[0].name = "urfd";
	m_Protocols[0].callback = &CWebsocketLwsNotifier::Callback;
	memset(&m_Retry, 0, sizeof(m_Retry));
}

CWebsocketLwsNotifier::~CWebsocketLwsNotifier()
{
	Stop();
}

bool CWebsocketLwsNotifier::Start(const std::string &bindAddr, uint16_t port, int maxClients, int perClientQueue, int pingIntervalSec, int idleTimeoutSec)
{
	if (m_Running)
		return false;
	m_Bind = bindAddr;
	m_MaxClients = maxClients;
	m_PerClientQueueLen = perClientQueue;

	// lws pings a quiet client and hangs up when no pong came back in time;
	// browsers answer pings on their own
	m_Retry.secs_since_valid_ping = (uint16_t)pingIntervalSec;
	m_Retry.secs_since_valid_hangup = (uint16_t)std::max(idleTimeoutSec, pingIntervalSec + 5);

#ifdef DEBUG
	lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);
#else
	lws_set_log_level(LLL_ERR, nullptr);
#endif

	struct lws_context_creation_info info;
	memset(&info, 0, sizeof(info));
	info.port = port;
	info.iface = m_Bind.c_str();
	info.protocols = m_Protocols;
	info.gid = -1;
	info.uid = -1;
	info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;
	info.retry_and_idle_policy = &m_Retry;
	info.user = this;

	m_Ctx = lws_create_context(&info);
	if (!m_Ctx)
		return false;

	m_Running = true;
	m_Thread = std::thread(&CWebsocketLwsNotifier::ServiceLoop, this);
	return true;
}

void CWebsocketLwsNotifier::Stop(void)
{
	{
		// under the lock: a Publish() that saw m_Running has called
		// lws_cancel_service() before the context goes away
		std::lock_guard<std::mutex> lk(m_Mutex);
		if (!m_Running)
			return;
		m_Running = false;
	}
	lws_cancel_service(m_Ctx);
	if (m_Thread.joinable())
		m_Thread.join();
	// closes the connections; their LWS_CALLBACK_CLOSED runs in this thread
	lws_context_destroy(m_Ctx);
	m_Ctx = nullptr;
	std::lock_guard<std::mutex> lk(m_Mutex);
	m_Clients.clear();
}

void CWebsocketLwsNotifier::ServiceLoop(void)
{
	while (m_Running)
		lws_service(m_Ctx, 0);
}

void CWebsocketLwsNotifier::Publish(const std::string &msg)
{
	std::lock_guard<std::mutex> lk(m_Mutex);
	if (!m_Running || m_Clients.empty())
		return;
	for (auto &[wsi, conn] : m_Clients)
	{
		if (conn.kill)
			continue;
		if ((int)conn.queue.size() >= m_PerClientQueueLen)
		{
			// a client that missed events would show a wrong state: drop it,
			// it reconnects and gets a fresh snapshot
			conn.kill = true;
			conn.queue.clear();
			continue;
		}
		conn.queue.push_back(msg);
	}
	// the only lws call allowed from another thread; under m_Mutex, so Stop()
	// cannot destroy the context in between
	lws_cancel_service(m_Ctx);
}

int CWebsocketLwsNotifier::Callback(struct lws *wsi, enum lws_callback_reasons reason, void *, void *, size_t)
{
	auto *self = static_cast<CWebsocketLwsNotifier *>(lws_context_user(lws_get_context(wsi)));
	if (!self)
		return 0;
	switch (reason)
	{
	case LWS_CALLBACK_ESTABLISHED:
		return self->OnEstablished(wsi);
	case LWS_CALLBACK_CLOSED:
		self->OnClosed(wsi);
		break;
	case LWS_CALLBACK_SERVER_WRITEABLE:
		return self->OnWritable(wsi);
	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		self->OnCancelled();
		break;
	case LWS_CALLBACK_HTTP:
		// plain HTTP is not served here
		return -1;
	default:
		break;
	}
	return 0;
}

int CWebsocketLwsNotifier::OnEstablished(struct lws *wsi)
{
	{
		std::lock_guard<std::mutex> lk(m_Mutex);
		if ((int)m_Clients.size() >= m_MaxClients)
		{
			std::cerr << "WebSocket: " << m_MaxClients << " clients connected, refusing another one" << std::endl;
			return -1;
		}
		// registered first, so no event between the snapshot and now is lost
		m_Clients.emplace(wsi, SConn());
	}

	// built without m_Mutex: it takes the clients and users locks, and the
	// reflector threads call Publish() while they hold those
	std::string first, snapshot;
	try
	{
		const auto dump = [](const CEvent &ev) { return ev.ToJson().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace); };
		first = dump(CEvent("initial_connect", BuildInitialConnectPayload(), CNotifierManager::Instance().ReflectorName()));
		snapshot = dump(CEvent("state_snapshot", BuildReflectorSnapshot(), CNotifierManager::Instance().ReflectorName()));
	}
	catch (const std::exception &ex)
	{
		std::cerr << "WebSocket: cannot build the state for a new client: " << ex.what() << std::endl;
		std::lock_guard<std::mutex> lk(m_Mutex);
		m_Clients.erase(wsi);
		return -1;
	}

	{
		std::lock_guard<std::mutex> lk(m_Mutex);
		auto it = m_Clients.find(wsi);
		if (it == m_Clients.end())
			return -1;
		it->second.queue.push_front(snapshot);
		it->second.queue.push_front(first);
	}
	lws_callback_on_writable(wsi);
	return 0;
}

void CWebsocketLwsNotifier::OnClosed(struct lws *wsi)
{
	std::lock_guard<std::mutex> lk(m_Mutex);
	m_Clients.erase(wsi);
}

int CWebsocketLwsNotifier::OnWritable(struct lws *wsi)
{
	std::string msg;
	bool more;
	{
		std::lock_guard<std::mutex> lk(m_Mutex);
		auto it = m_Clients.find(wsi);
		if (it == m_Clients.end())
			return 0;
		if (it->second.kill)
			return -1;
		if (it->second.queue.empty())
			return 0;
		msg = std::move(it->second.queue.front());
		it->second.queue.pop_front();
		more = !it->second.queue.empty();
	}

	std::vector<unsigned char> buf(LWS_PRE + msg.size());
	memcpy(buf.data() + LWS_PRE, msg.data(), msg.size());
	if (lws_write(wsi, buf.data() + LWS_PRE, msg.size(), LWS_WRITE_TEXT) < (int)msg.size())
		return -1;
	if (more)
		lws_callback_on_writable(wsi);
	return 0;
}

void CWebsocketLwsNotifier::OnCancelled(void)
{
	// service thread, woken by Publish()
	std::lock_guard<std::mutex> lk(m_Mutex);
	for (auto &[wsi, conn] : m_Clients)
	{
		if (conn.kill || !conn.queue.empty())
			lws_callback_on_writable(wsi);
	}
}
