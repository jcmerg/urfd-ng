#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <nlohmann/json.hpp>

class CWebsocketLwsNotifier;
class CClient;
class CPeer;

// a linked node and a peer as the dashboard gets them, the IP masked as
// [Dashboard] LinksIP / PeersIP say
nlohmann::json NodeJson(const CClient &client);
nlohmann::json PeerJson(const CPeer &peer);

// one event for the WebSocket clients: {"event_type", "timestamp", "reflector_name", "payload"}
class CEvent
{
public:
	std::string event_type;
	std::string reflector_name;
	nlohmann::json payload = nlohmann::json::object();
	std::int64_t timestamp = static_cast<std::int64_t>(std::time(nullptr));

	CEvent() {}
	CEvent(const std::string &type, const nlohmann::json &p, const std::string &name)
		: event_type(type), reflector_name(name), payload(p) {}

	nlohmann::json ToJson() const;
};

class CNotifierManager
{
public:
	static CNotifierManager &Instance();

	// reads [WebSocket] from the configuration; changes need a restart
	void Start(void);
	void Stop(void);

	// sends an event to every WebSocket client; the payload is a callable that
	// returns the JSON, built only while the listener runs, so this is cheap
	// when it is off and may be called with reflector locks held
	template <typename F> void Dispatch(const std::string &type, F &&payload)
	{
		if (m_Running)
			Send(CEvent(type, payload(), m_ReflectorName));
	}

	// the same for a payload that takes the ID database locks (a station's
	// name): built in the notifier's own thread, in the order queued, so the
	// caller may hold the users lock while the ID database reloads. The
	// callable must own what it uses (capture by value).
	template <typename F> void DispatchDeferred(const std::string &type, F &&payload)
	{
		if (m_Running)
			Defer(type, std::function<nlohmann::json()>(std::forward<F>(payload)));
	}

	const std::string &ReflectorName(void) const { return m_ReflectorName; }
	bool IsRunning(void) const { return m_Running; }

private:
	CNotifierManager();
	~CNotifierManager();
	CNotifierManager(const CNotifierManager &) = delete;
	CNotifierManager &operator=(const CNotifierManager &) = delete;

	void Send(const CEvent &ev);
	void Defer(const std::string &type, std::function<nlohmann::json()> payload);
	void DeferLoop(void);

	std::atomic<bool> m_Running { false };

	// the deferred events, for DeferLoop
	std::thread m_DeferThread;
	std::mutex m_DeferMutex;
	std::condition_variable m_DeferCv;
	std::deque<std::pair<std::string, std::function<nlohmann::json()>>> m_Deferred;
	bool m_DeferStop = false;
	bool m_DeferFull = false;	// logged once that events were dropped
	std::string m_ReflectorName;	// set once in Start(), not read from the config later
	std::unique_ptr<CWebsocketLwsNotifier> m_Ws;
};
