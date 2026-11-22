#pragma once

// SteadyClock -- the steady clock as numbers, for stamps kept in atomics
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <chrono>
#include <cstdint>

inline int64_t SteadyMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline int64_t SteadyNs(std::chrono::steady_clock::time_point t) { return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count(); }
