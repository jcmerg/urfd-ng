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


#include <cmath>
#include "Users.h"
#include "Global.h"
#include "Notifier.h"
#include <nlohmann/json.hpp>

////////////////////////////////////////////////////////////////////////////////////////
// constructor

CUsers::CUsers() {}

////////////////////////////////////////////////////////////////////////////////////////
// users management

#define LASTHEARD_USERS_MAX_SIZE 20
void CUsers::AddUser(const CUser &user)
{
	// add
	m_Users.push_front(user);

	// if list size too big, remove oldest
	if ( m_Users.size() >= (LASTHEARD_USERS_MAX_SIZE-1) )
	{
		m_Users.resize(m_Users.size()-1);
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// operation

void CUsers::Hearing(const CCallsign &my, const CCallsign &rpt1, const CCallsign &rpt2, const std::string &proto)
{
	Hearing(my, rpt1, rpt2, g_Reflector.GetCallsign(), proto);
}

// replaces: the callsign this station was shown as so far (StationRenamed)
static void DispatchStationEvent(const std::string &eventType, const CUser &heard, const std::string &replaces = "")
{
	CNotifierManager::Instance().Dispatch(eventType, [&]() -> nlohmann::json {
		nlohmann::json j = {
			{ "callsign", heard.GetCallsign() },
			{ "name", heard.GetName() },
			{ "via_node", heard.GetViaNode() },
			{ "via_peer", heard.GetViaPeer() },
			{ "on_module", std::string(1, heard.GetOnModule()) },
			{ "protocol", heard.GetProtocol() },
			{ "last_heard_time", heard.GetLastHeardTime() },
			{ "locator", heard.GetLocator() } };
		if (!replaces.empty())
			j["replaces"] = replaces;
		return j; });
}

// one stream per module, so one talking station per module
void CUsers::StationStarted(const CCallsign &my, const CCallsign &rpt1, const CCallsign &rpt2, const CCallsign &xlx, const std::string &proto)
{
	CUser heard(my, rpt1, rpt2, xlx, proto);
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
	if (active != m_ActiveStations.end() && active->second.IsCallsign(from))
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
			Hearing(to, old.GetRpt1(), old.GetRpt2(), old.GetXlx(), old.GetProtocol());
			break;
		}
	}
}

void CUsers::Hearing(const CCallsign &my, const CCallsign &rpt1, const CCallsign &rpt2, const CCallsign &xlx, const std::string &proto)
{
	CUser heard(my, rpt1, rpt2, xlx, proto);

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
	g_Reflector.StatusChanged();
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
			user.SetLocation(loc, lat, lon);
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
