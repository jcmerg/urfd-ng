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

#include <ctime>
#include <string.h>
#include <sstream>
#include <iomanip>
#include "User.h"
#include "Utils.h"
#include "Global.h"

CUser::CUser()
{
	m_LastHeardTime = std::time(nullptr);
}

CUser::CUser(const CCallsign &my, const CCallsign &rpt1, const CCallsign &rpt2, const CCallsign &xlx, const std::string &proto)
{
	m_My = my;
	m_Rpt1 = rpt1;
	m_Rpt2 = rpt2;
	m_Xlx = xlx;
	m_Protocol = proto;
	m_LastHeardTime = std::time(nullptr);
}

CUser::CUser(const CUser &user)
{
	m_My = user.m_My;
	m_Rpt1 = user.m_Rpt1;
	m_Rpt2 = user.m_Rpt2;
	m_Xlx = user.m_Xlx;
	m_Protocol = user.m_Protocol;
	m_LastHeardTime = user.m_LastHeardTime;
	SetLocation(user);
}

////////////////////////////////////////////////////////////////////////////////////////
// operators

std::string CUser::Locator(double lat, double lon)
{
	const double la = lat + 90.0;
	const double lo = lon + 180.0;
	std::string loc(6, ' ');
	loc[0] = 'A' + int(lo) / 20;
	loc[1] = 'A' + int(la) / 10;
	loc[2] = '0' + (int(lo) % 20) / 2;
	loc[3] = '0' + int(la) % 10;
	loc[4] = 'a' + int(lo * 12.0) % 24;
	loc[5] = 'a' + int(la * 24.0) % 24;
	return loc;
}

bool CUser::operator ==(const CUser &user) const
{
	return ((user.m_My == m_My) && (user.m_Rpt1 == m_Rpt1) && (user.m_Rpt2 == m_Rpt2)  && (user.m_Xlx == m_Xlx));
}


bool CUser::operator <(const CUser &user) const
{
	// smallest is youngest
	return (std::difftime(m_LastHeardTime, user.m_LastHeardTime) > 0);
}

////////////////////////////////////////////////////////////////////////////////////////
// reporting

std::string CUser::OperatorName(const CCallsign &my)
{
	std::string name;
	g_LDid.Lock();
	uint32_t dmrid = g_LDid.FindDmrid(my.GetKey());
	if (dmrid != 0)
		name = g_LDid.FindName(dmrid);
	g_LDid.Unlock();
	if (name.empty())
	{
		g_LNid.Lock();
		uint16_t nxdnid = g_LNid.FindNXDNid(my.GetKey());
		if (nxdnid != 0)
			name = g_LNid.FindName(nxdnid);
		g_LNid.Unlock();
	}
	return name;
}

// takes the ID database locks, so the caller must not hold the users lock
void CUser::WriteXml(std::ofstream &xmlFile)
{
	xmlFile << "<STATION>" << std::endl;
	xmlFile << "\t<Callsign>" << CUtils::XmlEscape(m_My.GetLogString()) << "</Callsign>" << std::endl;
	xmlFile << "\t<Via node>" << CUtils::XmlEscape(m_Rpt1.GetLogString()) << "</Via node>" << std::endl;
	xmlFile << "\t<On module>" << CUtils::XmlEscape(std::string(1, m_Rpt2.GetCSModule())) << "</On module>" << std::endl;
	xmlFile << "\t<Via peer>" << CUtils::XmlEscape(m_Xlx.GetLogString()) << "</Via peer>" << std::endl;
	if (!m_Protocol.empty())
		xmlFile << "\t<Protocol>" << m_Protocol << "</Protocol>" << std::endl;
	auto name = OperatorName(m_My);
	if (!name.empty())
		xmlFile << "\t<Name>" << CUtils::XmlEscape(name) << "</Name>" << std::endl;
	if (!m_Locator.empty())
	{
		xmlFile << "\t<Locator>" << m_Locator << "</Locator>" << std::endl;
		std::ostringstream pos;
		pos << std::fixed << std::setprecision(5);
		pos << "\t<Latitude>" << m_Latitude << "</Latitude>" << std::endl;
		pos << "\t<Longitude>" << m_Longitude << "</Longitude>" << std::endl;
		xmlFile << pos.str();
	}

	char mbstr[100];
	if (std::strftime(mbstr, sizeof(mbstr), "%FT%TZ", std::gmtime(&m_LastHeardTime)))
	{
		xmlFile << "\t<LastHeardTime>" << mbstr << "</LastHeardTime>" << std::endl;
	}
	xmlFile << "</STATION>" << std::endl;
}

void CUser::JsonReport(nlohmann::json &report)
{
	nlohmann::json juser;
	juser["Callsign"] = m_My.GetCS();
	juser["Repeater"] = m_Rpt1.GetCS();
	juser["OnModule"] = std::string(1, m_Rpt2.GetCSModule());
	juser["ViaPeer"] = m_Xlx.GetCS();
	if (!m_Locator.empty())
	{
		juser["Locator"] = m_Locator;
		juser["Latitude"] = m_Latitude;
		juser["Longitude"] = m_Longitude;
	}
	char s[100];
	if (std::strftime(s, sizeof(s), "%FT%TZ", std::gmtime(&m_LastHeardTime)))
	{
		juser["LastHeard"] = s;
	}
	report["Users"].push_back(juser);
}
