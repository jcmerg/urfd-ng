#pragma once

// MmdvmClientConfig -- the keys of the [MMDVMClient] sections and the network
// of a connection, shared by the configuration reader and the protocol
//
// urfd connects to up to MMDVMCLI_MAX_INSTANCES DMR masters at once:
// [MMDVMClient] (instance 1, with the keys it always had, "mmdvmcliEnable")
// and further connections, either named, [MMDVMClient.<name>] (4.27.0), or
// numbered, [MMDVMClient2] to [MMDVMClient9] ("mmdvmcli2Enable", ...); the named
// ones get the instances 2, 3, ... in the order of the file, a file uses one of
// the two ways. Each has its own hotspot ID, two timeslots and TG map. A named
// connection takes what it leaves out from [MMDVMClient], and a DmrId of one or
// two digits (or a name of digits without a DmrId) is a suffix to the DmrId of
// [MMDVMClient]: [MMDVMClient.50] is hotspot <DmrId>50. [MMDVMClient] without
// TGs of its own then only gives the shared settings.
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

// the name of a numbered section and connection: "MMDVMClient", "MMDVMClient2", ...
inline std::string MmdvmClientDefaultName(unsigned n)
{
	return (n > 1) ? "MMDVMClient" + std::to_string(n) : std::string("MMDVMClient");
}

// where the configuration keeps the name of a named connection ("MMDVMClient.50")
inline std::string MmdvmClientNameKey(unsigned n)
{
	return MmdvmClientPrefix(n) + "Name";
}

// in urfd (MMDVMClientProtocol.cpp, from g_Configure): the name of instance n, its
// section's name for a named one, else MmdvmClientDefaultName(n)
std::string MmdvmClientName(unsigned n);

// 1 for "MMDVMClient", 2 to MMDVMCLI_MAX_INSTANCES for "MMDVMClient2" ..., else 0
inline unsigned MmdvmClientInstance(const std::string &name)
{
	if (name == "MMDVMClient")
		return 1;
	if (name.size() == 12 && name.compare(0, 11, "MMDVMClient") == 0 && name[11] >= '2' && name[11] <= '0' + MMDVMCLI_MAX_INSTANCES)
		return unsigned(name[11] - '0');
	return 0;
}

// in urfd (MMDVMClientProtocol.cpp): 1 to MMDVMCLI_MAX_INSTANCES for "MMDVMClient1" ...
// "MMDVMClient9" or the name of a named connection ("MMDVMClient.50"), the name of
// one connection in BlockProtocols and the admin commands, else 0; there plain
// "MMDVMClient" means every connection
unsigned MmdvmClientConnection(const std::string &name);

inline SJsonKeys::MMDVMCLIENT MmdvmClientKeys(unsigned n)
{
	const std::string p = MmdvmClientPrefix(n);
	return { p + "Enable", p + "Address", p + "Port", p + "LocalPort", p + "DmrId", p + "Password", p + "Callsign",
		p + "Latitude", p + "Longitude", p + "Location", p + "Description", p + "Url", p + "RxFreq", p + "TxFreq",
		p + "Software", p + "Firmware", p + "FallbackDmrId", p + "BlockProtocols", p + "BmApiKey", p + "Network" };
}

// the network a connection belongs to: its Network key, else "brandmeister.network"
// for every BrandMeister master (2622.master.brandmeister.network, ...), else the
// master's host name or address itself. Two masters of another network, or a
// BrandMeister master given by its IP address, need the Network key: a shared
// domain alone (*.ddns.net) does not make a network.
inline std::string MmdvmClientNetwork(const std::string &configured, const std::string &address)
{
	auto lower = [](std::string s) { for (auto &c : s) c = char(std::tolower((unsigned char)c)); return s; };
	if (!configured.empty())
		return lower(configured);
	const std::string a = lower(address);
	static const std::string bm = "brandmeister.network";
	if (a == bm || (a.size() > bm.size() && a.compare(a.size() - bm.size() - 1, bm.size() + 1, "." + bm) == 0))
		return bm;
	return a;
}
