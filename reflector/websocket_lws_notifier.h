#pragma once

#ifdef USE_LIBWEBSOCKETS

#include <libwebsockets.h>
#include <chrono>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

class CWebsocketLwsNotifier
{
public:
    CWebsocketLwsNotifier();
    ~CWebsocketLwsNotifier();

    bool Start(const std::string &bindAddr, uint16_t port, int maxClients = 64, int perClientQueue = 32,
        int pingIntervalSec = 30, int idleTimeoutSec = 90);
    void Stop();

    // Thread-safe publish: enqueue a message to be sent to all connected clients
    void Publish(const std::string &msg);

private:
    struct PerConn
    {
        std::deque<std::string> queue;
        std::chrono::steady_clock::time_point lastSeen = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point lastPingSent = std::chrono::steady_clock::time_point::min();
    };

    // LWS callbacks
    public:
    static int Callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);

    private:

    void ServiceLoop();
    void OnEstablished(struct lws *wsi);
    void OnClosed(struct lws *wsi);
    void OnReceive(struct lws *wsi, void *in, size_t len);
    void OnWritable(struct lws *wsi);
    void OnTimer(struct lws *wsi);

    std::thread m_Thread;
    std::mutex m_Mutex;
    // per-wsi queue
    std::unordered_map<struct lws *, PerConn> m_Clients;
    std::deque<std::string> m_GlobalQueue; // messages waiting to be enqueued to clients

    struct lws_context *m_Ctx;
    struct lws_protocols m_Protocols[2];
    bool m_Running;
    int m_MaxClients;
    int m_PerClientQueueLen;
    std::chrono::seconds m_PingInterval;
    std::chrono::seconds m_IdleTimeout;
    std::string m_Bind;
    uint16_t m_Port;
};

#endif // USE_LIBWEBSOCKETS
