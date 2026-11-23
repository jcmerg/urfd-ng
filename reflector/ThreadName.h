#pragma once

// ThreadName -- the calling thread's name in top -H, ps -L and gdb, as tcd
// names its threads (tcd-worker1, tcd-reflector)
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <pthread.h>
#include <cstring>
#include <string>

// Linux takes 15 characters and refuses a longer name (ERANGE) instead of
// cutting it: cut here
inline void SetThreadName(const std::string &name)
{
	char buf[16];
	strncpy(buf, name.c_str(), sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
#if defined(__linux__)
	pthread_setname_np(pthread_self(), buf);
#elif defined(__APPLE__)
	pthread_setname_np(buf);
#endif
}
