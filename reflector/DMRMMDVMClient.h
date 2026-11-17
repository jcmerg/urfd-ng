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

#pragma once

#include "Defines.h"
#include <map>
#include <string>
#include <vector>

#include "Client.h"

class CDmrmmdvmClient : public CClient
{
public:
	// constructors
	CDmrmmdvmClient();
	CDmrmmdvmClient(const CCallsign &, const CIp &, char = ' ');
	CDmrmmdvmClient(const CDmrmmdvmClient &);

	// destructor
	virtual ~CDmrmmdvmClient() {};

	// identity
	EProtocol GetProtocol(void) const           { return EProtocol::dmrmmdvm; }
	const char *GetProtocolName(void) const     { return "MMDVM"; }
	bool IsNode(void) const                     { return true; }

	// status
	bool IsAlive(void) const;

	// raw DMR ID (base or with extension, as received in RPTL/RPTK)
	void SetRawDmrId(uint32_t id)               { m_uiRawDmrId = id; }
	uint32_t GetRawDmrId(void) const             { return m_uiRawDmrId; }

	// dual-slot module linking (TS1 and TS2 independently): each slot carries the
	// static modules of the hotspot's options (RPTO) and at most one dynamic module,
	// the last one keyed up there; one call at a time per slot (the owner)
	const std::string &GetSlotModules(uint8_t slot) const;	// static ones first, then the dynamic one
	const std::string &GetSlotStatics(uint8_t slot) const;
	char GetSlotDynamic(uint8_t slot) const      { return ValidSlot(slot) ? m_Slot[slot-1].dynamic : ' '; }
	void SetSlotStatics(uint8_t slot, const std::vector<std::pair<char, uint32_t>> &);
	void SetSlotDynamic(uint8_t slot, char mod, uint32_t tg);
	void ClearSlotDynamic(uint8_t slot);
	uint8_t SlotOfModule(char mod) const;	// 0: on no slot
	bool HasReflectorModule(void) const override { return !m_Slot[0].modules.empty() || !m_Slot[1].modules.empty(); }
	char GetReflectorModule(void) const override;
	bool IsLinkedTo(char mod) const override     { return SlotOfModule(mod) != 0; }
	void CopySlots(const CDmrmmdvmClient &client) { m_Slot[0] = client.m_Slot[0]; m_Slot[1] = client.m_Slot[1]; }

	// per-module TG on a slot (the TG it was linked with, for outbound DstId)
	void SetSlotTG(uint8_t slot, char mod, uint32_t tg);
	uint32_t GetSlotTG(uint8_t slot, char mod) const;

	// the module whose call holds a slot, and since when (refreshed by every packet)
	char GetSlotOwner(uint8_t slot) const        { return ValidSlot(slot) ? m_Slot[slot-1].owner : ' '; }
	const CTimer &GetSlotOwnerTime(uint8_t slot) const { return m_Slot[ValidSlot(slot) ? slot-1 : 0].ownerTime; }
	void SetSlotOwner(uint8_t slot, char mod)    { if (ValidSlot(slot)) { m_Slot[slot-1].owner = mod; m_Slot[slot-1].ownerTime.start(); } }

protected:
	static bool ValidSlot(uint8_t slot)          { return slot >= 1 && slot <= 2; }
	void Rebuild(uint8_t slot);

	struct SSlot
	{
		std::string statics;	// from the options, in their order
		char dynamic = ' ';
		std::string modules;	// statics + dynamic
		std::map<char, uint32_t> tg;
		char owner = ' ';
		CTimer ownerTime;
	};
	uint32_t m_uiRawDmrId = 0;
	SSlot m_Slot[2];	// index 0=TS1, index 1=TS2
};
