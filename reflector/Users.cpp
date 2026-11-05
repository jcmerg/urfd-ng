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


#include <algorithm>
#include <cmath>
#include "Users.h"
#include "Global.h"
#include "Notifier.h"
#include "LookupDmr.h"
#include "LookupNxdn.h"
#include <nlohmann/json.hpp>

////////////////////////////////////////////////////////////////////////////////////////
// constructor

CUsers::CUsers() {}

////////////////////////////////////////////////////////////////////////////////////////
// users management

void CUsers::AddUser(const CUser &user)
{
	// add
	m_Users.push_front(user);

	// as long as the dashboard shows it ([Dashboard] LastHeard), the oldest goes
	const size_t max = std::max(1u, g_Configure.GetUnsigned(g_Keys.dashboard.lastheard));
	if ( m_Users.size() > max )
	{
		m_Users.resize(max);
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// [Modules] HideUnlisted: on these modules a talker that is in neither ID database
// (DMR, NXDN) stays out of the last heard list and the live stations, so out of
// the dashboard, XML, JSON and WebSocket; e.g. users without a licence on a module
// apart from amateur radio, who have no callsign to look up anyway

bool CUsers::IsHidden(const CCallsign &my, char module)
{
	if (!g_Configure.Contains(ModuleHideUnlistedKey) || std::string::npos == g_Configure.GetString(ModuleHideUnlistedKey).find(module))
		return false;
	// the databases decide, not an ID the radio sent (a group's own IDs are in none)
	const auto key = my.GetKey();
	g_LDid.Lock();
	const bool dmr = g_LDid.FindDmrid(key) != 0 || (my.GetDmrid() && g_LDid.FindCallsign(my.GetDmrid()));
	g_LDid.Unlock();
	if (dmr)
		return false;
	g_LNid.Lock();
	const bool nxdn = g_LNid.FindNXDNid(key) != 0 || (my.GetNXDNid() && g_LNid.FindCallsign(my.GetNXDNid()));
	g_LNid.Unlock();
	return !nxdn;
}

void CUsers::Clear(void)
{
	m_Users.clear();
	// in line with the station events
	CNotifierManager::Instance().DispatchDeferred("last_heard_cleared", []() -> nlohmann::json { return nlohmann::json::object(); });
}

////////////////////////////////////////////////////////////////////////////////////////
// operation

void CUsers::Hearing(const CCallsign &my, const CCallsign &rpt1, const CCallsign &rpt2, const std::string &proto)
{
	Hearing(my, rpt1, rpt2, g_Reflector.GetCallsign(), proto);
}

// replaces: the callsign this station was shown as so far (StationRenamed)
// built later in the notifier's thread: the name lookup takes the ID database
// locks, and the callers hold the users lock
static void DispatchStationEvent(const std::string &eventType, const CUser &heard, const std::string &replaces = "")
{
	CNotifierManager::Instance().DispatchDeferred(eventType, [heard, replaces]() -> nlohmann::json {
		auto j = heard.ToJson();
		if (!replaces.empty())
			j["replaces"] = replaces;
		return j; });
}

// one stream per module, so one talking station per module
void CUsers::StationStarted(const CCallsign &my, const CCallsign &rpt1, const CCallsign &rpt2, const CCallsign &xlx, const std::string &proto)
{
	CUser heard(my, rpt1, rpt2, xlx, proto);
	if (IsHidden(my, heard.GetOnModule()))
		return;
	// the locator is known from an earlier transmission (M17 GNSS, D-Star DPRS, YSF GPS), if at all
	for (const auto &user : m_Users)
	{
		if (user.GetCallsign() == heard.GetCallsign() && !user.GetLocator().empty())
		{
			heard.SetLocation(user);
			break;
		}
	}
	m_ActiveStations[heard.GetOnModule()] = heard;
	DispatchStationEvent("station_started", heard);
}

void CUsers::StationStopped(char module)
{
	auto it = m_ActiveStations.find(module);
	if (it == m_ActiveStations.end())
		return;
	DispatchStationEvent("station_stopped", it->second);
	m_ActiveStations.erase(it);
}

void CUsers::StationRenamed(char module, const CCallsign &from, const CCallsign &to)
{
	auto active = m_ActiveStations.find(module);
	if (active != m_ActiveStations.end() && active->second.IsCallsign(from) && IsHidden(to, module))
	{
		// the new talker is one HideUnlisted keeps off: the station ends here
		DispatchStationEvent("station_stopped", active->second);
		m_ActiveStations.erase(active);
	}
	else if (active != m_ActiveStations.end() && active->second.IsCallsign(from))
	{
		const CUser renamed(to, active->second.GetRpt1(), active->second.GetRpt2(), active->second.GetXlx(), active->second.GetProtocol());
		const std::string was = active->second.GetCallsign();
		DispatchStationEvent("station_stopped", active->second);
		active->second = renamed;
		DispatchStationEvent("station_started", renamed, was);
	}

	// the newest entry of the old callsign on the module, as Hearing() made it
	for (auto it = m_Users.begin(); it != m_Users.end(); it++)
	{
		if (it->IsCallsign(from) && it->GetOnModule() == module)
		{
			const CUser old(*it);
			m_Users.erase(it);
			DispatchStationEvent("user_removed", old);
			Hearing(to, old.GetRpt1(), old.GetRpt2(), old.GetXlx(), old.GetProtocol());
			break;
		}
	}
}

void CUsers::Hearing(const CCallsign &my, const CCallsign &rpt1, const CCallsign &rpt2, const CCallsign &xlx, const std::string &proto)
{
	CUser heard(my, rpt1, rpt2, xlx, proto);
	if (IsHidden(my, heard.GetOnModule()))
		return;

	// a known position stays with the callsign until it reports a new one
	for ( auto &user : m_Users )
	{
		if ( user.IsCallsign(my) )
		{
			heard.SetLocation(user);
			break;
		}
	}

	// first check if this user is already listed. If so, erase him.
	for ( auto it=begin(); it!=end(); it++ )
	{
		if (*it == heard)
		{
			m_Users.erase(it);
			break;
		}
	}

	AddUser(heard);
	// the new head of the last heard list; the dashboard drops an older entry equal to it
	DispatchStationEvent("user_heard", heard);
}

void CUsers::Location(const CCallsign &my, double lat, double lon)
{
	if ( (lat < -90.0) || (lat >= 90.0) || (lon < -180.0) || (lon >= 180.0) )
		return;
	const auto loc = CUser::Locator(lat, lon);
	// the dashboard, XML and JSON are public: publish the middle of the
	// locator's field (about 4.6 x 2.3 km), not where the station is
	lat = (std::floor((lat + 90.0) * 24.0) + 0.5) / 24.0 - 90.0;
	lon = (std::floor((lon + 180.0) * 12.0) + 0.5) / 12.0 - 180.0;
	// the most recent entry of this callsign
	for ( auto &user : m_Users )
	{
		if ( user.IsCallsign(my) )
		{
			// M17 sends its position every second: tell the dashboard when the locator changes
			const bool changed = (user.GetLocator() != loc);
			user.SetLocation(loc, lat, lon);
			if (changed)
				DispatchStationEvent("user_heard", user);
			break;
		}
	}
	// and the station talking now, so its station_stopped carries it
	for ( auto &[module, user] : m_ActiveStations )
	{
		if ( user.IsCallsign(my) )
			user.SetLocation(loc, lat, lon);
	}
}
