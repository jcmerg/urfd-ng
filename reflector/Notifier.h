#pragma once

#include <string>
#include <memory>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include "nlohmann/json.hpp"
#ifdef USE_LIBWEBSOCKETS
#include "websocket_lws_notifier.h"
#endif

class CEvent
{
public:
    std::string event_type;
    std::string reflector_name;
    std::string payload; // raw XML or JSON payload
    std::int64_t timestamp; // seconds since epoch UTC

    CEvent() : timestamp(0) {}

    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        j["event_type"] = event_type;
        j["timestamp"] = timestamp;
        if (!reflector_name.empty()) j["reflector_name"] = reflector_name;

        nlohmann::json payloadJson = nlohmann::json::object();
        if (!payload.empty())
        {
            try
            {
                payloadJson = nlohmann::json::parse(payload);
            }
            catch (const std::exception &)
            {
                payloadJson = nlohmann::json::object();
                payloadJson["data"] = payload;
            }
        }

        // ensure websocket payloads are always objects instead of raw strings
        if (payloadJson.is_string())
        {
            payloadJson = nlohmann::json::object();
            payloadJson["data"] = payload;
        }

        j["payload"] = payloadJson;
        return j;
    }
};

class CNotifierManager
{
public:
    static CNotifierManager &Instance();

    // Dispatch an event to all configured/notified backends. Enqueues and returns quickly.
    void Dispatch(const CEvent &ev);

private:
    CNotifierManager();
    ~CNotifierManager();

    // non-copyable
    CNotifierManager(const CNotifierManager &) = delete;
    CNotifierManager &operator=(const CNotifierManager &) = delete;

    // internal helper to actually perform delivery (currently simple logging placeholder)
    void Deliver(const CEvent &ev);

    // worker loop
    void WorkerThread();

    // queue and synchronization
    std::mutex m_Mutex;
    std::condition_variable m_Cond;
    std::deque<CEvent> m_Queue;
    std::thread m_Worker;
    bool m_Running;

#ifdef USE_LIBWEBSOCKETS
    std::unique_ptr<CWebsocketLwsNotifier> m_WsLwsNotifier;
#endif
};
