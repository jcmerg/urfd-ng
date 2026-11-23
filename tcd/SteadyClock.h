#pragma once

// SteadyClock -- the steady clock as numbers, for stamps kept in atomics
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <chrono>
#include <cstdint>
#include <atomic>

inline int64_t SteadyMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline int64_t SteadyNs(std::chrono::steady_clock::time_point t) { return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count(); }

// true at most once a second for a stamp (SteadyMs of the last true): a log
// line that could come with every frame
inline bool OncePerSecond(std::atomic<int64_t> &stamp)
{
	const int64_t now = SteadyMs();
	int64_t last = stamp.load(std::memory_order_relaxed);
	return now - last >= 1000 && stamp.compare_exchange_strong(last, now);
}

// tcd's time limits that depend on each other: a module worker drops a packet from
// the reflector that waited longer than kWorkerMaxAgeMs for it (not a stream's first
// or last frame, not a device's answer); an answer an AMBE device owes for
// kLostAnswerMs (bytes of it dropped on the serial line, an FTDI receive overrun) is
// taken as lost, the reader looks every 50 ms, and its frame goes on with silence;
// a stream's report to urfd waits kReportDelayMs after its last activity (the last
// frame in from urfd, a decode's levels), so its last frames are coded by then: the
// last frame may wait up to kWorkerMaxAgeMs for its worker (it is never dropped, and
// nothing refreshes the activity meanwhile), then its encode on a device may lose its
// answer (kLostAnswerMs and up to 50 ms until the reader looks), and some 300 ms for
// the queues and a device's 20-100 ms
constexpr int kWorkerMaxAgeMs = 400;
constexpr int kLostAnswerMs = 300;
constexpr int kReportDelayMs = kWorkerMaxAgeMs + kLostAnswerMs + 50 + 300;
