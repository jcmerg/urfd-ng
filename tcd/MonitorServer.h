#pragma once

// MonitorServer — embedded HTTP + WebSocket + plain TCP stats server
// Uses libwebsockets for HTTP/WS, plus a separate TCP stats socket for tcdmon.
// No authentication — designed for local/trusted networks.

#include <atomic>
#include <future>
#include <string>
#include <unordered_map>
#include <libwebsockets.h>

class CMonitorServer
{
public:
	CMonitorServer();
	~CMonitorServer();

	bool Start(const std::string &bind, uint16_t http_port, uint16_t stats_port);
	void Stop();
	bool IsRunning() const { return m_running.load(); }

	// Called by REST handler to save config to ini file
	bool SaveConfig(const std::string &ini_path);

private:
	void HttpThread();
	void StatsThread();

	// HTTP and WebSocket (libwebsockets), in the service thread
	// one HTTP request or WebSocket client, used by the service thread only
	struct SSession
	{
		bool ws = false;
		std::string uri, body;	// a POST collects its body first
		std::string out;		// the response body still to send
		size_t sent = 0;
		bool responded = false;	// a POST with Content-Length: 0 also gets HTTP_BODY_COMPLETION
	};
	static int Callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);
	int Respond(struct lws *wsi);
	int WriteBody(struct lws *wsi);
	static int HandleRest(const std::string &uri, const std::string &body, std::string &reply);
	struct lws_context *m_Ctx = nullptr;
	struct lws_protocols m_Protocols[2];
	std::unordered_map<struct lws *, SSession> m_Sessions;
	std::string m_Push;					// the stats being sent to the WebSocket clients
	std::atomic<bool> m_PushDue{false};

	// Stats socket (plain TCP, line-delimited JSON)
	void StatsAcceptLoop();
	std::string BuildStatsJson();

	std::atomic<bool> m_running{false};
	std::string m_bind{"127.0.0.1"};
	uint16_t m_httpPort{8080};
	uint16_t m_statsPort{8081};
	std::future<void> m_httpFuture;
	std::future<void> m_statsFuture;
};

// Defined in Main.cpp, used by MonitorServer.cpp static handlers
extern CMonitorServer g_Monitor;
