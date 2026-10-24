// MMDVMClientPeer -- Represents the MMDVM master server as a virtual client
// Part of the MMDVMClient protocol extension for urfd

#include "MMDVMClientPeer.h"

// keep alive timeout same as MMDVM
#define MMDVMCLI_KEEPALIVE_TIMEOUT 60

CMMDVMClientPeer::CMMDVMClientPeer()
{
}

CMMDVMClientPeer::CMMDVMClientPeer(const CCallsign &callsign, const CIp &ip, char reflectorModule, const CProtocol *owner)
	: CClient(callsign, ip, reflectorModule), m_Owner(owner)
{
}

CMMDVMClientPeer::CMMDVMClientPeer(const CMMDVMClientPeer &client)
	: CClient(client), m_Owner(client.m_Owner)
{
}

bool CMMDVMClientPeer::IsAlive(void) const
{
	return (m_LastKeepaliveTime.time() < MMDVMCLI_KEEPALIVE_TIMEOUT);
}
