//  Copyright © 2015 Jean-Luc Deltombe (LX3JL). All rights reserved.

// urfd -- The universal reflector
// Copyright © 2021 Thomas A. Early N7TAE
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.


#include "DExtraProtocol.h"
#include "DPlusProtocol.h"
#include "DCSProtocol.h"
#include "URFProtocol.h"
#include "DMRPlusProtocol.h"
#include "DMRMMDVMProtocol.h"
#include "YSFProtocol.h"
#include "M17Protocol.h"
#include "XLXPeerProtocol.h"
#include "MMDVMClientProtocol.h"
#include "MmdvmClientConfig.h"
#include "P25Protocol.h"
#include "NXDNProtocol.h"
#include "USRPProtocol.h"
#include "SvxReflectorProtocol.h"
#include "SvxProtocol.h"
#include "HyTalkProtocol.h"
#include "DCSClientProtocol.h"
#include "DExtraClientProtocol.h"
#include "DPlusClientProtocol.h"
#include "YSFClientProtocol.h"
#include "G3Protocol.h"
#include "Protocols.h"
#include "Global.h"

////////////////////////////////////////////////////////////////////////////////////////
// destructor

CProtocols::~CProtocols()
{
	Close();
}

////////////////////////////////////////////////////////////////////////////////////////
// initialization

// Helper: check if protocol is enabled (defaults to true if key not set)
#define IS_PROTO_ENABLED(key) (!g_Configure.Contains(key) || g_Configure.GetBoolean(key))

bool CProtocols::Init(void)
{
	m_Mutex.lock();
	{
		if (IS_PROTO_ENABLED(g_Keys.dextra.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CDextraProtocol>(new CDextraProtocol));
			if (! m_Protocols.back()->Initialize("XRF", EProtocol::dextra, uint16_t(g_Configure.GetUnsigned(g_Keys.dextra.port)), DSTAR_IPV4, DSTAR_IPV6))
				return false;
		}

		if (IS_PROTO_ENABLED(g_Keys.dplus.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CDplusProtocol>(new CDplusProtocol));
			if (! m_Protocols.back()->Initialize("REF", EProtocol::dplus, uint16_t(g_Configure.GetUnsigned(g_Keys.dplus.port)), DSTAR_IPV4, DSTAR_IPV6))
				return false;
		}

		if (IS_PROTO_ENABLED(g_Keys.dcs.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CDcsProtocol>(new CDcsProtocol));
			if (! m_Protocols.back()->Initialize("DCS", EProtocol::dcs, uint16_t(g_Configure.GetUnsigned(g_Keys.dcs.port)), DSTAR_IPV4, DSTAR_IPV6))
				return false;
		}

		if (IS_PROTO_ENABLED(g_Keys.mmdvm.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CDmrmmdvmProtocol>(new CDmrmmdvmProtocol));
			if (! m_Protocols.back()->Initialize(nullptr, EProtocol::dmrmmdvm, uint16_t(g_Configure.GetUnsigned(g_Keys.mmdvm.port)), DMR_IPV4, DMR_IPV6))
				return false;
		}

		if (g_Configure.Contains(g_Keys.xlxpeer.enable) && g_Configure.GetBoolean(g_Keys.xlxpeer.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CXlxPeerProtocol>(new CXlxPeerProtocol));
			if (! m_Protocols.back()->Initialize("XLX", EProtocol::xlxpeer, uint16_t(g_Configure.GetUnsigned(g_Keys.xlxpeer.port)), DMR_IPV4, DMR_IPV6))
				return false;
		}

		if (IS_PROTO_ENABLED(g_Keys.dmrplus.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CDmrplusProtocol>(new CDmrplusProtocol));
			if (! m_Protocols.back()->Initialize(nullptr, EProtocol::dmrplus, uint16_t(g_Configure.GetUnsigned(g_Keys.dmrplus.port)), DMR_IPV4, DMR_IPV6))
				return false;
		}

		if (IS_PROTO_ENABLED(g_Keys.ysf.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CYsfProtocol>(new CYsfProtocol));
			if (! m_Protocols.back()->Initialize("YSF", EProtocol::ysf, uint16_t(g_Configure.GetUnsigned(g_Keys.ysf.port)), YSF_IPV4, YSF_IPV6))
				return false;
		}

		if (IS_PROTO_ENABLED(g_Keys.m17.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CM17Protocol>(new CM17Protocol));
			if (! m_Protocols.back()->Initialize("URF", EProtocol::m17, uint16_t(g_Configure.GetUnsigned(g_Keys.m17.port)), M17_IPV4, M17_IPV6))
				return false;
		}

		if (IS_PROTO_ENABLED(g_Keys.p25.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CP25Protocol>(new CP25Protocol));
			if (! m_Protocols.back()->Initialize("P25", EProtocol::p25, uint16_t(g_Configure.GetUnsigned(g_Keys.p25.port)), P25_IPV4, P25_IPV6))
				return false;
		}

		if (IS_PROTO_ENABLED(g_Keys.nxdn.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CNXDNProtocol>(new CNXDNProtocol));
			if (! m_Protocols.back()->Initialize("NXDN", EProtocol::nxdn, uint16_t(g_Configure.GetUnsigned(g_Keys.nxdn.port)), NXDN_IPV4, NXDN_IPV6))
				return false;
		}

		if (g_Configure.Contains(g_Keys.usrp.enable) && g_Configure.GetBoolean(g_Keys.usrp.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CUSRPProtocol>(new CUSRPProtocol));
			if (! m_Protocols.back()->Initialize("USRP", EProtocol::usrp, uint16_t(g_Configure.GetUnsigned(g_Keys.usrp.rxport)), USRP_IPV4, USRP_IPV6))
				return false;
		}

		if (g_Configure.Contains(g_Keys.svx.enable) && g_Configure.GetBoolean(g_Keys.svx.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CSvxReflectorProtocol>(new CSvxReflectorProtocol));
			if (! m_Protocols.back()->Initialize(nullptr, EProtocol::svxreflector, uint16_t(0), SVX_IPV4, SVX_IPV6))
				return false;
		}

		if (g_Configure.Contains(g_Keys.svxs.enable) && g_Configure.GetBoolean(g_Keys.svxs.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CSvxProtocol>(new CSvxProtocol));
			if (! m_Protocols.back()->Initialize("SVX", EProtocol::svx, uint16_t(g_Configure.GetUnsigned(g_Keys.svxs.port)), SVX_IPV4, SVX_IPV6))
				return false;
		}

		if (g_Configure.Contains(g_Keys.hytalk.enable) && g_Configure.GetBoolean(g_Keys.hytalk.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CHyTalkProtocol>(new CHyTalkProtocol));
			if (! m_Protocols.back()->Initialize("HYT", EProtocol::hytalk, uint16_t(g_Configure.GetUnsigned(g_Keys.hytalk.port)), HYTALK_IPV4, HYTALK_IPV6))
				return false;
		}

		if (IS_PROTO_ENABLED(g_Keys.urf.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CURFProtocol>(new CURFProtocol));
			if (! m_Protocols.back()->Initialize("URF", EProtocol::urf, uint16_t(g_Configure.GetUnsigned(g_Keys.urf.port)), URF_IPV4, URF_IPV6))
				return false;
		}

		if (g_Configure.Contains(g_Keys.g3.enable) && g_Configure.GetBoolean(g_Keys.g3.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CG3Protocol>(new CG3Protocol));
			if (! m_Protocols.back()->Initialize("XLX", EProtocol::g3, G3_DV_PORT, DMR_IPV4, DMR_IPV6))
			return false;
		}

		for (unsigned n = 1; n <= MMDVMCLI_MAX_INSTANCES; n++)
		{
			const auto ck = MmdvmClientKeys(n);
			if (!g_Configure.Contains(ck.enable) || !g_Configure.GetBoolean(ck.enable))
				continue;
			uint16_t localport = 0;
			if (g_Configure.Contains(ck.localport))
				localport = uint16_t(g_Configure.GetUnsigned(ck.localport));
			auto cli = std::unique_ptr<CMMDVMClientProtocol>(new CMMDVMClientProtocol(n));
			m_Protocols.emplace_back(std::move(cli));
			if (! m_Protocols.back()->Initialize(nullptr, EProtocol::mmdvmclient, localport, DMR_IPV4, false))
				return false;
		}

		if (g_Configure.Contains(g_Keys.dcsclient.enable) && g_Configure.GetBoolean(g_Keys.dcsclient.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CDcsClientProtocol>(new CDcsClientProtocol));
			if (! m_Protocols.back()->Initialize(nullptr, EProtocol::dcsclient, uint16_t(0), DSTAR_IPV4, false))
				return false;
		}

		if (g_Configure.Contains(g_Keys.ysfclient.enable) && g_Configure.GetBoolean(g_Keys.ysfclient.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CYsfClientProtocol>(new CYsfClientProtocol));
			if (! m_Protocols.back()->Initialize(nullptr, EProtocol::ysfclient, uint16_t(0), YSF_IPV4, false))
				return false;
		}

		if (g_Configure.Contains(g_Keys.dextraclient.enable) && g_Configure.GetBoolean(g_Keys.dextraclient.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CDExtraClientProtocol>(new CDExtraClientProtocol));
			if (! m_Protocols.back()->Initialize(nullptr, EProtocol::dextraclient, uint16_t(0), DSTAR_IPV4, false))
				return false;
		}

		if (g_Configure.Contains(g_Keys.dplusclient.enable) && g_Configure.GetBoolean(g_Keys.dplusclient.enable))
		{
			m_Protocols.emplace_back(std::unique_ptr<CDPlusClientProtocol>(new CDPlusClientProtocol));
			if (! m_Protocols.back()->Initialize(nullptr, EProtocol::dplusclient, uint16_t(0), DSTAR_IPV4, false))
				return false;
		}

	}
	for (auto &p : m_Protocols)
		if (p->GetProtocolType() == EProtocol::mmdvmclient)
			m_Mmdvm.push_back(static_cast<CMMDVMClientProtocol *>(p.get()));
	m_Mutex.unlock();

	// done
	return true;
}

CProtocol *CProtocols::FindByType(EProtocol type)
{
	for (auto &p : m_Protocols)
	{
		if (p->GetProtocolType() == type)
			return p.get();
	}
	return nullptr;
}

CMMDVMClientProtocol *CProtocols::FindMmdvmClient(unsigned instance)
{
	for (auto *c : MmdvmClients())
		if (c->GetInstance() == instance)
			return c;
	return nullptr;
}

void CProtocols::Close(void)
{
	// take them out under the lock, but stop them without it: a protocol thread
	// may still be waiting for this lock, and stopping waits for that thread
	std::list<std::unique_ptr<CProtocol>> protocols;
	m_Mutex.lock();
	m_Mmdvm.clear();
	protocols.swap(m_Protocols);
	m_Mutex.unlock();

	// stop each protocol's threads while the object is whole; deleting it first left
	// its thread running on a half destroyed object ("pure virtual method called")
	for (auto &protocol : protocols)
		protocol->Close();
	protocols.clear();
}
