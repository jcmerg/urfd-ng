// M17Peer -- An interlinked mrefd reflector (M17-XXX), one M17 client per shared module
//
// Copyright (C) 2026 Jens-Christian Merg (DL4JC)
// Licensed under the GNU General Public License v3 or later

#include <string.h>
#include "M17Peer.h"

CM17Peer::CM17Peer(const CCallsign &callsign, const CIp &ip, const char *modules, bool legacy)
	: CPeer(callsign, ip, modules, CVersion()), m_Legacy(legacy)
{
	for ( unsigned i = 0; i < ::strlen(modules); i++ )
		m_Clients.push_back(std::make_shared<CM17Client>(callsign, ip, modules[i], legacy ? EM17ClientType::legacy : EM17ClientType::reflector));
}

bool CM17Peer::IsAlive(void) const
{
	return (m_LastKeepaliveTime.time() < M17_KEEPALIVE_TIMEOUT);
}
