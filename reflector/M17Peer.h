#pragma once

// M17Peer -- An interlinked mrefd reflector (M17-XXX), one M17 client per shared module
//
// Copyright (C) 2026 Jens-Christian Merg (DL4JC)
// Licensed under the GNU General Public License v3 or later

#include "Peer.h"
#include "M17Client.h"

class CM17Peer : public CPeer
{
public:
	// constructors
	CM17Peer(const CCallsign &, const CIp &, const char *, bool legacy);
	CM17Peer(const CM17Peer &) = delete;

	// status
	bool IsAlive(void) const;
	bool IsLegacy(void) const                  { return m_Legacy; }

	// identity
	EProtocol GetProtocol(void) const          { return EProtocol::m17; }
	const char *GetProtocolName(void) const    { return "M17"; }

protected:
	bool m_Legacy;
};
