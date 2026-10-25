#pragma once

// Plugins -- the shared objects of [Plugins] in urfd.ini, see Plugin.h
//
// Loaded by the first CConfigure::ReadData (its PluginHook), before the
// protocols start; a reload (SIGHUP) hands them their sections again, but
// loads no new shared object. Nothing here changes after the start, so the
// threads read it without a lock.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <memory>
#include <string>
#include <vector>

#include "Defines.h"
#include "Plugin.h"

class CPlugins
{
public:
	// Main sets CConfigure::PluginHook to this
	bool OnReadData(CConfigure &cfg, const CIniSections &sections);

	size_t Size(void) const { return m_Plugins.size(); }
	CPlugin &Get(size_t i) const { return *m_Plugins[i].plugin; }
	EProtocol GetId(size_t i) const { return PluginProtocolId(unsigned(i)); }

	// nullptr when p is no plugin's protocol
	CPlugin *Find(EProtocol p) const;
	// by its name, as in the ini file; nullptr when there is none
	CPlugin *Find(const std::string &name) const;
	// the name of a plugin protocol, empty when there is none
	std::string GetName(EProtocol p) const;

private:
	bool Load(const std::string &path);

	struct SLoaded
	{
		std::string path;
		void *handle;
		std::unique_ptr<CPlugin> plugin;
	};
	std::vector<SLoaded> m_Plugins;
	bool m_Loaded = false;
};

extern CPlugins g_Plugins;
