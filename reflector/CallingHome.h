#pragma once

// CallingHome -- registers the reflector in the XLX directory
//
// Sends what the dashboard sent before, so the directory sees the same reflector:
// the same hash ([CallingHome] Hash, made and saved there on the first start),
// name, uptime, URL, country, comment, version and interlinks. Does nothing
// unless [CallingHome] Enable = true.
//
// Copyright (C) 2026 Jens-Christian Merg (DL4JC)
// Licensed under the GNU General Public License v3 or later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>

class CCallingHome
{
public:
	CCallingHome() : m_Run(false) {}
	~CCallingHome() { Stop(); }

	void Start(void);
	void Stop(void);
	// push now, e.g. after the external address changed
	void PushNow(void);

private:
	void Thread(void);
	bool CreateHash(void);	// true on failure
	static bool SaveHash(const std::string &ini, const std::string &hash);	// true on failure
	std::string BuildXml(void) const;
	bool Push(void);

	// read once at start: a SIGHUP reload swaps the configuration under running threads
	std::string m_Name, m_Url, m_Country, m_Comment, m_OverrideIP, m_Server;
	unsigned m_PushDelay = 300;
	long m_Timeout = 5;

	std::string m_Hash;
	std::chrono::steady_clock::time_point m_StartTime;
	bool m_Reached = false;	// last push got through, for logging state changes only

	std::atomic<bool> m_Run;
	bool m_PushNow = false;
	std::mutex m_Mutex;
	std::condition_variable m_Wake;
	std::future<void> m_Future;
};
