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

#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <vector>
#include <list>
#include <map>
#include <algorithm>
#include <regex>
#include <stdexcept>
#include <cctype>

#include "Global.h"
#include "CurlGet.h"
#include "MmdvmClientConfig.h"

// ini file keywords
#define JAUTOLINKMODULE          "AutoLinkModule"
#define JBINDINGADDRESS          "BindingAddress"
#define JBLACKLISTPATH           "BlacklistPath"
#define JBOOTSTRAP               "Bootstrap"
#define JXLXPEER                 "XLXPeer"
#define JBRANDMEISTER            "Brandmeister"  // legacy
#define JCALLSIGN                "Callsign"
#define JCOUNTRY                 "Country"
#define JDASHBOARDURL            "DashboardUrl"
#define JDCS                     "DCS"
#define JDEFAULTID               "DefaultId"
#define JDEFAULTRXFREQ           "DefaultRxFreq"
#define JDEFAULTTXFREQ           "DefaultTxFreq"
#define JDESCRIPTION             "Description"
#define JDEXTRA                  "DExtra"
#define JDMRIDDB                 "DMR ID DB"
#define JDMRPLUS                 "DMRPlus"
#define JDPLUS                   "DPlus"
#define JENABLE                  "Enable"
#define JFILES                   "Files"
#define JFILEPATH                "FilePath"
#define JG3                      "G3"
#define JG3TERMINALPATH          "G3TerminalPath"
#define JINTERLINKPATH           "InterlinkPath"
#define JIPADDRESS               "IPAddress"
#define JIPADDRESSES             "IP Addresses"
#define JIPV4BINDING             "IPv4Binding"
#define JIPV4EXTERNAL            "IPv4External"
#define JIPV6BINDING             "IPv6Binding"
#define JIPV6EXTERNAL            "IPv6External"
#define JJSONPATH                "JsonPath"
#define JM17                     "M17"
#define JMMDVM                   "MMDVM"
#define JMODE                    "Mode"
#define JMODULE                  "Module"
#define JMODULES                 "Modules"
#define JNAMES                   "Names"
#define JNXDNIDDB                "NXDN ID DB"
#define JNXDN                    "NXDN"
#define JP25                     "P25"
#define JPIDPATH                 "PidPath"
#define JPORT                    "Port"
#define JREFLECTORID             "ReflectorID"
#define JREFRESHMIN              "RefreshMin"
#define JREGISTRATIONDESCRIPTION "RegistrationDescription"
#define JREGISTRATIONID          "RegistrationID"
#define JREGISTRATIONNAME        "RegistrationName"
#define JRXPORT                  "RxPort"
#define JSPONSOR                 "Sponsor"
#define JSYSOPEMAIL              "SysopEmail"
#define JTRANSCODER              "Transcoder"
#define JCALLINGHOME             "CallingHome"
#define JTXPORT                  "TxPort"
#define JURF                     "URF"
#define JENABLEDGID              "EnableDGID"
#define JURL                     "URL"
#define JUSRP                    "USRP"
#define JWEBSOCKET               "WebSocket"
#define JWHITELISTPATH           "WhitelistPath"
#define JXMLPATH                 "XmlPath"
#define JYSF                     "YSF"
#define JYSFTXRXDB               "YSF TX/RX DB"
#define JMMDVMCLIENT             "MMDVMClient"
#define JSVXREFLECTOR            "SvxReflector"
#define JSVXSERVER               "SVXServer"
#define JPLUGINS                 "Plugins"
#define JDCSCLIENT               "DCSClient"

static inline void split(const std::string &s, char delim, std::vector<std::string> &v)
{
	std::istringstream iss(s);
	std::string item;
	while (std::getline(iss, item, delim))
		v.push_back(item);
}

// trim from start (in place)
static inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
        return !std::isspace(ch);
    }));
}

// trim from end (in place)
static inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

// trim from both ends (in place)
static inline void trim(std::string &s) {
    ltrim(s);
    rtrim(s);
}

// the credential backends this build has, see the Makefile and CredentialStore.h
static bool CredBackendBuilt(const std::string &backend)
{
#ifdef CRED_SQLITE
	if (backend == "sqlite")
		return true;
#endif
#ifdef CRED_MYSQL
	if (backend == "mysql")
		return true;
#endif
#ifdef CRED_PGSQL
	if (backend == "postgresql")
		return true;
#endif
	(void)backend;	// no backend built in
	return false;
}

std::function<bool(CConfigure &, const CIniSections &)> CConfigure::PluginHook;

CConfigure::CConfigure()
{
	IPv4RegEx = std::regex("^((25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])\\.){3,3}(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9]){1,1}$", std::regex::extended);
	IPv6RegEx = std::regex("^(([0-9a-fA-F]{1,4}:){7,7}[0-9a-fA-F]{1,4}|([0-9a-fA-F]{1,4}:){1,7}:|([0-9a-fA-F]{1,4}:){1,6}(:[0-9a-fA-F]{1,4}){1,1}|([0-9a-fA-F]{1,4}:){1,5}(:[0-9a-fA-F]{1,4}){1,2}|([0-9a-fA-F]{1,4}:){1,4}(:[0-9a-fA-F]{1,4}){1,3}|([0-9a-fA-F]{1,4}:){1,3}(:[0-9a-fA-F]{1,4}){1,4}|([0-9a-fA-F]{1,4}:){1,2}(:[0-9a-fA-F]{1,4}){1,5}|([0-9a-fA-F]{1,4}:){1,1}(:[0-9a-fA-F]{1,4}){1,6}|:((:[0-9a-fA-F]{1,4}){1,7}|:))$", std::regex::extended);
}

bool CConfigure::ReadData(const std::string &path)
// returns true on failure
{
	bool rval = false;
	ESection section = ESection::none;
	counter = 0;
	SJsonKeys::DB *pdb;
	unsigned tcport = 0;
	unsigned cliInstance = 1;	// [MMDVMClient<n>] being read
	CIniSections extra;			// the sections urfd does not know, for the plugins
	std::string extraName;

	data.clear();
	inipath = path;
	std::ifstream cfgfile(path.c_str(), std::ifstream::in);
	if (! cfgfile.is_open()) {
		std::cerr << "ERROR: '" << path << "' was not found!" << std::endl;
		return true;
	}

	std::string ipv4, ipv6;

	{
		CCurlGet curl;
		std::stringstream ss;
		if (CURLE_OK == curl.GetURL("https://ipv4.icanhazip.com", ss))
		{
			ipv4.assign(ss.str());
			trim(ipv4);
		}
		ss.str(std::string());
		if (CURLE_OK == curl.GetURL("https://ipv6.icanhazip.com", ss))
		{
			ipv6.assign(ss.str());
			trim(ipv6);
		}
	}

	std::string line;
	while (std::getline(cfgfile, line))
	{
		counter++;
		trim(line);
		if (3 > line.size())
			continue;	// can't be anything
		if ('#' == line.at(0) || ';' == line.at(0))
			continue;	// skip comments

		// check for next section
		if ('[' == line.at(0))
		{
			std::string hname(line.substr(1));
			auto pos = hname.find(']');
			if (std::string::npos != pos)
				hname.resize(pos);
			section = ESection::none;
			if (0 == hname.compare(JNAMES))
				section = ESection::names;
			else if (0 == hname.compare(JIPADDRESSES))
				section = ESection::ip;
			else if (0 == hname.compare(JTRANSCODER))
				section = ESection::tc;
			else if (0 == hname.compare(JCALLINGHOME))
				section = ESection::callinghome;
			else if (0 == hname.compare(JMODULES))
				section = ESection::modules;
			else if (0 == hname.compare(JDPLUS))
				section = ESection::dplus;
			else if (0 == hname.compare(JDEXTRA))
				section = ESection::dextra;
			else if (0 == hname.compare(JG3))
				section = ESection::g3;
			else if (0 == hname.compare(JDMRPLUS))
				section = ESection::dmrplus;
			else if (0 == hname.compare(JMMDVM))
				section = ESection::mmdvm;
			else if (0 == hname.compare(JNXDN))
				section = ESection::nxdn;
			else if (0 == hname.compare(JXLXPEER) || 0 == hname.compare(JBRANDMEISTER))
				section = ESection::bm;
			else if (0 == hname.compare(JYSF))
				section = ESection::ysf;
			else if (0 == hname.compare(JDCS))
				section = ESection::dcs;
			else if (0 == hname.compare(JP25))
				section = ESection::p25;
			else if (0 == hname.compare(JM17))
				section = ESection::m17;
			else if (0 == hname.compare(JUSRP))
				section = ESection::usrp;
			else if (0 == hname.compare(JURF))
				section = ESection::urf;
			else if (0 == hname.compare(JDMRIDDB))
				section = ESection::dmrid;
			else if (0 == hname.compare(JNXDNIDDB))
				section = ESection::nxdnid;
			else if (0 == hname.compare(JYSFTXRXDB))
				section = ESection::ysffreq;
			else if (MmdvmClientInstance(hname))
			{
				section = ESection::mmdvmclient;
				cliInstance = MmdvmClientInstance(hname);
			}
			else if (0 == hname.compare("Echo"))
				section = ESection::echo;
			else if (0 == hname.compare(JSVXREFLECTOR))
				section = ESection::svx;
			else if (0 == hname.compare(JSVXSERVER))
				section = ESection::svxserver;
			else if (0 == hname.compare(JPLUGINS))
				section = ESection::plugins;
			else if (0 == hname.compare(JFILES))
				section = ESection::files;
			else if (0 == hname.compare(JWEBSOCKET))
				section = ESection::websocket;
			else if (0 == hname.compare(JDCSCLIENT))
				section = ESection::dcsclient;
			else if (0 == hname.compare("YSFClient"))
				section = ESection::ysfclient;
			else if (0 == hname.compare("DExtraClient"))
				section = ESection::dextraclient;
			else if (0 == hname.compare("DPlusClient"))
				section = ESection::dplusclient;
			else if (0 == hname.compare("Admin"))
				section = ESection::admin;
			else if (0 == hname.compare("Credentials"))
				section = ESection::credentials;
			else if (!hname.empty())
			{
				// perhaps a plugin's, see the end
				section = ESection::extra;
				extraName = hname;
				extra[hname];
			}
			else
			{
				std::cerr << "WARNING: unknown ini file section: " << line << std::endl;
			}
			continue;
		}

		// split on first '=' only
		auto eqpos = line.find('=');
		if (eqpos == std::string::npos)
		{
			std::cout << "WARNING: line #" << counter << ": '" << line << "' does not contain an equal sign, skipping" << std::endl;
			continue;
		}
		std::vector<std::string> tokens;
		tokens.push_back(line.substr(0, eqpos));
		tokens.push_back(eqpos + 1 < line.size() ? line.substr(eqpos + 1) : "");
		auto pos = tokens[1].find('#');
		if (std::string::npos != pos)
		{
			tokens[1].assign(tokens[1].substr(0, pos));
			rtrim(tokens[1]); // whitespace between the value and the end-of-line comment
		}
		// trim whitespace from around the '='
		rtrim(tokens[0]);
		ltrim(tokens[1]);
		const std::string key(tokens[0]);
		const std::string value(tokens[1]);
		if (key.empty())
		{
			std::cout << "WARNING: line #" << counter << ": missing key: '" << line << "'" << std::endl;
			continue;
		}
		switch (section)
		{
			case ESection::names:
				if (0 == key.compare(JCALLSIGN))
					data[g_Keys.names.callsign] = value;
				else if (0 == key.compare(JBOOTSTRAP))
					data[g_Keys.names.bootstrap] = value;
				else if (0 == key.compare(JDASHBOARDURL))
					data[g_Keys.names.url] = value;
				else if (0 == key.compare(JSYSOPEMAIL))
					data[g_Keys.names.email] = value;
				else if (0 == key.compare(JCOUNTRY))
					data[g_Keys.names.country] = value.substr(0,2);
				else if (0 == key.compare(JSPONSOR))
					data[g_Keys.names.sponsor] = value;
				else
					badParam(key);
				break;
			case ESection::ip:
				if (0 == key.compare(JIPV4BINDING))
				{
					data[g_Keys.ip.ipv4bind] = value;
				}
				else if (0 == key.compare(JIPV6BINDING))
				{
					data[g_Keys.ip.ipv6bind] = value;
				}
				else if (0 == key.compare(JIPV4EXTERNAL))
				{
					data[g_Keys.ip.ipv4address] = value;
				}
				else if (0 == key.compare(JIPV6EXTERNAL))
				{
					data[g_Keys.ip.ipv6address] = value;
				}
				else
					badParam(key);
				break;
			case ESection::callinghome:
				if (0 == key.compare(JENABLE))
					data[g_Keys.callinghome.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare("Country"))
					data[g_Keys.callinghome.country] = value;
				else if (0 == key.compare("Comment"))
				{
					// the directory takes 100 characters; cut between UTF-8 sequences
					size_t pos = 0;
					for (int chars = 0; pos < value.size() && chars < 100; chars++)
						do pos++; while (pos < value.size() && 0x80 == (0xC0 & (unsigned char)value[pos]));
					data[g_Keys.callinghome.comment] = value.substr(0, pos);
				}
				else if (0 == key.compare("Hash"))
				{
					if (!value.empty())	// blank = make one at startup
						data[g_Keys.callinghome.hash] = value;
				}
				else if (0 == key.compare("OverrideIP"))
					data[g_Keys.callinghome.overrideip] = value;
				else if (0 == key.compare("Server"))
					data[g_Keys.callinghome.server] = value;
				else if (0 == key.compare("PushDelay"))
					data[g_Keys.callinghome.pushdelay] = getUnsigned(value, "CallingHome PushDelay", 60, 86400, 300);
				else if (0 == key.compare("Timeout"))
					data[g_Keys.callinghome.timeout] = getUnsigned(value, "CallingHome Timeout", 1, 60, 5);
				else
					badParam(key);
				break;
			case ESection::tc:
				if (0 == key.compare(JPORT))
					data[g_Keys.tc.port] = getUnsigned(value, "Transcoder Port", 0, 40000, 10100);
				else if (0 == key.compare(JBINDINGADDRESS))
					data[g_Keys.tc.bind] = value;
				else if (0 == key.compare(JMODULES))
				{
					std::string m(value);
					std::string lower(value);
					std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
					if (lower == "auto")
					{
						// tcd's Modules decide: any module of [Modules], transcoded while tcd is connected for it
						data[g_Keys.tc.automatic] = true;
						data[g_Keys.tc.modules] = "";
					}
					else
					{
						if (checkModules(m))
						{
							std::cerr << "ERROR: line #" << counter << ": no letters found in transcoder Modules: '" << m << "'" << std::endl;
							rval = true;
						}
						data[g_Keys.tc.modules] = m;
					}
				}
				else if (0 == key.compare("Password"))
					data[g_Keys.tc.password] = value;
				else
					badParam(key);
				break;
			case ESection::modules:
				if (0 == key.compare(JMODULES))
				{
					std::string m(value);
					if (checkModules(m))
					{
						std::cerr << "ERROR: line #" << counter <<  ": no letters found in Modules: '" << m << "'" << std::endl;
						rval = true;
					} else
						data[g_Keys.modules.modules] = m;
				}
				else if (0 == key.compare(0, 11, "Description"))
				{
					if (12 == key.size() && isupper(key[11]))
						data[key] = value;
					else
						badParam(key);
				}
				else
					badParam(key);
				break;
			case ESection::bm:
				if (0 == key.compare(JPORT))
					data[g_Keys.xlxpeer.port] = getUnsigned(value, "XLXPeer Port", 1024, 65535, 10002);
				else if (0 == key.compare(JENABLE))
					data[g_Keys.xlxpeer.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare(JCALLSIGN))
					data[g_Keys.xlxpeer.peercallsign] = value;
				else
					badParam(key);
				break;
			case ESection::dcs:
				if (0 == key.compare(JPORT))
					data[g_Keys.dcs.port] = getUnsigned(value, "DCS Port", 1024, 65535, 30051);
				else if (0 == key.compare(JENABLE))
					data[g_Keys.dcs.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare(JCALLSIGN))
					data[g_Keys.dcs.peercallsign] = value;
				else
					badParam(key);
				break;
			case ESection::dextra:
				if (0 == key.compare(JPORT))
					data[g_Keys.dextra.port] = getUnsigned(value, "DExtra Port", 1024, 65535, 30001);
				else if (0 == key.compare(JENABLE))
					data[g_Keys.dextra.enable] = IS_TRUE(value[0]);
				else
					badParam(key);
				break;
			case ESection::g3:
				if (0 == key.compare(JENABLE))
					data[g_Keys.g3.enable] = IS_TRUE(value[0]);
				else
					badParam(key);
				break;
			case ESection::dmrplus:
				if (0 == key.compare(JPORT))
					data[g_Keys.dmrplus.port] = getUnsigned(value, "DMRPlus Port", 1024, 65535, 8880);
				else if (0 == key.compare(JENABLE))
					data[g_Keys.dmrplus.enable] = IS_TRUE(value[0]);
				else
					badParam(key);
				break;
			case ESection::dplus:
				if (0 == key.compare(JPORT))
					data[g_Keys.dplus.port] = getUnsigned(value, "DPlus Port", 1024, 65535, 20001);
				else if (0 == key.compare(JENABLE))
					data[g_Keys.dplus.enable] = IS_TRUE(value[0]);
				else
					badParam(key);
				break;
			case ESection::m17:
				if (0 == key.compare(JPORT))
					data[g_Keys.m17.port] = getUnsigned(value, "M17 Port", 1024, 65535, 17000);
				else if (0 == key.compare(JENABLE))
					data[g_Keys.m17.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare(JCALLSIGN))
				{
					// M17 designator used to interlink with mrefd reflectors
					std::string cs(value);
					for (auto &c : cs) c = toupper(c);
					if (7 == cs.size() && 0 == cs.compare(0, 4, "M17-"))
						data[g_Keys.m17.callsign] = cs;
					else
						std::cerr << "ERROR: [M17] Callsign '" << value << "' is not an M17 reflector designator like M17-XXX, M17 interlinking stays off" << std::endl;
				}
				else
					badParam(key);
				break;
			case ESection::mmdvm:
				if (0 == key.compare(JPORT))
					data[g_Keys.mmdvm.port] = getUnsigned(value, "MMDVM Port", 1024, 65535, 62030);
				else if (0 == key.compare(JDEFAULTID) || 0 == key.compare("FallbackDmrId"))
					data[g_Keys.mmdvm.fallbackdmrid] = getUnsigned(value, "MMDVM FallbackDmrId", 0, 999999999, 0);
				else if (0 == key.compare(JENABLE))
					data[g_Keys.mmdvm.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare(0, 2, "TG"))
					data["mmdvmTG" + key.substr(2)] = value;  // TG mapping: TG<N> = <Module>[,TS<1|2>]
				else if (0 == key.compare("BlockProtocols"))
					data["mmdvmBlockProtocols"] = value;
				else if (0 == key.compare("RequireAuth"))
					data[g_Keys.mmdvm.requireauth] = IS_TRUE(value[0]);
				else
				{
					// check if it's a user entry (DMR ID or callsign = password)
					bool isUser = false;
					if (!key.empty())
					{
						// numeric keys are DMR IDs, alpha keys are callsigns
						bool isNumeric = std::all_of(key.begin(), key.end(), ::isdigit);
						bool isCallsign = !isNumeric && key.size() >= 3 && std::isalpha(key[0]);
						if (isNumeric || isCallsign)
						{
							data["mmdvmUsr_" + key] = value;
							isUser = true;
						}
					}
					if (!isUser)
						badParam(key);
				}
				break;
			case ESection::nxdn:
				if (0 == key.compare(JPORT))
					data[g_Keys.nxdn.port] = getUnsigned(value, "NDXN Port", 1024, 65535, 41400);
				else if (0 == key.compare(JAUTOLINKMODULE))
					setAutolink(JNXDN, g_Keys.nxdn.autolinkmod, value);
				else if (0 == key.compare(JREFLECTORID))
					data[g_Keys.nxdn.reflectorid] = getUnsigned(value, "NXDN ReflectorID", 0, 65535, 0);
				else if (0 == key.compare(JENABLE))
					data[g_Keys.nxdn.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare("FallbackNxdnId"))
					data[g_Keys.nxdn.fallbacknxdnid] = getUnsigned(value, "NXDN FallbackNxdnId", 0, 65535, 0);
				else if (0 == key.compare("ModuleDstIdBase"))
					data[g_Keys.nxdn.moduledstidbase] = getUnsigned(value, "NXDN ModuleDstIdBase", 0, 65509, 0);
				else
					badParam(key);
				break;
			case ESection::p25:
				if (0 == key.compare(JPORT))
					data[g_Keys.p25.port] = getUnsigned(value, "P25 Port", 1024, 65535, 41000);
				else if (0 == key.compare(JAUTOLINKMODULE))
					setAutolink(JP25, g_Keys.p25.autolinkmod, value);
				else if (0 == key.compare(JREFLECTORID))
					data[g_Keys.p25.reflectorid] = getUnsigned(value, "P25 ReflectorID", 0, 16777215, 0);
				else if (0 == key.compare(JENABLE))
					data[g_Keys.p25.enable] = IS_TRUE(value[0]);
				else
					badParam(key);
				break;
			case ESection::urf:
				if (0 == key.compare(JPORT))
					data[g_Keys.urf.port] = getUnsigned(value, "URF Port", 1024, 65535, 10017);
				else if (0 == key.compare(JENABLE))
					data[g_Keys.urf.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare("InterlinkVersion"))
					data[g_Keys.urf.interlinkversion] = getUnsigned(value, "URF InterlinkVersion", 3, 4, 4);
				else
					badParam(key);
				break;
			case ESection::usrp:
				if (0 == key.compare(JENABLE))
					data[g_Keys.usrp.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare(JIPADDRESS))
					data[g_Keys.usrp.ip] = value;
				else if (0 == key.compare(JTXPORT))
					data[g_Keys.usrp.txport] = getUnsigned(value, "USRP TxPort", 1024, 65535, 32000);
				else if (0 == key.compare(JRXPORT))
					data[g_Keys.usrp.rxport] = getUnsigned(value, "USRP RxPort", 1024, 65535, 34000);
				else if (0 == key.compare(JMODULE))
					data[g_Keys.usrp.module] = value.substr(0, 1);
				else if (0 == key.compare(JCALLSIGN))
					data[g_Keys.usrp.callsign] = value;
				else if (0 == key.compare(JFILEPATH))
					data[g_Keys.usrp.filepath] = value;
				else if (0 == key.compare("RxGain"))
					data[g_Keys.usrp.rxgain] = value;
				else if (0 == key.compare("TxGain"))
					data[g_Keys.usrp.txgain] = value;
				else
					badParam(key);
				break;
			case ESection::ysf:
				if (0 == key.compare(JPORT))
					data[g_Keys.ysf.port] = getUnsigned(value, "YSF Port", 1024, 65535, 42000);
				else if (0 == key.compare(JAUTOLINKMODULE))
					setAutolink(JYSF, g_Keys.ysf.autolinkmod, value);
				else if (0 == key.compare(JENABLEDGID))
					data[g_Keys.ysf.enabledgid] = IS_TRUE(value[0]);
				else if (0 == key.compare(JDEFAULTTXFREQ))
					data[g_Keys.ysf.defaulttxfreq] = getUnsigned(value, "YSF DefaultTxFreq", 40000000, 2600000000, 439000000);
				else if (0 == key.compare(JDEFAULTRXFREQ))
					data[g_Keys.ysf.defaultrxfreq] = getUnsigned(value, "YSF DefaultRxFreq", 40000000, 2600000000, 439000000);
				else if (0 == key.compare(JREGISTRATIONID))
					data[g_Keys.ysf.ysfreflectordb.id] = getUnsigned(value, "YSF RegistrationID", 0, 9999999, 0);
				else if (0 == key.compare(JREGISTRATIONNAME))
				{
					std::string name(value);
					if (name.size() > 13) name.resize(13);
					data[g_Keys.ysf.ysfreflectordb.name] = name;
				}
				else if (0 == key.compare(JREGISTRATIONDESCRIPTION))
				{
					std::string desc(value);
					if (desc.size() > 16) desc.resize(16);
					data[g_Keys.ysf.ysfreflectordb.description] = desc;
				}
				else if (0 == key.compare(JENABLE))
					data[g_Keys.ysf.enable] = IS_TRUE(value[0]);
				else
					badParam(key);
				break;
			case ESection::dmrid:
			case ESection::nxdnid:
			case ESection::ysffreq:
				switch (section)
				{
					case ESection::dmrid:   pdb = &g_Keys.dmriddb;   break;
					case ESection::nxdnid:  pdb = &g_Keys.nxdniddb;  break;
					case ESection::ysffreq: pdb = &g_Keys.ysftxrxdb; break;
				}
				if (0 == key.compare(JURL))
					data[pdb->url] = value;
				else if (0 == key.compare(JMODE))
				{
					if ((0==value.compare("file")) || (0==value.compare("http")) || (0==value.compare("both")))
						data[pdb->mode] = value;
					else
					{
						std::cout << "WARNING: line #" << counter << ": Mode, '" << value << "' not recognized. Setting to 'http'" << std::endl;
						data[pdb->mode] = "http";
					}
				}
				else if (0 == key.compare(JREFRESHMIN))
					data[pdb->refreshmin] = getUnsigned(value, JREFRESHMIN, 15, 14400, 180);
				else if (0 == key.compare(JFILEPATH))
					data[pdb->filepath] = value;
				else
					badParam(key);
				break;
			case ESection::mmdvmclient:
			{
				const auto ck = MmdvmClientKeys(cliInstance);
				if (0 == key.compare("Enable"))
					data[ck.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare("MasterAddress"))
					data[ck.address] = value;
				else if (0 == key.compare("MasterPort"))
					data[ck.port] = getUnsigned(value, "MMDVMClient MasterPort", 1024, 65535, 62031);
				else if (0 == key.compare("LocalPort"))
					data[ck.localport] = getUnsigned(value, "MMDVMClient LocalPort", 0, 65535, 0);
				else if (0 == key.compare("DmrId"))
					data[ck.dmrid] = getUnsigned(value, "MMDVMClient DmrId", 1, 999999999, 0);
				else if (0 == key.compare("Password"))
					data[ck.password] = value;
				else if (0 == key.compare("Callsign"))
					data[ck.callsign] = value;
				else if (0 == key.compare("Latitude"))
					data[ck.latitude] = value;
				else if (0 == key.compare("Longitude"))
					data[ck.longitude] = value;
				else if (0 == key.compare("Location"))
					data[ck.location] = value;
				else if (0 == key.compare("Description"))
					data[ck.description] = value;
				else if (0 == key.compare("URL"))
					data[ck.url] = value;
				else if (0 == key.compare("RxFreq"))
					data[ck.rxfreq] = getUnsigned(value, "MMDVMClient RxFreq", 40000000, 2600000000, 439000000);
				else if (0 == key.compare("TxFreq"))
					data[ck.txfreq] = getUnsigned(value, "MMDVMClient TxFreq", 40000000, 2600000000, 439000000);
				else if (0 == key.compare("Software"))
					data[ck.software] = value;
				else if (0 == key.compare("Firmware"))
					data[ck.firmware] = value;
				else if (0 == key.compare("FallbackDmrId"))
					data[ck.fallbackdmrid] = getUnsigned(value, "MMDVMClient FallbackDmrId", 0, 999999999, 0);
				else if (0 == key.compare("BlockProtocols"))
					data[ck.blockprotocols] = value;
				else if (0 == key.compare("BrandMeisterApiKey"))
					data[ck.bmapikey] = value;
				else if (0 == key.compare("Network"))
					data[ck.network] = value;
				else if (0 == key.compare(0, 2, "TG"))
				{
					// TG<number> = <Module>[,TS<1|2>]
					// e.g. TG26363 = F,TS2 or TG26363 = F
					std::string tgkey = MmdvmClientPrefix(cliInstance) + "TG" + key.substr(2);
					data[tgkey] = value;
				}
				else
					badParam(key);
				break;
			}
			case ESection::svx:
				if (0 == key.compare(JENABLE))
					data[g_Keys.svx.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare("Host"))
					data[g_Keys.svx.host] = value;
				else if (0 == key.compare(JPORT))
					data[g_Keys.svx.port] = getUnsigned(value, "SvxReflector Port", 1024, 65535, 5300);
				else if (0 == key.compare(JCALLSIGN))
					data[g_Keys.svx.callsign] = value;
				else if (0 == key.compare("Password"))
					data[g_Keys.svx.password] = value;
				else if (0 == key.compare("BlockProtocols"))
					data[g_Keys.svx.blockprotocols] = value;
				else if (0 == key.compare("RxGain"))
					data[g_Keys.svx.rxgain] = value;
				else if (0 == key.compare("TxGain"))
					data[g_Keys.svx.txgain] = value;
				else if (0 == key.compare("Software"))
					data[g_Keys.svx.software] = value;
				else if (0 == key.compare(0, 2, "TG"))
				{
					std::string tgkey = "svxTG" + key.substr(2);
					data[tgkey] = value;
				}
				else
					badParam(key);
				break;
			case ESection::svxserver:
				if (0 == key.compare(JENABLE))
					data[g_Keys.svxs.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare(JPORT))
					data[g_Keys.svxs.port] = getUnsigned(value, "SVXServer Port", 1024, 65535, 5300);
				else if (0 == key.compare("BlockProtocols"))
					data[g_Keys.svxs.blockprotocols] = value;
				else if (0 == key.compare("RxGain"))
					data[g_Keys.svxs.rxgain] = value;
				else if (0 == key.compare("TxGain"))
					data[g_Keys.svxs.txgain] = value;
				else if (0 == key.compare(0, 2, "TG"))
				{
					std::string tgkey = "svxsTG" + key.substr(2);
					data[tgkey] = value;
				}
				else
				{
					// Everything else is a user/password pair: Callsign = Password
					std::string userkey = "svxsUser_" + key;
					data[userkey] = value;
				}
				break;
			case ESection::plugins:
				if (0 == key.compare("Load"))
				{
					if (!data.contains(g_Keys.plugins.load))
						data[g_Keys.plugins.load] = nlohmann::json::array();
					data[g_Keys.plugins.load].push_back(value);
				}
				else
					badParam(key);
				break;
			case ESection::extra:
				extra[extraName].emplace_back(key, value);
				break;
			case ESection::ysfclient:
				if (0 == key.compare(JENABLE))
					data[g_Keys.ysfclient.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare(JCALLSIGN))
					data[g_Keys.ysfclient.callsign] = value;
				else if (0 == key.compare("BlockProtocols"))
					data[g_Keys.ysfclient.blockprotocols] = value;
				else if (0 == key.compare(0, 3, "Map"))
				{
					std::string mapkey = "ysfClientMap" + key.substr(3);
					data[mapkey] = value;
				}
				else
					badParam(key);
				break;
			case ESection::dplusclient:
				if (0 == key.compare(JENABLE))
					data[g_Keys.dplusclient.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare(JCALLSIGN))
					data[g_Keys.dplusclient.callsign] = value;
				else if (0 == key.compare("BlockProtocols"))
					data[g_Keys.dplusclient.blockprotocols] = value;
				else if (0 == key.compare(0, 3, "Map"))
				{
					std::string mapkey = "dplusClientMap" + key.substr(3);
					data[mapkey] = value;
				}
				else
					badParam(key);
				break;
			case ESection::dextraclient:
				if (0 == key.compare(JENABLE))
					data[g_Keys.dextraclient.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare(JCALLSIGN))
					data[g_Keys.dextraclient.callsign] = value;
				else if (0 == key.compare("BlockProtocols"))
					data[g_Keys.dextraclient.blockprotocols] = value;
				else if (0 == key.compare(0, 3, "Map"))
				{
					std::string mapkey = "dextraClientMap" + key.substr(3);
					data[mapkey] = value;
				}
				else
					badParam(key);
				break;
			case ESection::dcsclient:
				if (0 == key.compare(JENABLE))
					data[g_Keys.dcsclient.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare(JCALLSIGN))
					data[g_Keys.dcsclient.callsign] = value;
				else if (0 == key.compare("BlockProtocols"))
					data[g_Keys.dcsclient.blockprotocols] = value;
				else if (0 == key.compare(0, 3, "Map"))
				{
					// Map<N> = <Host>,<Port>,<RemoteModule>,<LocalModule>
					std::string mapkey = "dcsClientMap" + key.substr(3);
					data[mapkey] = value;
				}
				else
					badParam(key);
				break;
			case ESection::admin:
				if (0 == key.compare(JENABLE))
					data[g_Keys.admin.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare(JPORT))
					data[g_Keys.admin.port] = getUnsigned(value, "Admin Port", 1024, 65535, 10101);
				else if (0 == key.compare("Password"))
					data[g_Keys.admin.password] = value;
				else if (0 == key.compare(JBINDINGADDRESS))
					data[g_Keys.admin.bindaddress] = value;
				else
					badParam(key);
				break;
			case ESection::credentials:
				if (0 == key.compare("Backend"))
					data[g_Keys.cred.backend] = value;
				else if (0 == key.compare("Path"))
					data[g_Keys.cred.path] = value;
				else if (0 == key.compare("Host"))
					data[g_Keys.cred.host] = value;
				else if (0 == key.compare(JPORT))
					data[g_Keys.cred.port] = getUnsigned(value, "Credentials Port", 1, 65535, 0);
				else if (0 == key.compare("Database"))
					data[g_Keys.cred.database] = value;
				else if (0 == key.compare("User"))
					data[g_Keys.cred.user] = value;
				else if (0 == key.compare("Password"))
					data[g_Keys.cred.password] = value;
				else if (0 == key.compare("Table"))
					data[g_Keys.cred.table] = value;
				else if (0 == key.compare("Timeout"))
					data[g_Keys.cred.timeout] = getUnsigned(value, "Credentials Timeout", 1, 30, 3);
				else if (0 == key.compare("CacheTTL"))
					data[g_Keys.cred.cachettl] = getUnsigned(value, "Credentials CacheTTL", 0, 86400, 300);
				else if (0 == key.compare("NegativeTTL"))
					data[g_Keys.cred.negativettl] = getUnsigned(value, "Credentials NegativeTTL", 0, 86400, 60);
				else if (0 == key.compare("CacheSize"))
					data[g_Keys.cred.cachesize] = getUnsigned(value, "Credentials CacheSize", 0, 1000000, 1000);
				else
					badParam(key);
				break;
			case ESection::echo:
				if (0 == key.compare(JENABLE))
					data[g_Keys.echo.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare("Module"))
				{
					if (value.size() >= 1 && value[0] >= 'A' && value[0] <= 'Z')
						data[g_Keys.echo.module] = std::string(1, value[0]);
				}
				else
					badParam(key);
				break;
			case ESection::files:
				if (0 == key.compare(JPIDPATH))
					data[g_Keys.files.pid] = value;
				else if (0 == key.compare(JXMLPATH))
					data[g_Keys.files.xml] = value;
				else if (0 == key.compare(JJSONPATH))
					data[g_Keys.files.json] = value;
				else if (0 == key.compare(JWHITELISTPATH))
					data[g_Keys.files.white] = value;
				else if (0 == key.compare(JBLACKLISTPATH))
					data[g_Keys.files.black] = value;
				else if (0 == key.compare(JINTERLINKPATH))
					data[g_Keys.files.interlink] = value;
				else if (0 == key.compare(JG3TERMINALPATH))
					data[g_Keys.files.terminal] = value;
				else
					badParam(key);
				break;
			case ESection::websocket:
				if (0 == key.compare(JENABLE))
					data[g_Keys.ws.enable] = IS_TRUE(value[0]);
				else if (0 == key.compare(JPORT))
					data[g_Keys.ws.port] = getUnsigned(value, "WebSocket Port", 1024, 65535, 10102);
				else if (0 == key.compare(JBINDINGADDRESS))
					data[g_Keys.ws.bindaddress] = value;
				else if (0 == key.compare("MaxClients"))
					data[g_Keys.ws.maxclients] = getUnsigned(value, "WebSocket MaxClients", 1, 1000, 64);
				else if (0 == key.compare("QueueLength"))
					data[g_Keys.ws.queuelen] = getUnsigned(value, "WebSocket QueueLength", 16, 10000, 256);
				else if (0 == key.compare("PingInterval"))
					data[g_Keys.ws.pinginterval] = getUnsigned(value, "WebSocket PingInterval", 5, 300, 30);
				else if (0 == key.compare("IdleTimeout"))
					data[g_Keys.ws.idletimeout] = getUnsigned(value, "WebSocket IdleTimeout", 10, 3600, 90);
				else
					badParam(key);
				break;
			default:
				std::cout << "WARNING: parameter '" << line << "' defined before any [section]" << std::endl;
		}

	}
	cfgfile.close();

	////////////////////////////// check the input
	// Names section
	if (isDefined(ErrorLevel::fatal, JNAMES, JCALLSIGN, g_Keys.names.callsign, rval))
	{
		const auto cs = data[g_Keys.names.callsign].get<std::string>();
		auto RefRegEx = std::regex("^URF([A-Z0-9]){3,3}$", std::regex::extended);
		if (! std::regex_match(cs, RefRegEx))
		{
			std::cerr << "ERROR: [" << JNAMES << ']' << JCALLSIGN << " '" << cs <<  "' is malformed" << std::endl;
			rval = true;
		}
	}

#ifndef NO_DHT
	isDefined(ErrorLevel::fatal, JNAMES, JBOOTSTRAP, g_Keys.names.bootstrap, rval);
#endif
	isDefined(ErrorLevel::fatal, JNAMES, JDASHBOARDURL, g_Keys.names.url, rval);
	isDefined(ErrorLevel::mild, JNAMES, JSYSOPEMAIL, g_Keys.names.email, rval);
	isDefined(ErrorLevel::mild, JNAMES, JCOUNTRY, g_Keys.names.country, rval);
	isDefined(ErrorLevel::mild, JNAMES, JSPONSOR, g_Keys.names.sponsor, rval);

	// IP Address section
	// ipv4 bind and external
	if (isDefined(ErrorLevel::fatal, JIPADDRESSES, JIPV4BINDING, g_Keys.ip.ipv4bind, rval))
	{
		if (std::regex_match(data[g_Keys.ip.ipv4bind].get<std::string>(), IPv4RegEx))
		{
			if (data.contains(g_Keys.ip.ipv4address))
			{
				auto v4 = data[g_Keys.ip.ipv4address].get<std::string>();
				if (std::regex_match(v4, IPv4RegEx))
				{
					if (ipv4.compare(v4))
					std::cout << "WARNING: specified IPv4 external address, " << v4 << ", is different than detected address, " << ipv4 << std::endl;
				}
				else
				{
					std::cerr << "ERROR: specified IPv4 external address, " << v4 << ", is malformed" << std::endl;
					rval = true;
				}
			}
			else
			{
				// make sure curl worked!
				if (std::regex_match(ipv4, IPv4RegEx))
					data[g_Keys.ip.ipv4address] = ipv4;
				else
				{
					std::cerr << "ERROR: could not detect IPv4 address at this time" << std::endl;
					rval = true;
				}
			}
		}
		else
		{
			std::cerr << "ERROR: IPv4 binding address, " << data[g_Keys.ip.ipv4address].get<std::string>() << ", is malformed" << std::endl;
			rval = true;
		}
	}

	// ipv6 bind and external
	if (data.contains(g_Keys.ip.ipv6bind))
	{
		if (std::regex_match(data[g_Keys.ip.ipv6bind].get<std::string>(), IPv6RegEx))
		{
			if (data.contains(g_Keys.ip.ipv6address))
			{
				auto v6 = data[g_Keys.ip.ipv6address].get<std::string>();
				if (std::regex_match(v6, IPv6RegEx))
				{
					if (ipv6.compare(v6))
						std::cout << "WARNING: specified IPv6 external address [" << v6 << "], is different than detected address [" << ipv6 << ']' << std::endl;
				}
				else
				{
					std::cerr << "ERROR: the specified IPv6 address [" << v6 << "] is malformed" << std::endl;
					rval = true;
				}
			}
			else
			{
				// make sure curl worked!
				if (std::regex_match(ipv6, IPv6RegEx))
					data[g_Keys.ip.ipv6address] = ipv6;
				else
				{
					std::cerr << "ERROR: could not detect IPv6 address at this time" << std::endl;
					rval = true;
				}
			}
		}
		else
		{
			std::cerr << "ERROR: IPv6 binding address, " << data[g_Keys.ip.ipv6address].get<std::string>() << ", is malformed" << std::endl;
			rval = true;
		}
	}
	else
	{
		data[g_Keys.ip.ipv6bind] = nullptr;
		data[g_Keys.ip.ipv6address] = nullptr;
	}

	// Modules section
	if (isDefined(ErrorLevel::fatal, JMODULES, JMODULES, g_Keys.modules.modules, rval))
	{
		const auto mods(data[g_Keys.modules.modules].get<std::string>());

		// finally, check the module descriptions
		for (unsigned i=0; i<26; i++)
		{
			if (std::string::npos == mods.find('A'+i))
			{
				if (data.contains(g_Keys.modules.descriptor[i]))
				{
					std::cout << "WARNING: " << g_Keys.modules.descriptor[i] << " defined for an unconfigured module. Deleting..." << std::endl;
					data.erase(g_Keys.modules.descriptor[i]);
				}
			}
			else
			{
				if (! data.contains(g_Keys.modules.descriptor[i]))
				{
					std::string value("Module ");
					value.append(1, 'A'+i);
					std::cout << "WARNING: " << g_Keys.modules.descriptor[i] << " not found. Setting to " << value << std::endl;
					data[g_Keys.modules.descriptor[i]] = value;
				}
			}
		}
	}

	// Transcoder section
	if (isDefined(ErrorLevel::fatal, JTRANSCODER, JPORT, g_Keys.tc.port, rval))
	{
		tcport = GetUnsigned(g_Keys.tc.port);
		if (tcport)
		{
			// no Password in [Transcoder]: an empty one (the template has it commented out)
			if (!data.contains(g_Keys.tc.password))
				data[g_Keys.tc.password] = "";
			if (isDefined(ErrorLevel::fatal, JTRANSCODER, JBINDINGADDRESS, g_Keys.tc.bind, rval))
			{
				const auto bind(data[g_Keys.tc.bind].get<std::string>());
				if (!std::regex_match(bind, IPv4RegEx) && !std::regex_match(bind, IPv6RegEx))
				{
					std::cerr << "ERROR: Transcoder bind address [" << bind << "] is malformed" << std::endl;
					rval = true;
				}
				else if (0 != bind.compare(0, 4, "127.") && 0 != bind.compare("::1") && GetString(g_Keys.tc.password).empty())
					std::cout << "WARNING: the transcoder port listens on " << bind << " without a [Transcoder] Password, anyone who reaches it can take over the transcoded modules" << std::endl;
			}
			if (!data.contains(g_Keys.tc.automatic))
				data[g_Keys.tc.automatic] = false;
			if (data[g_Keys.tc.automatic].get<bool>())
			{
				// Modules = auto: every module may be transcoded, tcd's list decides
				data[g_Keys.tc.modules] = GetString(g_Keys.modules.modules);
				std::cout << "[" << JTRANSCODER << "] Modules = auto: a module is transcoded while tcd is connected for it (USRP, SvxReflector and SVXServer modules need tcd to have them)" << std::endl;
			}
			else if (isDefined(ErrorLevel::fatal, JTRANSCODER, JMODULES, g_Keys.tc.modules, rval))
			{
				const auto tcmods(data[g_Keys.tc.modules].get<std::string>());

				// make sure each transcoded module is configured
				const std::string mods(GetString(g_Keys.modules.modules));
				for (auto c : tcmods)
				{
					if (std::string::npos == mods.find(c))
					{
						std::cerr << "ERROR: transcoded module '" << c << "' not found in defined modules" << std::endl;
						rval = true;
					}
				}
			}
		}
		else
		{
			// there is no transcoder
			data[g_Keys.tc.automatic] = false;
			data[g_Keys.tc.modules] = "";
			data[g_Keys.tc.bind] = "";
			data[g_Keys.tc.password] = "";
		}
	}

	// "simple" protocols with only a Port
	isDefined(ErrorLevel::fatal, JDCS, JPORT, g_Keys.dcs.port, rval);
	isDefined(ErrorLevel::fatal, JDEXTRA, JPORT, g_Keys.dextra.port, rval);
	isDefined(ErrorLevel::fatal, JDMRPLUS, JPORT, g_Keys.dmrplus.port, rval);
	isDefined(ErrorLevel::fatal, JDPLUS, JPORT, g_Keys.dplus.port, rval);
	isDefined(ErrorLevel::fatal, JM17, JPORT, g_Keys.m17.port, rval);
	isDefined(ErrorLevel::fatal, JURF, JPORT, g_Keys.urf.port, rval);

	// XLXPeer (legacy: Brandmeister)
	if (isDefined(ErrorLevel::fatal, JXLXPEER, JENABLE, g_Keys.xlxpeer.enable, rval))
	{
		if (GetBoolean(g_Keys.xlxpeer.enable))
		{
			isDefined(ErrorLevel::fatal, JXLXPEER, JPORT, g_Keys.xlxpeer.port, rval);
		}
	}

	// G3
	isDefined(ErrorLevel::fatal, JG3, JENABLE, g_Keys.g3.enable, rval);

	// Credentials: optional, the logins of MMDVM, SVXServer (and of plugins) from a database
	{
		// no database by default; a SQLite file sits next to urfd.ini
		const auto slash = inipath.find_last_of('/');
		const std::string dbpath = (std::string::npos == slash ? std::string(".") : inipath.substr(0, slash)) + "/credentials.db";
		const std::pair<const std::string &, nlohmann::json> credDefaults[] = {
			{ g_Keys.cred.backend, "ini" }, { g_Keys.cred.path, dbpath },
			{ g_Keys.cred.host, "localhost" }, { g_Keys.cred.port, 0u }, { g_Keys.cred.database, "urfd" },
			{ g_Keys.cred.user, "urfd" }, { g_Keys.cred.password, "" }, { g_Keys.cred.table, "urfd_credentials" },
			{ g_Keys.cred.timeout, 3u }, { g_Keys.cred.cachettl, 300u }, { g_Keys.cred.negativettl, 60u }, { g_Keys.cred.cachesize, 1000u } };	// unsigned, for GetUnsigned()
		for (const auto &[key, def] : credDefaults)
		{
			if (!data.contains(key))
				data[key] = def;
		}
		const std::string backend = GetString(g_Keys.cred.backend);
		if (backend != "ini" && backend != "sqlite" && backend != "mysql" && backend != "postgresql")
		{
			std::cerr << "ERROR: [Credentials] Backend must be ini, sqlite, mysql or postgresql" << std::endl;
			rval = true;
		}
		else if (backend != "ini" && !CredBackendBuilt(backend))
		{
			std::cerr << "ERROR: [Credentials] Backend = " << backend << ", but this urfd was built without it (install its client library and rebuild)" << std::endl;
			rval = true;
		}
		// the table name goes into the SQL text
		const std::string table = GetString(g_Keys.cred.table);
		if (table.empty() || table.size() > 64 || !std::all_of(table.begin(), table.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
		{
			std::cerr << "ERROR: [Credentials] Table may only contain letters, digits and '_'" << std::endl;
			rval = true;
		}
	}
	const bool credDb = (GetString(g_Keys.cred.backend) != "ini");

	// MMDVM
	if (isDefined(ErrorLevel::mild, JMMDVM, JENABLE, g_Keys.mmdvm.enable, rval))
	{
		if (GetBoolean(g_Keys.mmdvm.enable))
		{
			isDefined(ErrorLevel::fatal, JMMDVM, JPORT, g_Keys.mmdvm.port, rval);
			// Validate TG mappings: modules must be configured
			if (data.contains(g_Keys.modules.modules))
			{
				const auto mods(GetString(g_Keys.modules.modules));
				for (auto it = data.begin(); it != data.end(); ++it)
				{
					const std::string &k = it.key();
					if (k.substr(0, 7) == "mmdvmTG")
					{
						std::string val = it.value().get<std::string>();
						if (val.size() >= 1 && std::string::npos == mods.find(val[0]))
						{
							std::cerr << "ERROR: [" << JMMDVM << "] TG" << k.substr(7) << " module " << val[0] << " is not a configured module" << std::endl;
							rval = true;
						}
					}
				}
			}
		}
	}

	// NXDN
	if (isDefined(ErrorLevel::mild, JNXDN, JENABLE, g_Keys.nxdn.enable, rval))
	{
		if (GetBoolean(g_Keys.nxdn.enable))
		{
			isDefined(ErrorLevel::fatal, JNXDN, JPORT, g_Keys.nxdn.port, rval);
			checkAutoLink(JNXDN, JAUTOLINKMODULE, g_Keys.nxdn.autolinkmod, rval);
			isDefined(ErrorLevel::fatal, JNXDN, JREFLECTORID, g_Keys.nxdn.reflectorid, rval);
		}
	}

	// P25
	if (isDefined(ErrorLevel::mild, JP25, JENABLE, g_Keys.p25.enable, rval))
	{
		if (GetBoolean(g_Keys.p25.enable))
		{
			isDefined(ErrorLevel::fatal, JP25, JPORT, g_Keys.p25.port, rval);
			checkAutoLink(JP25, JAUTOLINKMODULE, g_Keys.p25.autolinkmod, rval);
			isDefined(ErrorLevel::fatal, JP25, JREFLECTORID, g_Keys.p25.reflectorid, rval);
		}
	}

	// USRP
	if (isDefined(ErrorLevel::fatal, JUSRP, JENABLE, g_Keys.usrp.enable, rval))
	{
		if (GetBoolean(g_Keys.usrp.enable))
		{
			if (tcport)
			{
				if (isDefined(ErrorLevel::fatal, JUSRP, JMODULE, g_Keys.usrp.module, rval))
				{
					if (std::string::npos == GetString(g_Keys.tc.modules).find(GetString(g_Keys.usrp.module).at(0)))
					{
						std::cerr << "ERROR: [" << JUSRP << ']' << JMODULE << " is not a transcoded module" << std::endl;
						rval = true;
					}
				}
				if (isDefined(ErrorLevel::fatal, JUSRP, JIPADDRESS, g_Keys.usrp.ip, rval))
				{
					// check for syntax
					if (! std::regex_match(data[g_Keys.usrp.ip].get<std::string>(), IPv4RegEx))
					{
						std::cerr << "ERROR: [" << JUSRP << ']' << JIPADDRESS " '" << data[g_Keys.usrp.ip] << "' is malformed" << std::endl;
						rval = true;
					}
				}
				isDefined(ErrorLevel::fatal, JUSRP, JTXPORT, g_Keys.usrp.txport, rval);
				isDefined(ErrorLevel::fatal, JUSRP, JRXPORT, g_Keys.usrp.rxport, rval);
				isDefined(ErrorLevel::fatal, JUSRP, JCALLSIGN, g_Keys.usrp.callsign, rval);
				//if (isDefined(ErrorLevel::fatal, JUSRP, JFILEPATH, g_Keys.usrp.filepath, rval))
				if (data.contains(g_Keys.usrp.filepath))
					checkFile(JUSRP, JFILEPATH, data[g_Keys.usrp.filepath]);
			}
			else
			{
				std::cerr << "ERROR: " << JUSRP << " requires a transcoder" << std::endl;
				rval = true;
			}
		}
	}

	// SvxReflector
	if (isDefined(ErrorLevel::mild, JSVXREFLECTOR, JENABLE, g_Keys.svx.enable, rval))
	{
		if (GetBoolean(g_Keys.svx.enable))
		{
			if (tcport)
			{
				isDefined(ErrorLevel::fatal, JSVXREFLECTOR, "Host", g_Keys.svx.host, rval);
				isDefined(ErrorLevel::fatal, JSVXREFLECTOR, JPORT, g_Keys.svx.port, rval);
				isDefined(ErrorLevel::fatal, JSVXREFLECTOR, JCALLSIGN, g_Keys.svx.callsign, rval);
				isDefined(ErrorLevel::fatal, JSVXREFLECTOR, "Password", g_Keys.svx.password, rval);
				// Validate TG mappings: each module must be transcoded
				const auto &jdata = GetData();
				for (auto it = jdata.begin(); it != jdata.end(); ++it)
				{
					const std::string &k = it.key();
					if (k.substr(0, 5) == "svxTG")
					{
						std::string val = it.value().get<std::string>();
						if (val.size() >= 1 && std::string::npos == GetString(g_Keys.tc.modules).find(val[0]))
						{
							std::cerr << "ERROR: [" << JSVXREFLECTOR << "] TG" << k.substr(5) << " module " << val[0] << " is not a transcoded module" << std::endl;
							rval = true;
						}
					}
				}
			}
			else
			{
				std::cerr << "ERROR: " << JSVXREFLECTOR << " requires a transcoder" << std::endl;
				rval = true;
			}
		}
	}

	// SVXServer
	if (isDefined(ErrorLevel::mild, JSVXSERVER, JENABLE, g_Keys.svxs.enable, rval))
	{
		if (GetBoolean(g_Keys.svxs.enable))
		{
			if (tcport)
			{
				isDefined(ErrorLevel::fatal, JSVXSERVER, JPORT, g_Keys.svxs.port, rval);
				// Validate TG mappings: each module must be transcoded
				const auto &jdata = GetData();
				bool hasUsers = false;
				for (auto it = jdata.begin(); it != jdata.end(); ++it)
				{
					const std::string &k = it.key();
					if (k.substr(0, 6) == "svxsTG")
					{
						std::string val = it.value().get<std::string>();
						if (val.size() >= 1 && std::string::npos == GetString(g_Keys.tc.modules).find(val[0]))
						{
							std::cerr << "ERROR: [" << JSVXSERVER << "] TG" << k.substr(6) << " module " << val[0] << " is not a transcoded module" << std::endl;
							rval = true;
						}
					}
					if (k.substr(0, 9) == "svxsUser_")
						hasUsers = true;
				}
				if (!hasUsers && !credDb)
				{
					std::cerr << "ERROR: [" << JSVXSERVER << "] no users configured" << std::endl;
					rval = true;
				}
			}
			else
			{
				std::cerr << "ERROR: " << JSVXSERVER << " requires a transcoder" << std::endl;
				rval = true;
			}
		}
	}

	// CallingHome, only checked when it is switched on
	if (data.contains(g_Keys.callinghome.enable) && GetBoolean(g_Keys.callinghome.enable))
	{
		isDefined(ErrorLevel::mild, JCALLINGHOME, "Country", g_Keys.callinghome.country, rval);
		isDefined(ErrorLevel::mild, JCALLINGHOME, "Comment", g_Keys.callinghome.comment, rval);
		// the dashboard's CreateCode(16) made 16, older ones may differ: just letters and digits
		if (data.contains(g_Keys.callinghome.hash))
		{
			const auto hash = data[g_Keys.callinghome.hash].get<std::string>();
			if (hash.size() > 64 || std::string::npos != hash.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"))
			{
				std::cerr << "ERROR: [" << JCALLINGHOME << "] Hash must be letters and digits only, at most 64" << std::endl;
				rval = true;
			}
		}
	}

	// Echo
	if (isDefined(ErrorLevel::mild, "Echo", JENABLE, g_Keys.echo.enable, rval))
	{
		if (GetBoolean(g_Keys.echo.enable))
		{
			if (isDefined(ErrorLevel::fatal, "Echo", JMODULE, g_Keys.echo.module, rval))
			{
				if (data.contains(g_Keys.modules.modules))
				{
					const auto mods(GetString(g_Keys.modules.modules));
					const auto echomod = GetString(g_Keys.echo.module);
					if (echomod.size() >= 1 && std::string::npos == mods.find(echomod[0]))
					{
						std::cerr << "ERROR: [Echo] Module '" << echomod[0] << "' is not a configured module" << std::endl;
						rval = true;
					}
				}
			}
		}
	}

	// Admin
	if (isDefined(ErrorLevel::mild, "Admin", JENABLE, g_Keys.admin.enable, rval))
	{
		if (GetBoolean(g_Keys.admin.enable))
		{
			isDefined(ErrorLevel::fatal, "Admin", JPORT, g_Keys.admin.port, rval);
			isDefined(ErrorLevel::fatal, "Admin", "Password", g_Keys.admin.password, rval);
		}
	}

	// WebSocket: optional, every key has a default
	const std::pair<const std::string &, nlohmann::json> wsDefaults[] = {
		{ g_Keys.ws.enable, false }, { g_Keys.ws.port, 10102u }, { g_Keys.ws.bindaddress, "127.0.0.1" },
		{ g_Keys.ws.maxclients, 64u }, { g_Keys.ws.queuelen, 256u }, { g_Keys.ws.pinginterval, 30u }, { g_Keys.ws.idletimeout, 90u } };	// unsigned, for GetUnsigned()
	for (const auto &[key, def] : wsDefaults)
	{
		if (!data.contains(key))
			data[key] = def;
	}

	// MMDVMClient, MMDVMClient2 ... MMDVMClient9
	{
		struct SUsed { std::string network, name; };
		std::map<std::string, SUsed> usedTG, usedId;	// "<network>/<TG or DMR ID>" -> first user
		for (unsigned n = 1; n <= MMDVMCLI_MAX_INSTANCES; n++)
		{
			const auto ck = MmdvmClientKeys(n);
			const std::string name = MmdvmClientName(n);
			if (n > 1 && !data.contains(ck.enable))
				continue;	// no such section
			if (!isDefined(ErrorLevel::mild, name, JENABLE, ck.enable, rval) || !GetBoolean(ck.enable))
				continue;
			isDefined(ErrorLevel::fatal, name, "MasterAddress", ck.address, rval);
			isDefined(ErrorLevel::fatal, name, "MasterPort", ck.port, rval);
			isDefined(ErrorLevel::fatal, name, "DmrId", ck.dmrid, rval);
			isDefined(ErrorLevel::fatal, name, "Password", ck.password, rval);
			isDefined(ErrorLevel::fatal, name, "Callsign", ck.callsign, rval);
			const std::string network = MmdvmClientNetwork(data.contains(ck.network) ? GetString(ck.network) : "",
				data.contains(ck.address) ? GetString(ck.address) : "");
			// one network gets a hotspot ID and a TG from one connection only
			if (data.contains(ck.dmrid))
			{
				const std::string idKey = network + "/" + std::to_string(GetUnsigned(ck.dmrid));
				if (usedId.count(idKey))
				{
					std::cerr << "ERROR: [" << name << "] DmrId " << GetUnsigned(ck.dmrid) << " is used by [" << usedId[idKey].name << "] in the same network " << network << std::endl;
					rval = true;
				}
				else
					usedId[idKey] = { network, name };
			}
			// TG mappings: the module must exist; without transcoding only AMBE+2 (DMR,
			// YSF, NXDN) crosses it, the other modes stay local
			const auto tcmods(tcport ? GetString(g_Keys.tc.modules) : std::string());
			const auto mods(data.contains(g_Keys.modules.modules) ? GetString(g_Keys.modules.modules) : std::string());
			const std::string tgPrefix = MmdvmClientPrefix(n) + "TG";
			for (auto it = data.begin(); it != data.end(); ++it)
			{
				const std::string &k = it.key();
				if (k.compare(0, tgPrefix.size(), tgPrefix) != 0 || k.size() == tgPrefix.size() || !std::isdigit((unsigned char)k[tgPrefix.size()]))
					continue;
				const std::string tg = k.substr(tgPrefix.size());
				std::string val = it.value().get<std::string>();
				if (val.size() >= 1 && std::string::npos == mods.find(val[0]))
				{
					std::cerr << "ERROR: [" << name << "] TG" << tg << " module " << val[0] << " is not a configured module" << std::endl;
					rval = true;
				}
				else if (val.size() >= 1 && std::string::npos == tcmods.find(val[0]))
					std::cout << "NOTE: [" << name << "] TG" << tg << " is on module " << val[0] << ", which is not transcoded: only DMR, YSF and NXDN reach it and come from it" << std::endl;
				const std::string tgKey = network + "/" + tg;
				if (usedTG.count(tgKey))
				{
					std::cerr << "ERROR: [" << name << "] TG" << tg << " is mapped by [" << usedTG[tgKey].name << "] in the same network " << network << " already" << std::endl;
					rval = true;
				}
				else
					usedTG[tgKey] = { network, name };
			}
		}
	}

	// DCSClient
	if (isDefined(ErrorLevel::mild, JDCSCLIENT, JENABLE, g_Keys.dcsclient.enable, rval))
	{
		if (GetBoolean(g_Keys.dcsclient.enable))
		{
			isDefined(ErrorLevel::fatal, JDCSCLIENT, JCALLSIGN, g_Keys.dcsclient.callsign, rval);
			bool hasMap = false;
			for (auto it = data.begin(); it != data.end(); ++it)
				if (it.key().substr(0, 12) == "dcsClientMap") { hasMap = true; break; }
			if (!hasMap)
				std::cout << "WARNING: [" << JDCSCLIENT << "] no Map entries configured" << std::endl;
		}
	}

	// DExtraClient
	if (isDefined(ErrorLevel::mild, "DExtraClient", JENABLE, g_Keys.dextraclient.enable, rval))
	{
		if (GetBoolean(g_Keys.dextraclient.enable))
		{
			isDefined(ErrorLevel::fatal, "DExtraClient", JCALLSIGN, g_Keys.dextraclient.callsign, rval);
			bool hasMap = false;
			for (auto it = data.begin(); it != data.end(); ++it)
				if (it.key().substr(0, 14) == "dextraClientMa") { hasMap = true; break; }
			if (!hasMap)
				std::cout << "WARNING: [DExtraClient] no Map entries configured" << std::endl;
		}
	}

	// DPlusClient
	if (isDefined(ErrorLevel::mild, "DPlusClient", JENABLE, g_Keys.dplusclient.enable, rval))
	{
		if (GetBoolean(g_Keys.dplusclient.enable))
		{
			isDefined(ErrorLevel::fatal, "DPlusClient", JCALLSIGN, g_Keys.dplusclient.callsign, rval);
			bool hasMap = false;
			for (auto it = data.begin(); it != data.end(); ++it)
				if (it.key().substr(0, 14) == "dplusClientMap") { hasMap = true; break; }
			if (!hasMap)
				std::cout << "WARNING: [DPlusClient] no Map entries configured" << std::endl;
		}
	}

	// YSFClient
	if (isDefined(ErrorLevel::mild, "YSFClient", JENABLE, g_Keys.ysfclient.enable, rval))
	{
		if (GetBoolean(g_Keys.ysfclient.enable))
		{
			isDefined(ErrorLevel::fatal, "YSFClient", JCALLSIGN, g_Keys.ysfclient.callsign, rval);
			bool hasMap = false;
			for (auto it = data.begin(); it != data.end(); ++it)
				if (it.key().substr(0, 12) == "ysfClientMap") { hasMap = true; break; }
			if (!hasMap)
				std::cout << "WARNING: [YSFClient] no Map entries configured" << std::endl;
		}
	}

	// YSF
	isDefined(ErrorLevel::fatal, JYSF, JPORT, g_Keys.ysf.port, rval);
	isDefined(ErrorLevel::mild, JYSF, JENABLEDGID, g_Keys.ysf.enabledgid, rval);
	checkAutoLink(JYSF, JAUTOLINKMODULE, g_Keys.ysf.autolinkmod, rval);
	isDefined(ErrorLevel::fatal, JYSF, JDEFAULTRXFREQ, g_Keys.ysf.defaultrxfreq, rval);
	isDefined(ErrorLevel::fatal, JYSF, JDEFAULTTXFREQ, g_Keys.ysf.defaulttxfreq, rval);
	isDefined(ErrorLevel::mild, JYSF, JREGISTRATIONID, g_Keys.ysf.ysfreflectordb.id, rval);
	isDefined(ErrorLevel::mild, JYSF, JREGISTRATIONNAME, g_Keys.ysf.ysfreflectordb.name, rval);
	isDefined(ErrorLevel::mild, JYSF, JREGISTRATIONDESCRIPTION, g_Keys.ysf.ysfreflectordb.description, rval);

	// Databases
	std::list<std::pair<const std::string, const struct SJsonKeys::DB *>> dbs = {
		{ JDMRIDDB,   &g_Keys.dmriddb   },
		{ JNXDNIDDB,  &g_Keys.nxdniddb  },
		{ JYSFTXRXDB, &g_Keys.ysftxrxdb }
	};
	for ( auto &item : dbs )
	{
		if (isDefined(ErrorLevel::fatal, item.first, JMODE,       item.second->mode,       rval))
		{
			if (ERefreshType::file != GetRefreshType(item.second->mode))
			{
				isDefined(ErrorLevel::fatal, item.first, JURL,        item.second->url,        rval);
				isDefined(ErrorLevel::fatal, item.first, JREFRESHMIN, item.second->refreshmin, rval);
			}
			if (ERefreshType::http != GetRefreshType(item.second->mode))
			{
				if (isDefined(ErrorLevel::fatal, item.first, JFILEPATH,   item.second->filepath,   rval))
					checkFile(item.first, JFILEPATH, data[item.second->filepath]);
			}
		}
	}

	// Other files
	isDefined(ErrorLevel::fatal, JFILES, JPIDPATH, g_Keys.files.pid, rval);
	isDefined(ErrorLevel::fatal, JFILES, JXMLPATH, g_Keys.files.xml, rval);
	if (isDefined(ErrorLevel::fatal, JFILES, JWHITELISTPATH, g_Keys.files.white, rval))
		checkFile(JFILES, JWHITELISTPATH, data[g_Keys.files.white]);
	if (isDefined(ErrorLevel::fatal, JFILES, JBLACKLISTPATH, g_Keys.files.black, rval))
		checkFile(JFILES, JBLACKLISTPATH, data[g_Keys.files.black]);
	if (isDefined(ErrorLevel::fatal, JFILES, JINTERLINKPATH, g_Keys.files.interlink, rval))
		checkFile(JFILES, JINTERLINKPATH, data[g_Keys.files.interlink]);
	if (data.contains(g_Keys.g3.enable) && GetBoolean(g_Keys.g3.enable))
	{
		if (isDefined(ErrorLevel::fatal, JFILES, JG3TERMINALPATH, g_Keys.files.terminal, rval))
			checkFile(JFILES, JG3TERMINALPATH, data[g_Keys.files.terminal]);
	}

	// Plugins: they read their sections, the rest is unknown
	if (PluginHook)
	{
		if (PluginHook(*this, extra))
			rval = true;
	}
	else
	{
		for (const auto &sec : extra)
			std::cout << "NOTE: [" << sec.first << "] is no section of urfd, perhaps one of a plugin: not checked here" << std::endl;
	}

	return rval;
}

bool CConfigure::isDefined(ErrorLevel level, const std::string &section, const std::string &pname, const std::string &key, bool &rval)
{
	if (data.contains(key))
		return true;

	if (ErrorLevel::mild == level)
	{
		std::cout << "WARNING: [" << section << ']' << pname << " is not defined" << std::endl;
		data[key] = nullptr;
	}
	else
	{
		std::cerr << "ERROR: [" << section << ']' << pname << " is not defined" << std::endl;
		rval = true;
	}
	return false;
}

void CConfigure::checkAutoLink(const std::string &section, const std::string &pname, const std::string &key, bool &rval) const
{
	const auto autolinkModule = GetAutolinkModule(key);
	if (' ' != autolinkModule)
	{
		auto ismods = data.contains(g_Keys.modules.modules);
		const auto mods(ismods ? data[g_Keys.modules.modules].get<std::string>() : "");
		if (std::string::npos == mods.find(autolinkModule))
		{
			std::cerr << "ERROR: [" << section << ']' << pname << " module '" << autolinkModule << "' not a configured module" << std::endl;
			rval = true;
		}
	}
}

std::string CConfigure::getDataRefreshType(ERefreshType type) const
{
	if (ERefreshType::both == type)
		return std::string("both");
	else if (ERefreshType::file == type)
		return std::string("file");
	else
		return std::string("http");
}

unsigned CConfigure::getUnsigned(const std::string &valuestr, const std::string &label, unsigned min, unsigned max, unsigned def) const
{
	auto i = unsigned(std::stoul(valuestr.c_str()));
	if ( i < min || i > max )
	{
		std::cout << "WARNING: line #" << counter << ": " << label << " is out of range. Reset to " << def << std::endl;
		i = def;
	}
	return (unsigned)i;
}

unsigned CConfigure::ParseUnsigned(const std::string &value, const std::string &label, unsigned min, unsigned max, unsigned def)
{
	unsigned long i = def;
	try
	{
		size_t used = 0;
		i = std::stoul(value, &used);
		if (used != value.size())
			throw std::invalid_argument(value);
	}
	catch (const std::exception &)
	{
		std::cout << "WARNING: " << label << " '" << value << "' is no number. Set to " << def << std::endl;
		return def;
	}
	if (i < min || i > max)
	{
		std::cout << "WARNING: " << label << " is out of range. Reset to " << def << std::endl;
		return def;
	}
	return unsigned(i);
}

void CConfigure::badParam(const std::string &key) const
{
	std::cout << "WARNING: line #" << counter << ": Unexpected parameter: '" << key << "'" << std::endl;
}

bool CConfigure::checkModules(std::string &m) const
{
	bool rval = false; // return true on error
	for (unsigned i=0; i<m.size(); i++)
		if (islower(m[i]))
			m[i] = toupper(m[i]);

	const std::string all("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
	std::string found;
	for (auto c : all)
		if (std::string::npos != m.find(c) && std::string::npos == found.find(c))
			found.append(1, c);
	m.assign(found);
	return m.empty();
}

void CConfigure::setAutolink(const std::string &section, const std::string &key, const std::string &value)
{
	auto c = toupper(value.at(0));
	if (isupper(c))
		data[key] = std::string(1, c);
	else
		std::cout << "WARNING: line #" << counter << ": " << section << " AutoLinkModule is invalid: '" << value.substr(0, 1) << "'" << std::endl;
}

void CConfigure::checkFile(const std::string &section, const std::string &key, const std::string &filepath) const
{
	struct stat sstat;
	auto rval = stat(filepath.c_str(), &sstat);
	if (rval)
	{
		std::cout << "WARNING: [" << section << ']' << key << " \"" << filepath << "\": " << strerror(errno) << std::endl;
	}
}

void CConfigure::Dump(bool justpublic) const
{
	nlohmann::json tmpjson = data;
	if (justpublic)
	{
		for (auto &it : data.items())
		{
			if (islower(it.key().at(0)))
				tmpjson.erase(it.key());
		}
	}
	std::cout << tmpjson.dump(4) << std::endl;
}

ERefreshType CConfigure::GetRefreshType(const std::string &key) const
{
	ERefreshType type = ERefreshType::http;
	if (data.contains(key))
	{
		if (data[key].is_string())
		{
			auto s = data[key].get<std::string>();
			if (0 == s.compare("both"))
				type = ERefreshType::both;
			else if (0 == s.compare("file"))
				type = ERefreshType::file;
			else
				type = ERefreshType::http;
		}
	}
	return type;
}

bool CConfigure::Contains(const std::string &key) const
{
	return data.contains(key);
}

std::string CConfigure::GetString(const std::string &key) const
{
	std::string str;
	if (data.contains(key))
	{
		if (data[key].is_null())
		{
			// null is the same thing as an empty string
			return str;
		}
		else if (data[key].is_string())
		{
			str.assign(data[key].get<std::string>());
		}
		else
			std::cerr << "ERROR: GetString(): '" << key << "' is not a string" << std::endl;
	}
	else
	{
		std::cerr << "ERROR: GetString(): item at '" << key << "' is not defined" << std::endl;
	}
	return str;
}

unsigned CConfigure::GetUnsigned(const std::string &key) const
{
	unsigned u = 0;
	if (data.contains(key))
	{
		if (data[key].is_number_unsigned())
		{
			u = data[key].get<unsigned>();
		}
		else
			std::cerr << "ERROR: GetUnsigned(): '" << key << "' is not an unsigned value" << std::endl;
	}
	else
	{
		std::cerr << "ERROR: GetUnsigned(): item at '" << key << "' is not defined" << std::endl;
	}
	return u;
}

bool CConfigure::GetBoolean(const std::string &key) const
{
	if (data.contains(key) && data[key].is_boolean())
		return data[key].get<bool>();
	else
		return false;
}

char CConfigure::GetAutolinkModule(const std::string &key) const
{
	char c = ' ';
	if (data.contains(key))
	{
		if (data[key].is_string())
		{
			c = data[key].get<std::string>().at(0);
		}
	}
	return c;
}

bool CConfigure::IsString(const std::string &key) const
{
	if (data.contains(key))
		return data[key].is_string();
	return false;
}

#ifdef INICHECK
SJsonKeys g_Keys;
int main(int argc, char *argv[])
{
	if (3 == argc && strlen(argv[1]) > 1 && '-' == argv[1][0])
	{
		CConfigure d;
		auto rval = d.ReadData(argv[2]);
		if ('q' != argv[1][1])
			d.Dump(('n' == argv[1][1]) ? true : false);
		return rval ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	std::cerr << "Usage: " << argv[0] << " -(q|n|v) FILENAME\nWhere:\n\t-q just prints warnings and errors.\n\t-n also prints keys that begin with an uppercase letter.\n\t-v prints all keys, warnings and errors." << std::endl;
	return EXIT_SUCCESS;
}
#endif
