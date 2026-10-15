#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

class CWebsocketLwsNotifier;

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

	const std::string &ReflectorName(void) const { return m_ReflectorName; }

private:
	CNotifierManager();
	~CNotifierManager();
	CNotifierManager(const CNotifierManager &) = delete;
	CNotifierManager &operator=(const CNotifierManager &) = delete;

	void Send(const CEvent &ev);

	std::atomic<bool> m_Running { false };
	std::string m_ReflectorName;	// set once in Start(), not read from the config later
	std::unique_ptr<CWebsocketLwsNotifier> m_Ws;
};
