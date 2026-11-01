#pragma once

// ExternalIp -- the address this host has on the internet, as web services see it
//
// Behind NAT the bound address is not the one clients reach. urfd asks a list
// of services (IPCheckUrl, the first that answers with a well-formed address
// wins) at start and, with IPCheckInterval, again while it runs, so a changed
// address (another uplink, a fallback line) reaches the directory, the DHT, G3
// and the plugins. A configured IPv4External / IPv6External is never replaced.
//
// Copyright (C) 2026 Jens-Christian Merg (DL4JC)
// Licensed under the GNU General Public License v3 or later

#include <functional>
#include <string>
#include <vector>

// the services asked when IPCheckUrl is not set: each answers with the
// caller's address as plain text, asked over IPv4 or IPv6 as needed
#define EXTERNAL_IP_URLS "https://icanhazip.com, https://ifconfig.me/ip, https://api64.ipify.org, https://ident.me"

// the URLs of a comma separated list, the defaults for an empty one
std::vector<std::string> ExternalIpUrls(const std::string &list);

// the external IPv4 (v6 false) or IPv6 address, from the first service that
// answers with a well-formed one; empty when none does. stop, when given, is
// asked before each service: true ends the search (urfd stops)
std::string DetectExternalIp(bool v6, const std::vector<std::string> &urls, const std::function<bool()> &stop = nullptr);
