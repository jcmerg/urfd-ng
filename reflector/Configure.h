/*
 *   Copyright (c) 2023 by Thomas A. Early N7TAE
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#pragma once

#include <cstdint>
#include <string>
#include <regex>
#include <map>
#include <vector>
#include <utility>
#include <functional>
#include <nlohmann/json.hpp>

enum class ErrorLevel { fatal, mild };
enum class ERefreshType { file, http, both };
enum class ESection { none, names, ip, modules, urf, dplus, dextra, dcs, g3, dmrplus, mmdvm, nxdn, bm, ysf, p25, m17, usrp, dmrid, nxdnid, ysffreq, files, tc, mmdvmclient, echo, svx, svxserver, plugins, extra, admin, dcsclient, ysfclient, dextraclient, dplusclient, callinghome, websocket, credentials };

// the lines of an ini section in the order of the file: key, value
using CIniSection = std::vector<std::pair<std::string, std::string>>;
// the sections ReadData does not know, by name
using CIniSections = std::map<std::string, CIniSection>;

#define IS_TRUE(a) ((a)=='t' || (a)=='T' || (a)=='1')

class CConfigure
{
public:
	CConfigure();
	bool ReadData(const std::string &path);
	bool Contains(const std::string &key) const;
	void Dump(bool justpublic) const;
	std::string GetString(const std::string &key) const;
	unsigned GetUnsigned(const std::string &key) const;
	bool GetBoolean(const std::string &key) const;
	ERefreshType GetRefreshType(const std::string &key) const;
	bool IsString(const std::string &key) const;
	char GetAutolinkModule(const std::string &key) const;
	const nlohmann::json &GetData() { return data; }
	const std::string &GetPath() const { return inipath; }	// the ini last read
	void SetData(const nlohmann::json &d) { data = d; }

	// for the plugins (Plugin.h): ReadData hands them the sections it does not
	// know at its end; true on an error. Not set in inicheck and dbutil.
	static std::function<bool(CConfigure &, const CIniSections &)> PluginHook;
	// a plugin's settings, under keys of its own
	void Set(const std::string &key, const nlohmann::json &value) { data[key] = value; }
	// a plugin's number from min to max, def when it is out of range or no number (with a warning on label)
	static unsigned ParseUnsigned(const std::string &value, const std::string &label, unsigned min, unsigned max, unsigned def);

private:
	// CFGDATA data;
	unsigned counter;
	nlohmann::json data;
	std::string inipath;
	std::regex IPv4RegEx, IPv6RegEx;

	std::string getDataRefreshType(ERefreshType t) const;
	unsigned getUnsigned(const std::string &value, const std::string &label, unsigned min, unsigned max, unsigned defaultvalue) const;
	void badParam(const std::string &param) const;
	bool checkModules(std::string &m) const;
	void checkFile(const std::string &section, const std::string &key, const std::string &filepath) const;
	void setAutolink(const std::string &section, const std::string &key, const std::string &value);
	bool isDefined(ErrorLevel level, const std::string &section, const std::string &pname, const std::string &key, bool &rval);
	void checkAutoLink(const std::string &section, const std::string &pname, const std::string &key, bool &rval) const;
};
