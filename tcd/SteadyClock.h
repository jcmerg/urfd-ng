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
// the reflector that waited longer than kWorkerMaxAgeMs for it (not a stream's last);
// an answer an AMBE device owes for kLostAnswerMs is taken as lost (the reader looks
// every 50 ms) and its frame goes on with silence; a stream's report to urfd waits
// kReportDelayMs after its last frame, so its last frames are coded by then: a
// device answers in some 20-100 ms, and a frame can pass two device channels
// (decoded on one, encoded on the other), each of which can lose its answer
constexpr int kWorkerMaxAgeMs = 400;
constexpr int kLostAnswerMs = 300;
constexpr int kReportDelayMs = kWorkerMaxAgeMs + 2 * (kLostAnswerMs + 50) + 300;
