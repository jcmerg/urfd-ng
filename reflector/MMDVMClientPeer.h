#pragma once

// MMDVMClientPeer -- Represents the MMDVM master server as a virtual client
// Part of the MMDVMClient protocol extension for urfd

#include "Defines.h"
#include "Client.h"

class CMMDVMClientPeer : public CClient
{
public:
	CMMDVMClientPeer();
	CMMDVMClientPeer(const CCallsign &, const CIp &, char = ' ', const CProtocol *owner = nullptr);
	CMMDVMClientPeer(const CMMDVMClientPeer &);

	virtual ~CMMDVMClientPeer() {};

	// identity
	EProtocol GetProtocol(void) const           { return EProtocol::mmdvmclient; }
	const char *GetProtocolName(void) const     { return "MMDVMClient"; }
	const CProtocol *GetOwnerProtocol(void) const { return m_Owner; }
	bool IsNode(void) const                     { return true; }
	bool IsLink(void) const                     { return true; }

	// status
	bool IsAlive(void) const;

	// reporting
	void WriteXml(std::ofstream &) {}

private:
	const CProtocol *m_Owner = nullptr;	// the connection it belongs to
};
