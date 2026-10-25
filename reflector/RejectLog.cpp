// RejectLog -- keeps scanners and misconfigured clients from flooding the log
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "RejectLog.h"

#include <chrono>
#include <iostream>
#include <map>
#include <mutex>

#define REJECT_LOG_WINDOW std::chrono::hours(1)
#define REJECT_LOG_MAX 4096		// a flood from many addresses must not grow the map without end

namespace
{
	struct SEntry
	{
		std::chrono::steady_clock::time_point start;	// when the logged one came
		unsigned more;									// left out since
	};

	std::mutex s_Mutex;
	std::map<std::pair<std::string, std::string>, SEntry> s_Entries;
	std::chrono::steady_clock::time_point s_LastSweep;

	void Summary(const std::pair<std::string, std::string> &key, const SEntry &e)
	{
		if (e.more)
			std::cout << key.first << " from " << key.second << ": " << e.more << " more in the hour after the logged one, not logged" << std::endl;
	}
}

bool RejectLog(const char *kind, const std::string &addr)
{
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(s_Mutex);

	// the hours that are over: their summary, and out of the map
	if (now - s_LastSweep >= std::chrono::minutes(1))
	{
		s_LastSweep = now;
		for (auto it = s_Entries.begin(); it != s_Entries.end(); )
		{
			if (now - it->second.start >= REJECT_LOG_WINDOW)
			{
				Summary(it->first, it->second);
				it = s_Entries.erase(it);
			}
			else
				++it;
		}
	}

	const auto key = std::make_pair(std::string(kind), addr);
	auto it = s_Entries.find(key);
	if (it != s_Entries.end() && now - it->second.start < REJECT_LOG_WINDOW)
	{
		it->second.more++;
		return false;
	}
	if (it != s_Entries.end())
	{
		Summary(it->first, it->second);
		it->second = { now, 0 };
		return true;
	}
	if (s_Entries.size() >= REJECT_LOG_MAX)
		return true;	// full: log it, as without this
	s_Entries.emplace(key, SEntry{ now, 0 });
	return true;
}
