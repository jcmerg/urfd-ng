#include "Notifier.h"
#include <iostream>
#include <ctime>
#include "Global.h" // for g_Configure if needed later

CNotifierManager &CNotifierManager::Instance()
{
    static CNotifierManager instance;
    return instance;
}

CNotifierManager::CNotifierManager()
    : m_Running(true)
{
    // Initialization: read configuration (urfd.ini) and start backends (WebSocket if enabled)
    try {
        bool wsEnabled = false;
        uint16_t wsPort = 8080;
        std::string wsBind = "0.0.0.0";
        int maxClients = 64;
        int perClientQueue = 32;
        int pingIntervalSec = 30;
        int idleTimeoutSec = 90;

        // configuration lookup (prefer [Websocket] section; accept both capitalized and lowercase object keys)
        const auto &cfg = g_Configure.GetData();
        const nlohmann::json *wsCfg = nullptr;
        if (cfg.contains("Websocket") && cfg["Websocket"].is_object())
            wsCfg = &cfg["Websocket"];
        if (cfg.contains("websocket") && cfg["websocket"].is_object())
            wsCfg = &cfg["websocket"];
        if (wsCfg)
        {
            const auto &n = *wsCfg;
            if (n.contains("Enable")) wsEnabled = n["Enable"].get<bool>();
            else if (n.contains("enable")) wsEnabled = n["enable"].get<bool>();
            if (n.contains("Port")) wsPort = static_cast<uint16_t>(n["Port"].get<unsigned>());
            else if (n.contains("port")) wsPort = static_cast<uint16_t>(n["port"].get<unsigned>());
            if (n.contains("Bind")) wsBind = n["Bind"].get<std::string>();
            else if (n.contains("bind")) wsBind = n["bind"].get<std::string>();
            if (n.contains("MaxClients")) maxClients = n["MaxClients"].get<int>();
            else if (n.contains("maxclients")) maxClients = n["maxclients"].get<int>();
            if (n.contains("PerClientQueueLen")) perClientQueue = n["PerClientQueueLen"].get<int>();
            else if (n.contains("perclientqueuelen")) perClientQueue = n["perclientqueuelen"].get<int>();
            if (n.contains("PingIntervalSec")) pingIntervalSec = n["PingIntervalSec"].get<int>();
            else if (n.contains("ping_interval")) pingIntervalSec = n["ping_interval"].get<int>();
            if (n.contains("IdleTimeoutSec")) idleTimeoutSec = n["IdleTimeoutSec"].get<int>();
            else if (n.contains("idle_timeout")) idleTimeoutSec = n["idle_timeout"].get<int>();
        }

#ifdef USE_LIBWEBSOCKETS
        if (wsEnabled)
        {
            try {
                m_WsLwsNotifier = std::make_unique<CWebsocketLwsNotifier>();
                if (!m_WsLwsNotifier->Start(wsBind, wsPort, maxClients, perClientQueue,
                    pingIntervalSec, idleTimeoutSec))
                {
                    std::cerr << "NotifierManager: failed to start LWS WebSocket notifier on " << wsBind << ":" << wsPort << std::endl;
                    m_WsLwsNotifier.reset();
                }
                else
                {
                    std::cout << "NotifierManager: LWS WebSocket notifier listening on " << wsBind << ":" << wsPort << std::endl;
                }
            } catch (const std::exception &ex) {
                std::cerr << "NotifierManager: exception starting LWS WebSocket notifier: " << ex.what() << std::endl;
                m_WsLwsNotifier.reset();
            }
        }
#else
        if (wsEnabled)
        {
            std::cout << "NotifierManager: websocket enabled but build lacks libwebsockets; not starting websocket listener" << std::endl;
        }
#endif
    } catch (...) {
        // configuration read/parse error: continue without websocket
    }

    // start worker thread
    m_Worker = std::thread(&CNotifierManager::WorkerThread, this);
}

CNotifierManager::~CNotifierManager()
{
    // Signal worker to stop and join
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Running = false;
    }
    m_Cond.notify_all();
    if (m_Worker.joinable()) m_Worker.join();

#ifdef USE_LIBWEBSOCKETS
    // stop libwebsockets notifier if running
    if (m_WsLwsNotifier)
    {
        try { m_WsLwsNotifier->Stop(); } catch(...) {}
        m_WsLwsNotifier.reset();
    }
#endif
}

void CNotifierManager::Dispatch(const CEvent &ev)
{
    // Enqueue the event and notify worker. Return quickly.
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Queue.push_back(ev);
    }
    m_Cond.notify_one();
}

void CNotifierManager::WorkerThread()
{
    while (true)
    {
        CEvent ev;
        {
            std::unique_lock<std::mutex> lk(m_Mutex);
            m_Cond.wait(lk, [this]{ return !m_Queue.empty() || !m_Running; });
            if (!m_Running && m_Queue.empty())
                break;
            ev = std::move(m_Queue.front());
            m_Queue.pop_front();
        }

        try
        {
            Deliver(ev);
        }
        catch (const std::exception &ex)
        {
            std::cerr << "NotifierManager: exception in worker Deliver: " << ex.what() << std::endl;
        }
    }
}

void CNotifierManager::Deliver(const CEvent &ev)
{
    // Convert to JSON for logging / future delivery
    auto j = ev.ToJson();
    std::string s = j.dump();

    // Placeholder behavior: write a short log to stdout. Real backends (MQTT/WebSocket)
    // will be implemented behind build flags in future iterations.
    std::cout << "[Notifier] event_type=" << ev.event_type << " timestamp=" << ev.timestamp << " payload_len=" << ev.payload.size() << std::endl;

    // For debugging, print a small preview of the serialized event
    if (s.size() > 0 && s.size() < 4096) {
        std::cout << "[Notifier] payload json: " << s << std::endl;
    } else if (s.size() >= 4096) {
        std::cout << "[Notifier] payload json length: " << s.size() << " (truncated)" << std::endl;
    }

    // Send to WebSocket backend if present (one listener, multiple clients)
#ifdef USE_LIBWEBSOCKETS
    if (m_WsLwsNotifier)
    {
        try {
            m_WsLwsNotifier->Publish(s);
        } catch (const std::exception &ex) {
            std::cerr << "NotifierManager: LWS websocket publish exception: " << ex.what() << std::endl;
        }
    }
#else
    // libwebsockets not available at build time; websockets disabled
#endif

    // Future: iterate configured MQTT endpoints and publish there as well.
}

