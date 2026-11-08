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

#include <deque>
#include <list>
#include <map>
#include <mutex>

#include "User.h"
#include "TCPacketDef.h"

class CUsers
{
public:
	// constructor
	CUsers();

	// destructor
	virtual ~CUsers() {}

	// locks
	void Lock(void)                     { m_Mutex.lock(); }
	void Unlock(void)                   { m_Mutex.unlock(); }

	// management
	int    GetSize(void) const          { return (int)m_Users.size(); }
	void   AddUser(const CUser &);
	void   Clear(void);	// the active stations end with their streams

	// pass-through
	std::list<CUser>::iterator begin()              { return m_Users.begin(); }
	std::list<CUser>::iterator end()                { return m_Users.end(); }
	std::list<CUser>::const_iterator cbegin()       { return m_Users.cbegin(); }
	std::list<CUser>::const_iterator cend()         { return m_Users.cend(); }

	// operation
	void   Hearing(const CCallsign &, const CCallsign &, const CCallsign &, const std::string &proto = "");
	void   Hearing(const CCallsign &, const CCallsign &, const CCallsign &, const CCallsign &, const std::string &proto = "");
	// WebSocket station_started/station_stopped, from OpenStream and CloseStream
	void   StationStarted(const CCallsign &, const CCallsign &, const CCallsign &, const CCallsign &, const std::string &proto = "", uint16_t streamid = 0);
	// tcd's report of the audio of a stream (STCReport), after it: to the stream's last
	// heard entry, and a user_report to the WebSocket clients
	void   Report(char module, uint16_t streamid, const STCReport &report);
	void   StationStopped(char module, uint32_t lost_frames = 0);	// lost_frames: those the stream had filled in, for Report()
	// the talker on a module turned out to be someone else: the live station
	// and its last heard entry take the new callsign
	void   StationRenamed(char module, const CCallsign &from, const CCallsign &to);
	void   Location(const CCallsign &, double lat, double lon);
	// [Modules] HideUnlisted: a talker on such a module the ID databases do not know is not shown
	static bool IsHidden(const CCallsign &my, char module);

protected:
	// data
	std::mutex                       m_Mutex;
	std::list<CUser>                 m_Users;
	std::map<char, CUser>            m_ActiveStations;	// by module
	// the streams of a module waiting for tcd's report, oldest first: a report comes
	// about 0.3 s after its stream, when the next one may have started (a quick
	// round), and one waits for its stream to stop, which brings the lost frames.
	// A stream stopped more than 10 s ago gets none any more (not transcoded, or
	// too short for tcd to report), so a stream id DMR+ reuses cannot match it
	struct SReportWait { uint16_t streamid; CCallsign talker; std::time_t started, stopped; uint32_t lost_frames; nlohmann::json audio; };
	std::map<char, std::deque<SReportWait>> m_ReportWait;
	void ExpireReportWait(std::deque<SReportWait> &wait);
	void ApplyReport(char module, const SReportWait &wait);
};
