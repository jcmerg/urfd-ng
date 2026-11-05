#pragma once

#include <libwebsockets.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <nlohmann/json.hpp>

// libwebsockets listener for the dashboard. lws is single threaded: every lws
// call except lws_cancel_service() happens in the service thread. Publish()
// only queues the message and wakes that thread, which asks for the writes in
// LWS_CALLBACK_EVENT_WAIT_CANCELLED.
//
// A client may send requests, {"id": n, "cmd": "..."}: "snapshot" for the state
// again, and the admin commands of the admin socket after an "auth" with the
// [Admin] Password or a token from an earlier one. These run one after the other
// in their own thread, the snapshot too, so a slow one never holds up the events.
class CWebsocketLwsNotifier
{
public:
	CWebsocketLwsNotifier();
	~CWebsocketLwsNotifier();

	bool Start(const std::string &bindAddr, uint16_t port, int maxClients, int perClientQueue, int pingIntervalSec, int idleTimeoutSec);
	void Stop(void);

	// any thread
	void Publish(const std::string &msg);

	static int Callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);

private:
	struct SConn
	{
		std::deque<std::string> queue;
		bool kill = false;	// queue overflowed, the client is too slow
		uint64_t serial = 0;	// tells a connection from a later one at the same address
		std::string rx;		// a request arriving in fragments
		std::string token;	// the admin login of this connection, empty without one
		std::string addr;	// the browser's address, for the pause after a wrong password
		std::chrono::steady_clock::time_point lastSnapshot {};
	};
	struct SJob
	{
		struct lws *wsi;
		uint64_t serial;
		nlohmann::json request;
	};

	void ServiceLoop(void);
	void AdminLoop(void);
	int  OnEstablished(struct lws *wsi);
	void OnClosed(struct lws *wsi);
	int  OnWritable(struct lws *wsi);
	int  OnReceive(struct lws *wsi, const char *in, size_t len);
	void OnCancelled(void);

	// a request of one client and its answer, which only that client gets
	void Request(struct lws *wsi, uint64_t serial, const nlohmann::json &req);
	nlohmann::json RunAdmin(const SJob &job);
	void Reply(struct lws *wsi, uint64_t serial, const nlohmann::json &id, const nlohmann::json &result);
	void Enqueue(struct lws *wsi, uint64_t serial, std::string msg);

	std::thread m_Thread;
	std::mutex m_Mutex;
	std::unordered_map<struct lws *, SConn> m_Clients;
	uint64_t m_NextSerial = 1;

	// admin requests, for AdminLoop
	std::thread m_AdminThread;
	std::mutex m_JobMutex;
	std::condition_variable m_JobCv;
	std::deque<SJob> m_Jobs;
	bool m_AdminStop = false;
	// the last snapshot built, given to everyone who asks within a second (admin thread only)
	std::string m_SnapCache;
	std::chrono::steady_clock::time_point m_SnapBuilt {};

	struct lws_context *m_Ctx;
	struct lws_protocols m_Protocols[2];
	lws_retry_bo_t m_Retry;
	std::atomic<bool> m_Running;
	int m_MaxClients;
	int m_PerClientQueueLen;
	std::string m_Bind;
};
