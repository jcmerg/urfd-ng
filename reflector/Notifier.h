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
	CEvent(const std::string &type, const nlohmann::json &p);

	nlohmann::json ToJson() const;
};

class CNotifierManager
{
public:
	static CNotifierManager &Instance();

	// reads [WebSocket] from the configuration; changes need a restart
	void Start(void);
	void Stop(void);

	// sends the event to every WebSocket client; cheap and a no-op while the
	// listener is off, so it may be called with reflector locks held
	void Dispatch(const CEvent &ev);

private:
	CNotifierManager();
	~CNotifierManager();
	CNotifierManager(const CNotifierManager &) = delete;
	CNotifierManager &operator=(const CNotifierManager &) = delete;

	std::atomic<bool> m_Running { false };
	std::unique_ptr<CWebsocketLwsNotifier> m_Ws;
};
