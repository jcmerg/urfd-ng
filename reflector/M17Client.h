#pragma once

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

#include "Defines.h"
#include "Client.h"

// simple and listen-only clients are nodes, reflector and legacy are the modules of an
// interlinked mrefd (>= 1.0, or older with the legacy addressing)
enum class EM17ClientType : uint8_t { simple, listenonly, reflector, legacy };

class CM17Client : public CClient
{
public:
	// constructors
	CM17Client();
	CM17Client(const CCallsign &, const CIp &, char, EM17ClientType type = EM17ClientType::simple);
	CM17Client(const CM17Client &);

	// destructor
	virtual ~CM17Client() {};

	// identity
	const char *GetProtocolName(void) const     { return "M17"; }
	bool IsNode(void) const                     { return !IsReflector(); }
	bool IsPeer(void) const                     { return IsReflector(); }
	EProtocol GetProtocol(void) const           { return EProtocol::m17; }

	// status
	bool IsAlive(void) const;
	EM17ClientType GetType(void) const          { return m_Type; }
	bool IsListenOnly(void) const               { return EM17ClientType::listenonly == m_Type; }
	bool IsReflector(void) const                { return EM17ClientType::reflector == m_Type || EM17ClientType::legacy == m_Type; }

protected:
	EM17ClientType m_Type;
};

////////////////////////////////////////////////////////////////////////////////////////
