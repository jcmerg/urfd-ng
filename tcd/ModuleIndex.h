#pragma once

// ModuleIndex -- a module's slot in tcd's per-module arrays, locks and maps
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <cstdint>

// A-Z, and one slot for a key or packet of no module
constexpr int kModuleSlots = 27;
constexpr int kModuleOther = 26;

// 0-25 for the modules A-Z, kModuleOther for anything else
inline int ModuleSlot(char module) { return (module >= 'A' && module <= 'Z') ? module - 'A' : kModuleOther; }
// of a stream key (CTranscoderPacket::GetStreamKey: the module in bits 16-23)
inline int ModuleSlotOfKey(uint32_t key) { return ModuleSlot(char((key >> 16) & 0xffu)); }
