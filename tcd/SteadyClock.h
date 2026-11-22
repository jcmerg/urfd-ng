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
