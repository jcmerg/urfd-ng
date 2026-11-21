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


#include <fstream>
#include "DMRMMDVMClient.h"
#include "Utils.h"


std::atomic<unsigned> CDmrmmdvmClient::s_DynamicTimeout { 60 * DMRMMDVM_DYNAMIC_TIMEOUT };

////////////////////////////////////////////////////////////////////////////////////////
// constructors

CDmrmmdvmClient::CDmrmmdvmClient()
{
}

CDmrmmdvmClient::CDmrmmdvmClient(const CCallsign &callsign, const CIp &ip, char reflectorModule)
	: CClient(callsign, ip, reflectorModule)
{
}

CDmrmmdvmClient::CDmrmmdvmClient(const CDmrmmdvmClient &client)
	: CClient(client)
{
	m_uiRawDmrId = client.m_uiRawDmrId;
	CopySlots(client);
}

////////////////////////////////////////////////////////////////////////////////////////
// status

bool CDmrmmdvmClient::IsAlive(void) const
{
	return (m_LastKeepaliveTime.time() < DMRMMDVM_KEEPALIVE_TIMEOUT);
}

////////////////////////////////////////////////////////////////////////////////////////
// slots

const std::string &CDmrmmdvmClient::GetSlotModules(uint8_t slot) const
{
	static const std::string none;
	return ValidSlot(slot) ? m_Slot[slot-1].modules : none;
}

const std::string &CDmrmmdvmClient::GetSlotStatics(uint8_t slot) const
{
	static const std::string none;
	return ValidSlot(slot) ? m_Slot[slot-1].statics : none;
}

void CDmrmmdvmClient::SetSlotStatics(uint8_t slot, const std::vector<std::pair<char, uint32_t>> &mods)
{
	if (!ValidSlot(slot))
		return;
	auto &s = m_Slot[slot-1];
	for (char m : s.statics)
		if (m != s.dynamic)
			s.tg.erase(m);
	s.statics.clear();
	for (const auto &m : mods)
	{
		s.statics += m.first;
		s.tg[m.first] = m.second;
	}
	// a dynamic module that is static now (on either slot) is not dynamic any more
	for (uint8_t i = 0; i < 2; i++)
	{
		auto &d = m_Slot[i];
		if (d.dynamic != ' ' && s.statics.find(d.dynamic) != std::string::npos)
		{
			if (&d != &s)
				d.tg.erase(d.dynamic);
			d.dynamic = ' ';
			d.dynamicTalking = false;
			Rebuild(i + 1);
		}
	}
	Rebuild(slot);
}

void CDmrmmdvmClient::SetSlotDynamic(uint8_t slot, char mod, uint32_t tg)
{
	if (!ValidSlot(slot))
		return;
	ClearSlotDynamic(slot);
	auto &s = m_Slot[slot-1];
	s.dynamic = mod;
	s.tg[mod] = tg;
	s.dynamicTime.start();
	s.dynamicTalking = false;
	Rebuild(slot);
}

bool CDmrmmdvmClient::IsSlotDynamicExpired(uint8_t slot) const
{
	const unsigned timeout = s_DynamicTimeout;
	return ValidSlot(slot) && m_Slot[slot-1].dynamic != ' ' && timeout != 0 && m_Slot[slot-1].dynamicTime.time() >= timeout;
}

bool CDmrmmdvmClient::SwapSlotDynamicTalking(uint8_t slot, bool on)
{
	if (!ValidSlot(slot))
		return false;
	const bool was = m_Slot[slot-1].dynamicTalking;
	m_Slot[slot-1].dynamicTalking = on;
	return was;
}

int CDmrmmdvmClient::GetSlotDynamicLeft(uint8_t slot) const
{
	const unsigned timeout = s_DynamicTimeout;
	if (!ValidSlot(slot) || m_Slot[slot-1].dynamic == ' ' || timeout == 0)
		return -1;
	const double left = timeout - m_Slot[slot-1].dynamicTime.time();
	return (left > 0.0) ? int(left + 0.5) : 0;
}

void CDmrmmdvmClient::ClearSlotDynamic(uint8_t slot)
{
	if (!ValidSlot(slot))
		return;
	auto &s = m_Slot[slot-1];
	if (s.dynamic == ' ')
		return;
	if (s.statics.find(s.dynamic) == std::string::npos)
		s.tg.erase(s.dynamic);
	s.dynamic = ' ';
	s.dynamicTalking = false;
	Rebuild(slot);
}

uint8_t CDmrmmdvmClient::SlotOfModule(char mod) const
{
	if (mod == ' ')
		return 0;
	for (uint8_t i = 0; i < 2; i++)
		if (m_Slot[i].modules.find(mod) != std::string::npos)
			return i + 1;
	return 0;
}

char CDmrmmdvmClient::GetReflectorModule(void) const
{
	for (const auto &s : m_Slot)
		if (!s.modules.empty())
			return s.modules[0];
	return ' ';
}

void CDmrmmdvmClient::SetSlotTG(uint8_t slot, char mod, uint32_t tg)
{
	if (ValidSlot(slot) && m_Slot[slot-1].modules.find(mod) != std::string::npos)
		m_Slot[slot-1].tg[mod] = tg;
}

uint32_t CDmrmmdvmClient::GetSlotTG(uint8_t slot, char mod) const
{
	if (!ValidSlot(slot))
		return 0;
	auto it = m_Slot[slot-1].tg.find(mod);
	return (it == m_Slot[slot-1].tg.end()) ? 0 : it->second;
}

void CDmrmmdvmClient::Rebuild(uint8_t slot)
{
	auto &s = m_Slot[slot-1];
	s.modules = s.statics;
	if (s.dynamic != ' ' && s.modules.find(s.dynamic) == std::string::npos)
		s.modules += s.dynamic;
}
