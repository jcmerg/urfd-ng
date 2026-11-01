// CallingHome -- registers the reflector in the XLX directory
//
// Copyright (C) 2026 Jens-Christian Merg (DL4JC)
// Licensed under the GNU General Public License v3 or later

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <vector>

#include "CallingHome.h"
#include "CurlGet.h"
#include "Global.h"
#include "Utils.h"

#define CALLINGHOME_SERVER   "http://xlxapi.rlx.lu/api.php"

void CCallingHome::Start(void)
{
	if (!g_Configure.Contains(g_Keys.callinghome.enable) || !g_Configure.GetBoolean(g_Keys.callinghome.enable))
		return;

	// the directory knows URF reflectors by their XLX name, as the dashboard reports it
	m_Name = "XLX" + g_Configure.GetString(g_Keys.names.callsign).substr(3, 3);
	m_Url = g_Configure.GetString(g_Keys.names.url);
	// everything below Enable is optional
	auto optional = [](const std::string &key) { return g_Configure.Contains(key) ? g_Configure.GetString(key) : std::string(); };
	m_Country = optional(g_Keys.callinghome.country);
	m_Comment = optional(g_Keys.callinghome.comment);
	m_OverrideIP = optional(g_Keys.callinghome.overrideip);
	m_Server = optional(g_Keys.callinghome.server);
	if (m_Server.empty())
		m_Server = CALLINGHOME_SERVER;
	if (g_Configure.Contains(g_Keys.callinghome.pushdelay))
		m_PushDelay = g_Configure.GetUnsigned(g_Keys.callinghome.pushdelay);
	if (g_Configure.Contains(g_Keys.callinghome.timeout))
		m_Timeout = g_Configure.GetUnsigned(g_Keys.callinghome.timeout);
	m_Hash = optional(g_Keys.callinghome.hash);
	m_Reached = false;
	if (m_Hash.empty() && CreateHash())
		return;

	m_StartTime = std::chrono::steady_clock::now();
	m_Run = true;
	try
	{
		m_Future = std::async(std::launch::async, &CCallingHome::Thread, this);
	}
	catch (const std::exception &e)
	{
		m_Run = false;
		std::cerr << "CallingHome: cannot start its thread: " << e.what() << std::endl;
	}
}

void CCallingHome::Stop(void)
{
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Run = false;
	}
	m_Wake.notify_all();
	if (m_Future.valid())
		m_Future.get();
}

// The directory ties the name to the first hash it sees, so a new hash is only
// used once it is saved as [CallingHome] Hash in urfd.ini, where the next start
// finds it again.
bool CCallingHome::CreateHash(void)
{
	// same alphabet and length as the dashboard's CreateCode(16)
	static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	std::random_device rd;
	std::uniform_int_distribution<size_t> pick(0, sizeof(chars) - 2);
	std::string hash;
	for (int i = 0; i < 16; i++)
		hash += chars[pick(rd)];

	const std::string &ini = g_Configure.GetPath();
	if (SaveHash(ini, hash))
	{
		std::cerr << "CallingHome: cannot save a new hash in " << ini << ", not registering (set [CallingHome] Hash yourself)" << std::endl;
		return true;
	}
	m_Hash = hash;
	std::cout << "CallingHome: made a new hash, saved as [CallingHome] Hash in " << ini << std::endl;
	return false;
}

// Puts "Hash = ..." into [CallingHome], over a blank or commented-out Hash line
// if there is one, and leaves every other line as it is. Written to a temporary
// file renamed over the ini, so a crash never leaves it half written; a single
// file bind mount can't be renamed over, that one is rewritten in place.
bool CCallingHome::SaveHash(const std::string &ini, const std::string &hash)
{
	std::vector<std::string> lines;
	{
		std::ifstream in(ini);
		if (!in)
			return true;
		std::string line;
		while (std::getline(in, line))
			lines.push_back(line);
	}

	auto trimmed = [](const std::string &s) {
		const auto b = s.find_first_not_of(" \t\r");
		return (std::string::npos == b) ? std::string() : s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
	};
	const std::regex active("^\\s*Hash\\s*=.*"), commented("^\\s*#\\s*Hash\\s*=.*");
	size_t start = lines.size();
	for (size_t i = 0; i < lines.size(); i++)
	{
		if ("[CallingHome]" == trimmed(lines[i]))
		{
			start = i;
			break;
		}
	}
	if (start == lines.size())
		return true;	// can't happen, Enable = true was read from it
	size_t end = start + 1;
	while (end < lines.size() && '[' != trimmed(lines[end]).c_str()[0])
		end++;
	size_t hit = end;
	for (size_t i = start + 1; i < end && hit == end; i++)
		if (std::regex_match(lines[i], active))
			hit = i;
	for (size_t i = start + 1; i < end && hit == end; i++)
		if (std::regex_match(lines[i], commented))
			hit = i;
	const std::string entry = "Hash = " + hash;
	if (hit == end)
		lines.insert(lines.begin() + start + 1, entry);
	else
		lines[hit] = entry;

	std::stringstream content;
	for (const auto &line : lines)
		content << line << '\n';

	struct stat st;
	const mode_t mode = (0 == stat(ini.c_str(), &st)) ? (st.st_mode & 07777) : 0600;
	const std::string tmp = ini + ".tmp";
	{
		std::ofstream out(tmp, std::ios::trunc);
		out << content.str();
		out.flush();
		if (!out)
		{
			std::remove(tmp.c_str());
			return true;
		}
	}
	chmod(tmp.c_str(), mode);
	if (0 == std::rename(tmp.c_str(), ini.c_str()))
		return false;
	std::remove(tmp.c_str());

	std::ofstream out(ini, std::ios::trunc);
	out << content.str();
	out.flush();
	return !out;
}

std::string CCallingHome::BuildXml(void) const
{
	const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_StartTime).count();
	std::stringstream version;
	version << g_Version;

	std::stringstream xml;
	xml << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<query>CallingHome</query>\n<reflector>\n"
	    << "   <name>" << m_Name << "</name>\n"
	    << "   <uptime>" << uptime << "</uptime>\n"
	    << "   <hash>" << m_Hash << "</hash>\n"
	    << "   <url>" << CUtils::XmlEscape(m_Url) << "</url>\n"
	    << "   <country>" << CUtils::XmlEscape(m_Country) << "</country>\n"
	    << "   <comment>" << CUtils::XmlEscape(m_Comment) << "</comment>\n"
	    << "   <ip>" << CUtils::XmlEscape(m_OverrideIP) << "</ip>\n"
	    << "   <reflectorversion>" << version.str() << "</reflectorversion>\n"
	    << "</reflector>\n<interlinks>";

	auto ilmap = g_GateKeeper.GetInterlinkMap();
	for (auto it = ilmap->begin(); it != ilmap->end(); it++)
	{
		std::string address;
#ifndef NO_DHT
		if (it->second.UsesDHT())
			address = it->second.GetIPv4();
		else
#endif
			address = it->second.GetIp().GetAddress();
		xml << "\n   <interlink>\n"
		    << "      <name>" << CUtils::XmlEscape(it->first) << "</name>\n"
		    << "      <address>" << address << "</address>\n"
		    << "      <modules>" << it->second.GetModules() << "</modules>\n"
		    << "   </interlink>";
	}
	g_GateKeeper.ReleaseInterlinkMap();
	xml << "\n</interlinks>";
	return xml.str();
}

// true when the directory took it
bool CCallingHome::Push(void)
{
	CCurlGet curl;
	std::stringstream answer;
	long http = 0;
	const CURLcode code = curl.PostForm(m_Server, "xml", BuildXml(), answer, http, m_Timeout);
	if (CURLE_OK != code || http < 200 || http >= 300)
	{
		std::cerr << "CallingHome: " << m_Server << " did not take the update ("
		          << ((CURLE_OK != code) ? curl_easy_strerror(code) : "HTTP " + std::to_string(http)) << ")" << std::endl;
		return false;
	}
	return true;
}

void CCallingHome::Thread(void)
{
	while (m_Run)
	{
		const bool reached = Push();
		if (reached && !m_Reached)
			std::cout << "CallingHome: registered as " << m_Name << " in the XLX directory, updating every " << m_PushDelay << " s" << std::endl;
		m_Reached = reached;

		std::unique_lock<std::mutex> lock(m_Mutex);
		m_Wake.wait_for(lock, std::chrono::seconds(m_PushDelay), [this] { return !m_Run || m_PushNow; });
		m_PushNow = false;
	}
}

void CCallingHome::PushNow(void)
{
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_PushNow = true;
	}
	m_Wake.notify_all();
}
