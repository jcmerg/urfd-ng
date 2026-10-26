// YSFClientPeer -- Represents the remote YSF reflector as a virtual client

#include "YSFClientPeer.h"
#include "YSFClientProtocol.h"

CYSFClientPeer::CYSFClientPeer()
{
}

CYSFClientPeer::CYSFClientPeer(const CCallsign &callsign, const CIp &ip, char reflectorModule, uint8_t dgid)
	: CClient(callsign, ip, reflectorModule), m_DgId(dgid)
{
}

CYSFClientPeer::CYSFClientPeer(const CYSFClientPeer &client)
	: CClient(client), m_DgId(client.m_DgId)
{
}

bool CYSFClientPeer::IsAlive(void) const
{
	return (m_LastKeepaliveTime.time() < YSFCLI_KEEPALIVE_TIMEOUT);
}
