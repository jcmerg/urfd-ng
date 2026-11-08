#pragma once

// OpenBridgePeer -- the other end of the OpenBridge link as the client of a module
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "MMDVMClientPeer.h"

class COpenBridgePeer : public CMMDVMClientPeer
{
public:
	COpenBridgePeer(const CCallsign &cs, const CIp &ip, char module, const CProtocol *owner)
		: CMMDVMClientPeer(cs, ip, module, owner) {}
	COpenBridgePeer(const COpenBridgePeer &client) : CMMDVMClientPeer(client) {}

	EProtocol GetProtocol(void) const override       { return EProtocol::openbridge; }
	const char *GetProtocolName(void) const override { return "OpenBridge"; }
};
