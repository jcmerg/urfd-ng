#pragma once

#ifndef NO_WEBSOCKET

#include <libwebsockets.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// libwebsockets listener for the dashboard. lws is single threaded: every lws
// call except lws_cancel_service() happens in the service thread. Publish()
// only queues the message and wakes that thread, which asks for the writes in
// LWS_CALLBACK_EVENT_WAIT_CANCELLED.
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
	};

	void ServiceLoop(void);
	int  OnEstablished(struct lws *wsi);
	void OnClosed(struct lws *wsi);
	int  OnWritable(struct lws *wsi);
	void OnCancelled(void);

	std::thread m_Thread;
	std::mutex m_Mutex;
	std::unordered_map<struct lws *, SConn> m_Clients;

	struct lws_context *m_Ctx;
	struct lws_protocols m_Protocols[2];
	lws_retry_bo_t m_Retry;
	std::atomic<bool> m_Running;
	int m_MaxClients;
	int m_PerClientQueueLen;
	std::string m_Bind;
};

#endif
