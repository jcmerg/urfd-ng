#pragma once

#include "Client.h"

// a HyTalk radio on the module of its selected group
class CHyTalkClient : public CClient
{
public:
	CHyTalkClient();
	CHyTalkClient(const CCallsign &, const CIp &, char = ' ');
	CHyTalkClient(const CHyTalkClient &);
	virtual ~CHyTalkClient() {};

	// identity
	EProtocol GetProtocol(void) const           { return EProtocol::hytalk; }
	const char *GetProtocolName(void) const     { return "HyTalk"; }
	bool IsNode(void) const                     { return true; }

	// status
	bool IsAlive(void) const;
};
