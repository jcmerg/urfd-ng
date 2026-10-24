#include "HyTalkClient.h"
#include "HyTalkProtocol.h"

CHyTalkClient::CHyTalkClient()
{
}

CHyTalkClient::CHyTalkClient(const CCallsign &cs, const CIp &ip, char mod)
	: CClient(cs, ip, mod)
{
}

CHyTalkClient::CHyTalkClient(const CHyTalkClient &rhs)
	: CClient(rhs)
{
}

bool CHyTalkClient::IsAlive(void) const
{
	return (m_LastKeepaliveTime.time() < HYTALK_PEER_TIMEOUT);
}
