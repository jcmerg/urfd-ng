#pragma once

// ProtocolNames -- the protocol names of BlockProtocols in urfd.ini and of the
// admin socket's block commands, one table for all of them; a plugin's protocol
// joins it when the plugin is loaded (Plugins.cpp), before any protocol starts
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <map>
#include <string>
#include "Defines.h"

inline std::map<std::string, EProtocol> &ProtocolNameTable(void)
{
	static std::map<std::string, EProtocol> names = {
		// every MMDVMClient connection; "MMDVMClient1" ... "MMDVMClient9" name one of
		// them (MmdvmClientConnection in MmdvmClientConfig.h)
		{"MMDVMClient", EProtocol::mmdvmclient}, {"SvxReflector", EProtocol::svxreflector},
		{"SVX", EProtocol::svx}, {"SVXServer", EProtocol::svx},
		{"DCSClient", EProtocol::dcsclient}, {"DExtraClient", EProtocol::dextraclient},
		{"DPlusClient", EProtocol::dplusclient}, {"YSFClient", EProtocol::ysfclient},
		{"DExtra", EProtocol::dextra}, {"DPlus", EProtocol::dplus},
		{"DCS", EProtocol::dcs}, {"DMRPlus", EProtocol::dmrplus},
		{"MMDVM", EProtocol::dmrmmdvm}, {"YSF", EProtocol::ysf},
		{"M17", EProtocol::m17}, {"NXDN", EProtocol::nxdn},
		{"P25", EProtocol::p25}, {"USRP", EProtocol::usrp},
		{"URF", EProtocol::urf}, {"XLXPeer", EProtocol::xlxpeer},
		{"G3", EProtocol::g3},
	};
	return names;
}

inline const std::map<std::string, EProtocol> &ProtocolNames(void)
{
	return ProtocolNameTable();
}

inline void RegisterProtocolName(const std::string &name, EProtocol p)
{
	ProtocolNameTable()[name] = p;
}
