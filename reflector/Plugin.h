#pragma once

// Plugin -- a protocol urfd loads from a shared object at start
//
// urfd.ini names the shared objects in [Plugins]:
//
//   [Plugins]
//   Load = /usr/local/lib/urfd/myproto.so
//   Load = ...                               (as many as needed)
//
// A plugin is C++ built against this very source tree: it derives its protocol
// from CProtocol and its clients from CClient, and it uses g_Reflector,
// g_Configure, g_GateKeeper and the other globals the urfd executable exports
// (it links with -rdynamic). So it fits only the urfd it was built with: urfd
// compares the version and the layout of its main classes (SPluginAbi) and
// refuses a plugin that does not match.
//
// A plugin has a name, e.g. "Foo". Its settings are the ini section [Foo],
// "Foo" names it in BlockProtocols and in the block commands of the admin
// socket, and the dashboard lists it under that name. Its protocol gets an
// EProtocol of its own, PluginProtocolId(index) (see Plugins.h), which is what
// its clients return from GetProtocol().
//
// A shared object exports the two functions URFD_PLUGIN(CFooPlugin) defines.
// Each plugin is a CPlugin; urfd keeps the shared objects loaded until it ends.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

#include "Version.h"
#include "Configure.h"	// CIniSection

class CProtocol;

class CPlugin
{
public:
	virtual ~CPlugin() {}

	// the name: ini section, BlockProtocols, admin socket and dashboard
	virtual const char *GetName(void) const = 0;
	// one line for the protocol list of the dashboard, e.g. "Foo radios"
	virtual const char *GetDescription(void) const { return ""; }

	// reads the plugin's ini section (empty when urfd.ini has none) into cfg,
	// under keys of its own (CConfigure::Set), and checks it; true on an error,
	// as CConfigure::ReadData. Called by every ReadData, a reload included.
	virtual bool Configure(CConfigure &cfg, const CIniSection &section) = 0;
	// the section switches it on
	virtual bool IsEnabled(void) const = 0;

	// the protocol, made once at start when it is enabled; urfd initializes
	// it with GetType(), its EProtocol, GetPort(), HasIPv4() and HasIPv6()
	virtual std::unique_ptr<CProtocol> CreateProtocol(void) = 0;
	virtual const char *GetType(void) const { return nullptr; }	// CProtocol::Initialize's type, and the D-Star suffix of its callers ("HYT")
	virtual uint16_t GetPort(void) const = 0;	// also in the XML file and for the dashboard, 0 = none
	virtual bool HasIPv4(void) const { return true; }
	virtual bool HasIPv6(void) const { return false; }

	// the configuration was read again (SIGHUP); proto is nullptr when the protocol does not run
	virtual void Reload(CProtocol *proto) { (void)proto; }

	// admin socket, from its thread; proto is nullptr when the protocol does not run.
	// A command of its own: true when it is one, result is the answer
	virtual bool AdminCommand(CProtocol *proto, const std::string &command, const nlohmann::json &cmd, nlohmann::json &result)
	{ (void)proto; (void)command; (void)cmd; (void)result; return false; }
	// fields of its own in the answer to "status"
	virtual void AdminStatus(CProtocol *proto, nlohmann::json &status) { (void)proto; (void)status; }
	// its modules, appended to the "mappings" of tg_list (protocol = the name in lower case)
	virtual void AdminMappings(CProtocol *proto, nlohmann::json &mappings) { (void)proto; (void)mappings; }
	// its sections on the admin page of the dashboard, which draws them from this
	// description alone: tables filled by a command, forms and remove buttons that
	// call one (see docs/PLUGINS.md); an empty array for none
	virtual nlohmann::json AdminUi(void) const { return nlohmann::json::array(); }
};

////////////////////////////////////////////////////////////////////////////////////////
// what a plugin and urfd must agree on

#define URFD_PLUGIN_API 1

struct SPluginAbi
{
	unsigned api;           // URFD_PLUGIN_API
	int version;            // CVersion::GetVersion() of the urfd source
	unsigned flags;         // build options that change class layouts
	size_t sizes[9];        // of the main classes
};

#define URFD_ABI_NO_DHT  0x01u
#define URFD_ABI_DEBUG   0x02u

#ifdef NO_DHT
#define URFD_ABI_FLAG_DHT URFD_ABI_NO_DHT
#else
#define URFD_ABI_FLAG_DHT 0u
#endif
#ifdef DEBUG
#define URFD_ABI_FLAG_DEBUG URFD_ABI_DEBUG
#else
#define URFD_ABI_FLAG_DEBUG 0u
#endif

// the layout as the including source sees it; needs the headers of the classes,
// so it is used in Plugins.cpp and in URFD_PLUGIN only (Reflector.h, Global.h)
#define URFD_PLUGIN_ABI_VALUE(ver) SPluginAbi{ URFD_PLUGIN_API, (ver), URFD_ABI_FLAG_DHT | URFD_ABI_FLAG_DEBUG, \
	{ sizeof(CReflector), sizeof(CProtocol), sizeof(CClient), sizeof(CConfigure), \
	  sizeof(CPacket), sizeof(CDvHeaderPacket), sizeof(CDvFramePacket), sizeof(CPacketStream), sizeof(SJsonKeys) } }

// the urfd version of the source tree (Version.h), as CVersion::GetVersion()
#define URFD_SOURCE_VERSION (0x10000 * URFD_VERSION_MAJOR + 0x100 * URFD_VERSION_MINOR + URFD_VERSION_REVISION)

// in the plugin's source, once: URFD_PLUGIN(CFooPlugin); it needs Reflector.h and Configure.h
#define URFD_PLUGIN(cls) \
	extern "C" const SPluginAbi *urfd_plugin_abi(void) \
	{ static const SPluginAbi abi = URFD_PLUGIN_ABI_VALUE(URFD_SOURCE_VERSION); return &abi; } \
	extern "C" CPlugin *urfd_plugin_create(void) { return new cls; }
