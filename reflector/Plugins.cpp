// Plugins -- the shared objects of [Plugins] in urfd.ini
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <dlfcn.h>
#include <iostream>
#include <set>

#include "Plugins.h"
#include "ProtocolNames.h"
#include "Reflector.h"
#include "Global.h"

CPlugins g_Plugins;

bool CPlugins::Load(const std::string &path)
{
	// RTLD_LOCAL: two plugins may have classes of the same name
	void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!handle)
	{
		std::cerr << "ERROR: [Plugins] " << path << ": " << dlerror() << std::endl;
		return true;
	}
	using AbiFunc = const SPluginAbi *(*)(void);
	using CreateFunc = CPlugin *(*)(void);
	auto abiFunc = reinterpret_cast<AbiFunc>(dlsym(handle, "urfd_plugin_abi"));
	auto createFunc = reinterpret_cast<CreateFunc>(dlsym(handle, "urfd_plugin_create"));
	if (!abiFunc || !createFunc)
	{
		std::cerr << "ERROR: [Plugins] " << path << " is no urfd plugin (URFD_PLUGIN is missing)" << std::endl;
		dlclose(handle);
		return true;
	}

	// a plugin built from other headers would work on objects it does not know
	const SPluginAbi ours = URFD_PLUGIN_ABI_VALUE(g_Version.GetVersion());
	const SPluginAbi &theirs = *abiFunc();
	if (ours.api != theirs.api)
	{
		std::cerr << "ERROR: [Plugins] " << path << " was built for plugin API " << theirs.api << ", this urfd has " << ours.api << ", build it again against this source tree" << std::endl;
		dlclose(handle);
		return true;
	}
	bool sizes = true;
	for (size_t i = 0; i < sizeof(ours.sizes) / sizeof(ours.sizes[0]); i++)
		sizes = sizes && (ours.sizes[i] == theirs.sizes[i]);
	if (ours.version != theirs.version || ours.flags != theirs.flags || !sizes)
	{
		std::cerr << "ERROR: [Plugins] " << path << " was built for another urfd (version " << (theirs.version >> 16) << '.' << (theirs.version >> 8 & 0xff) << '.' << (theirs.version & 0xff)
			<< (ours.flags != theirs.flags ? ", other build options" : "") << (sizes ? "" : ", other class layouts")
			<< "), build it again against this source tree" << std::endl;
		dlclose(handle);
		return true;
	}

	std::unique_ptr<CPlugin> plugin(createFunc());
	if (!plugin)
	{
		std::cerr << "ERROR: [Plugins] " << path << " made no plugin" << std::endl;
		dlclose(handle);
		return true;
	}
	const std::string name(plugin->GetName() ? plugin->GetName() : "");
	if (name.empty() || name.find_first_of(" []=,") != std::string::npos)
	{
		std::cerr << "ERROR: [Plugins] " << path << ": the plugin's name '" << name << "' is no section name" << std::endl;
		plugin.reset();
		dlclose(handle);
		return true;
	}
	if (Find(name) || ProtocolNames().count(name))
	{
		std::cerr << "ERROR: [Plugins] " << path << ": there is a protocol named " << name << " already" << std::endl;
		plugin.reset();
		dlclose(handle);
		return true;
	}

	// from now on it is a protocol name like the built in ones
	const EProtocol id = PluginProtocolId(unsigned(m_Plugins.size()));
	RegisterProtocolName(name, id);
	std::cout << "Plugin " << name << " loaded from " << path << std::endl;
	m_Plugins.push_back({ path, handle, std::move(plugin) });
	return false;
}

bool CPlugins::OnReadData(CConfigure &cfg, const CIniSections &sections)
{
	bool rval = false;
	const auto &data = cfg.GetData();
	const std::string key(g_Keys.plugins.load);

	// the shared objects only once, at the start; they stay loaded
	if (!m_Loaded)
	{
		m_Loaded = true;
		if (data.contains(key))
			for (const auto &path : data[key])
				if (Load(path.get<std::string>()))
					rval = true;
	}
	else if (data.contains(key))
	{
		std::set<std::string> loaded;
		for (const auto &p : m_Plugins)
			loaded.insert(p.path);
		for (const auto &path : data[key])
			if (!loaded.count(path.get<std::string>()))
				std::cout << "NOTE: [Plugins] " << path.get<std::string>() << " is loaded at the next start, not by a reload" << std::endl;
	}

	// each plugin its section, an empty one when there is none
	static const CIniSection none;
	for (auto &p : m_Plugins)
	{
		const auto it = sections.find(p.plugin->GetName());
		if (p.plugin->Configure(cfg, it == sections.end() ? none : it->second))
			rval = true;
	}
	for (const auto &sec : sections)
		if (!Find(sec.first))
			std::cerr << "WARNING: unknown ini file section: [" << sec.first << "]" << std::endl;
	return rval;
}

CPlugin *CPlugins::Find(EProtocol p) const
{
	if (!IsPluginProtocol(p))
		return nullptr;
	const size_t i = size_t(int(p) - int(EProtocol::plugin));
	return i < m_Plugins.size() ? m_Plugins[i].plugin.get() : nullptr;
}

CPlugin *CPlugins::Find(const std::string &name) const
{
	for (const auto &p : m_Plugins)
		if (name == p.plugin->GetName())
			return p.plugin.get();
	return nullptr;
}

std::string CPlugins::GetName(EProtocol p) const
{
	const auto *plugin = Find(p);
	return plugin ? plugin->GetName() : std::string();
}
