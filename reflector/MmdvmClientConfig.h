#pragma once

// MmdvmClientConfig -- the keys of the [MMDVMClient] sections and the network
// of a connection, shared by the configuration reader and the protocol
//
// urfd connects to up to MMDVMCLI_MAX_INSTANCES DMR masters at once:
// [MMDVMClient] (instance 1, with the keys it always had, "mmdvmcliEnable")
// and [MMDVMClient2] to [MMDVMClient9] ("mmdvmcli2Enable", ...). Each has its
// own hotspot ID, two timeslots and TG map.
//
// Connections in the same network (two hotspots on BrandMeister) never pass a
// call to each other, the network would get it twice. Connections in different
// networks (BrandMeister and TGIF) bridge a module both of them map, unless
// BlockProtocols says otherwise.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <string>
#include <cctype>
#include "JsonKeys.h"

#define MMDVMCLI_MAX_INSTANCES 9

inline std::string MmdvmClientPrefix(unsigned n)
{
	return (n > 1) ? "mmdvmcli" + std::to_string(n) : std::string("mmdvmcli");
}

// the section and protocol name: "MMDVMClient", "MMDVMClient2", ...
inline std::string MmdvmClientName(unsigned n)
{
	return (n > 1) ? "MMDVMClient" + std::to_string(n) : std::string("MMDVMClient");
}

// 1 for "MMDVMClient", 2 to MMDVMCLI_MAX_INSTANCES for "MMDVMClient2" ..., else 0
inline unsigned MmdvmClientInstance(const std::string &name)
{
	if (name == "MMDVMClient")
		return 1;
	if (name.size() == 12 && name.compare(0, 11, "MMDVMClient") == 0 && name[11] >= '2' && name[11] <= '0' + MMDVMCLI_MAX_INSTANCES)
		return unsigned(name[11] - '0');
	return 0;
}

inline SJsonKeys::MMDVMCLIENT MmdvmClientKeys(unsigned n)
{
	const std::string p = MmdvmClientPrefix(n);
	return { p + "Enable", p + "Address", p + "Port", p + "LocalPort", p + "DmrId", p + "Password", p + "Callsign",
		p + "Latitude", p + "Longitude", p + "Location", p + "Description", p + "Url", p + "RxFreq", p + "TxFreq",
		p + "Software", p + "Firmware", p + "FallbackDmrId", p + "BlockProtocols", p + "BmApiKey", p + "Network" };
}

// the network a connection belongs to: its Network key, else the master's
// domain, so 2622.master.brandmeister.network and 2621.master.brandmeister.network
// are both "brandmeister.network"; an IP address is a network of its own
inline std::string MmdvmClientNetwork(const std::string &configured, const std::string &address)
{
	auto lower = [](std::string s) { for (auto &c : s) c = char(std::tolower((unsigned char)c)); return s; };
	if (!configured.empty())
		return lower(configured);
	const std::string a = lower(address);
	if (a.find_first_not_of("0123456789.") == std::string::npos || a.find(':') != std::string::npos)
		return a;
	const auto last = a.rfind('.');
	if (last == std::string::npos || last == 0)
		return a;
	const auto prev = a.rfind('.', last - 1);
	return (prev == std::string::npos) ? a : a.substr(prev + 1);
}
