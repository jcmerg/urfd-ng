#pragma once

// YSFClientPeer -- Represents the remote YSF reflector as a virtual client

#include "Defines.h"
#include "Client.h"

class CYSFClientPeer : public CClient
{
public:
	CYSFClientPeer();
	CYSFClientPeer(const CCallsign &, const CIp &, char = ' ', uint8_t dgid = 0);
	CYSFClientPeer(const CYSFClientPeer &);

	virtual ~CYSFClientPeer() {};

	EProtocol GetProtocol(void) const           { return EProtocol::ysfclient; }
	const char *GetProtocolName(void) const     { return "YSFClient"; }
	bool IsNode(void) const                     { return true; }
	bool IsLink(void) const                     { return true; }

	bool IsAlive(void) const;
	void WriteXml(std::ofstream &) {}

	// the DG-ID of its mapping (0 = none)
	uint8_t GetDgId(void) const                 { return m_DgId; }

protected:
	uint8_t m_DgId = 0;
};
