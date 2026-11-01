// ExternalIp -- the address this host has on the internet, as web services see it
//
// Copyright (C) 2026 Jens-Christian Merg (DL4JC)
// Licensed under the GNU General Public License v3 or later

#include <arpa/inet.h>
#include <sstream>

#include "CurlGet.h"
#include "ExternalIp.h"

static std::string Trim(const std::string &s)
{
	const auto b = s.find_first_not_of(" \t\r\n");
	if (std::string::npos == b)
		return std::string();
	return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::vector<std::string> ExternalIpUrls(const std::string &list)
{
	std::vector<std::string> urls;
	std::stringstream ss(list.empty() ? std::string(EXTERNAL_IP_URLS) : list);
	std::string url;
	while (std::getline(ss, url, ','))
	{
		url = Trim(url);
		if (!url.empty())
			urls.push_back(url);
	}
	return urls;
}

std::string DetectExternalIp(bool v6, const std::vector<std::string> &urls, const std::function<bool()> &stop)
{
	for (const auto &url : urls)
	{
		if (stop && stop())
			break;
		CCurlGet curl;
		std::stringstream ss;
		// short: with the uplink down every service times out
		if (CURLE_OK != curl.GetURL(url, ss, 5, 3, v6 ? CURL_IPRESOLVE_V6 : CURL_IPRESOLVE_V4))
			continue;
		const std::string ip = Trim(ss.str());
		unsigned char buf[16];
		// a service that answers with something else (an error page) is passed over
		if (1 == inet_pton(v6 ? AF_INET6 : AF_INET, ip.c_str(), buf))
			return ip;
	}
	return std::string();
}
