#pragma once

// LinkRetry -- how long a client protocol waits before it tries to link again, and
// where it sends that try
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <iostream>
#include <string>

#include "IP.h"

// DCSClient, DExtraClient and DPlusClient: 5 s, then twice as long each time up to
// 60 s, and 5 s again once linked (ircDDBGateway: 1 s doubling to 60 s). A reflector
// that is down or busy with itself is not asked every 5 s for good.
inline unsigned LinkRetrySeconds(unsigned tries)
{
	return (tries >= 4) ? 60U : (5U << tries);
}

// A client protocol looks its reflector's host up again before every try. When that
// fails (DNS down) the last address is kept and tried, and one line says so until
// the host resolves again. False while there is no address at all: nothing to send.
inline bool ResolveLink(CIp &ip, const std::string &host, uint16_t port, bool &unresolved, const char *name)
{
	// (a CIp built from a host name never reports IsSet(), so look at the address)
	CIp now(host.c_str(), AF_INET, SOCK_DGRAM, port);
	if (now.AddressIsZero())
	{
		if (!unresolved)
			std::cerr << name << ": cannot resolve " << host << (ip.AddressIsZero() ? ", not linking until it does" : ", trying its last address") << std::endl;
		unresolved = true;
		return !ip.AddressIsZero();
	}
	if (unresolved)
		std::cout << name << ": " << host << " resolves again" << std::endl;
	unresolved = false;
	ip = now;
	return true;
}
