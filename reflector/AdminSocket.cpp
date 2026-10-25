// AdminSocket -- TCP-based JSON admin interface for runtime management

#include <iostream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <random>
#include <iomanip>

#include "Global.h"
#include "ProtocolNames.h"
#include "AdminSocket.h"
#include "LogBuffer.h"
#include "MMDVMClientProtocol.h"
#include "DMRMMDVMProtocol.h"
#include "SvxReflectorProtocol.h"
#include "SvxProtocol.h"
#include "Plugins.h"
#include "DCSClientProtocol.h"
#include "DExtraClientProtocol.h"
#include "DPlusClientProtocol.h"
#include "YSFClientProtocol.h"

#define ADMIN_TOKEN_TTL_SECONDS   3600   // 1 hour session
#define ADMIN_MAX_MSG_SIZE        8192
#define ADMIN_AUTH_FAIL_DELAY     2      // seconds without logins after a wrong password

static const auto &s_NameToProto = ProtocolNames();

// "mmdvmclient" or "mmdvmclient2" ... "mmdvmclient9"
static bool IsMmdvmClientProtocol(const std::string &p)
{
	return p.compare(0, 11, "mmdvmclient") == 0 && (p.size() == 11 || (p.size() == 12 && p[11] >= '2' && p[11] <= '0' + MMDVMCLI_MAX_INSTANCES));
}

// the MMDVMClient connection a command means: the number in the protocol
// ("mmdvmclient2"), else "client" (1 to 9 or "MMDVMClient2"), else the one
// with the TG if tg is given, else [MMDVMClient] or the first one.
// Call with the protocols lock held.
static CMMDVMClientProtocol *FindMmdvmClient(CProtocols &protocols, const std::string &protocol, const nlohmann::json &cmd, uint32_t tg = 0, std::string *err = nullptr)
{
	unsigned n = 0;
	if (protocol.size() == 12)
		n = unsigned(protocol[11] - '0');
	else if (cmd.contains("client"))
	{
		const auto &c = cmd["client"];
		if (c.is_number_integer() && c.get<int>() > 0)
			n = c.get<unsigned>();
		else if (c.is_string())
			n = MmdvmClientInstance(c.get<std::string>());
		if (!n)
			return nullptr;
	}
	if (n)
		return protocols.FindMmdvmClient(n);
	const auto &all = protocols.MmdvmClients();
	if (tg)
	{
		// the connection with the TG; with two of them (two networks) it is not ours to pick
		CMMDVMClientProtocol *found = nullptr;
		for (auto *c : all)
			if (c->GetTGMap().IsTGMapped(tg))
			{
				if (found)
				{
					if (err)
						*err = "TG" + std::to_string(tg) + " is mapped by " + found->GetName() + " and " + c->GetName() + ", give \"client\"";
					return nullptr;
				}
				found = c;
			}
		if (found)
			return found;
	}
	if (auto *c = protocols.FindMmdvmClient(1))
		return c;
	return all.empty() ? nullptr : all.front();
}

static const std::map<EProtocol, std::string> s_BuiltinProtoToName = {
	{EProtocol::mmdvmclient, "MMDVMClient"}, {EProtocol::svxreflector, "SvxReflector"}, {EProtocol::svx, "SVX"},
	{EProtocol::dcsclient, "DCSClient"}, {EProtocol::dextraclient, "DExtraClient"},
	{EProtocol::dplusclient, "DPlusClient"}, {EProtocol::ysfclient, "YSFClient"},
	{EProtocol::dextra, "DExtra"}, {EProtocol::dplus, "DPlus"},
	{EProtocol::dcs, "DCS"}, {EProtocol::dmrplus, "DMRPlus"},
	{EProtocol::dmrmmdvm, "MMDVM"}, {EProtocol::ysf, "YSF"},
	{EProtocol::m17, "M17"}, {EProtocol::nxdn, "NXDN"},
	{EProtocol::p25, "P25"}, {EProtocol::usrp, "USRP"},
	{EProtocol::urf, "URF"}, {EProtocol::xlxpeer, "XLXPeer"},
	{EProtocol::g3, "G3"},
};

// the built in protocols and those of the plugins, which are loaded before the admin socket starts
static const std::map<EProtocol, std::string> &ProtoToName(void)
{
	static const std::map<EProtocol, std::string> names = [] {
		auto m = s_BuiltinProtoToName;
		for (size_t i = 0; i < g_Plugins.Size(); i++)
			m[g_Plugins.GetId(i)] = g_Plugins.Get(i).GetName();
		return m;
	}();
	return names;
}

// a plugin's protocol, nullptr when it does not run
static CProtocol *FindPluginProtocol(EProtocol id)
{
	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(id);
	protocols.Unlock();
	return proto;
}

static std::string Lower(std::string s)
{
	for (auto &c : s)
		c = char(std::tolower((unsigned char)c));
	return s;
}

CAdminSocket::CAdminSocket()
	: m_ListenFd(-1)
	, m_Port(10101)
	, m_Running(false)
{
}

CAdminSocket::~CAdminSocket()
{
	Stop();
}

bool CAdminSocket::Start(void)
{
	if (!g_Configure.Contains(g_Keys.admin.enable) || !g_Configure.GetBoolean(g_Keys.admin.enable))
	{
		std::cout << "Admin: disabled" << std::endl;
		return true;
	}

	m_Port = (uint16_t)g_Configure.GetUnsigned(g_Keys.admin.port);
	m_Password = g_Configure.GetString(g_Keys.admin.password);

	m_BindAddress = "127.0.0.1";
	if (g_Configure.Contains(g_Keys.admin.bindaddress))
		m_BindAddress = g_Configure.GetString(g_Keys.admin.bindaddress);

	// Create listening socket
	m_ListenFd = socket(AF_INET, SOCK_STREAM, 0);
	if (m_ListenFd < 0)
	{
		std::cerr << "Admin: failed to create socket: " << strerror(errno) << std::endl;
		return false;
	}

	int opt = 1;
	setsockopt(m_ListenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(m_Port);
	inet_pton(AF_INET, m_BindAddress.c_str(), &addr.sin_addr);

	if (bind(m_ListenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		std::cerr << "Admin: bind failed on " << m_BindAddress << ":" << m_Port << ": " << strerror(errno) << std::endl;
		close(m_ListenFd);
		m_ListenFd = -1;
		return false;
	}

	if (listen(m_ListenFd, 4) < 0)
	{
		std::cerr << "Admin: listen failed: " << strerror(errno) << std::endl;
		close(m_ListenFd);
		m_ListenFd = -1;
		return false;
	}

	// Runs in its own thread — does not block reflector audio processing
	m_Running = true;
	m_ListenFuture = std::async(std::launch::async, &CAdminSocket::ListenThread, this);

	std::cout << "Admin: listening on " << m_BindAddress << ":" << m_Port << std::endl;
	return true;
}

void CAdminSocket::Stop(void)
{
	m_Running = false;
	if (m_ListenFd >= 0)
	{
		shutdown(m_ListenFd, SHUT_RDWR);
		close(m_ListenFd);
		m_ListenFd = -1;
	}
	if (m_ListenFuture.valid())
		m_ListenFuture.get();
}

void CAdminSocket::ListenThread(void)
{
	while (m_Running)
	{
		struct pollfd pfd{};
		pfd.fd = m_ListenFd;
		pfd.events = POLLIN;

		int ret = poll(&pfd, 1, 1000);
		if (ret <= 0)
			continue;

		struct sockaddr_in clientAddr{};
		socklen_t addrLen = sizeof(clientAddr);
		int clientFd = accept(m_ListenFd, (struct sockaddr *)&clientAddr, &addrLen);
		if (clientFd < 0)
			continue;

		char addrStr[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &clientAddr.sin_addr, addrStr, sizeof(addrStr));
		std::string addr(addrStr);

		// Handle client in a detached thread so the listen loop stays responsive
		std::thread(&CAdminSocket::ClientThread, this, clientFd, addr).detach();
	}
}

void CAdminSocket::ClientThread(int clientFd, const std::string &clientAddr)
{
	struct timeval tv;
	tv.tv_sec = 5;
	tv.tv_usec = 0;
	setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	// a command may arrive split over several segments, so collect up to the newline
	std::string pending;
	while (m_Running)
	{
		char buf[ADMIN_MAX_MSG_SIZE];
		ssize_t n = recv(clientFd, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		pending.append(buf, n);

		std::string::size_type eol;
		while ((eol = pending.find('\n')) != std::string::npos)
		{
			std::string line = pending.substr(0, eol);
			pending.erase(0, eol + 1);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty())
				continue;

			try
			{
				auto cmd = nlohmann::json::parse(line);
				auto response = HandleCommand(cmd, clientAddr);
				std::string respStr = response.dump() + "\n";
				send(clientFd, respStr.c_str(), respStr.size(), MSG_NOSIGNAL);
			}
			catch (const nlohmann::json::exception &)
			{
				nlohmann::json err = {{"status", "error"}, {"message", "invalid JSON"}};
				std::string respStr = err.dump() + "\n";
				send(clientFd, respStr.c_str(), respStr.size(), MSG_NOSIGNAL);
			}
			catch (const std::exception &e)
			{
				// this thread is detached, an exception leaving it would end urfd
				nlohmann::json err = {{"status", "error"}, {"message", std::string("command failed: ") + e.what()}};
				std::string respStr = err.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
				send(clientFd, respStr.c_str(), respStr.size(), MSG_NOSIGNAL);
			}
		}
		// no command is that long, a client that never sends a newline is dropped
		if (pending.size() > ADMIN_MAX_MSG_SIZE)
			break;
	}

	close(clientFd);
}

nlohmann::json CAdminSocket::HandleCommand(const nlohmann::json &cmd, const std::string &clientAddr)
{
	if (!cmd.contains("cmd"))
		return {{"status", "error"}, {"message", "missing 'cmd' field"}};

	std::string command = cmd["cmd"];

	// Auth does not require token
	if (command == "auth")
		return CmdAuth(cmd);

	// All other commands require valid token
	if (!cmd.contains("token") || !ValidateToken(cmd["token"]))
		return {{"status", "error"}, {"message", "authentication required"}};

	if (command == "tg_add")
		return CmdTGAdd(cmd);
	else if (command == "tg_remove")
		return CmdTGRemove(cmd);
	else if (command == "tg_list")
		return CmdTGList(cmd);
	else if (command == "status")
		return CmdStatus();
	else if (command == "reconnect")
		return CmdReconnect(cmd);
	else if (command == "tc_stats")
		return CmdTCStats();
	else if (command == "log")
		return CmdLog(cmd);
	else if (command == "kerchunk")
		return CmdKerchunk(cmd);
	else if (command == "block")
		return CmdBlock(cmd);
	else if (command == "unblock")
		return CmdUnblock(cmd);
	else if (command == "block_reset")
		return CmdBlockReset();
	else if (command == "dcs_map_add")
		return CmdDcsMapAdd(cmd);
	else if (command == "dcs_map_remove")
		return CmdDcsMapRemove(cmd);
	else if (command == "dcs_map_list")
		return CmdDcsMapList();
	else if (command == "dextra_map_add")
		return CmdDExtraMapAdd(cmd);
	else if (command == "dextra_map_remove")
		return CmdDExtraMapRemove(cmd);
	else if (command == "dextra_map_list")
		return CmdDExtraMapList();
	else if (command == "dplus_map_add")
		return CmdDPlusMapAdd(cmd);
	else if (command == "dplus_map_remove")
		return CmdDPlusMapRemove(cmd);
	else if (command == "dplus_map_list")
		return CmdDPlusMapList();
	else if (command == "ysf_map_add")
		return CmdYsfMapAdd(cmd);
	else if (command == "ysf_map_remove")
		return CmdYsfMapRemove(cmd);
	else if (command == "ysf_map_list")
		return CmdYsfMapList();
	else if (command == "svxs_user_add")
		return CmdSvxsUserAdd(cmd);
	else if (command == "svxs_user_remove")
		return CmdSvxsUserRemove(cmd);
	else if (command == "svxs_user_list")
		return CmdSvxsUserList();
	else if (command == "svxs_peer_list")
		return CmdSvxsPeerList();
	else if (command == "mmdvm_user_add")
		return CmdMmdvmUserAdd(cmd);
	else if (command == "mmdvm_user_remove")
		return CmdMmdvmUserRemove(cmd);
	else if (command == "mmdvm_user_list")
		return CmdMmdvmUserList();
	else if (command == "mmdvm_tg_add")
		return CmdMmdvmTGAdd(cmd);
	else if (command == "mmdvm_tg_remove")
		return CmdMmdvmTGRemove(cmd);
	else if (command == "mmdvm_tg_list")
		return CmdMmdvmTGList();
	else if (command == "mmdvm_peer_list")
		return CmdMmdvmPeerList();
	else if (command == "credentials_flush")
		return CmdCredentialsFlush(cmd);
	else if (command == "credentials_status")
		return CmdCredentialsStatus();
	else if (command == "clear_users")
	{
		auto *users = g_Reflector.GetUsers();
		users->Clear();
		g_Reflector.ReleaseUsers();
		return {{"status", "ok"}, {"message", "last heard list cleared"}};
	}

	// the commands of the plugins: "plugin" names one ("name"), with the command in
	// "command"; a command of a plugin also works by itself
	if (command == "plugin")
	{
		if (!cmd.contains("name") || !cmd["name"].is_string() || !cmd.contains("command") || !cmd["command"].is_string())
			return {{"status", "error"}, {"message", "missing required fields: name, command"}};
		for (size_t i = 0; i < g_Plugins.Size(); i++)
		{
			auto &plugin = g_Plugins.Get(i);
			if (cmd["name"].get<std::string>() != plugin.GetName())
				continue;
			nlohmann::json result;
			if (plugin.AdminCommand(FindPluginProtocol(g_Plugins.GetId(i)), cmd["command"].get<std::string>(), cmd, result))
				return result;
			return {{"status", "error"}, {"message", "unknown command of " + std::string(plugin.GetName()) + ": " + cmd["command"].get<std::string>()}};
		}
		return {{"status", "error"}, {"message", "no plugin " + cmd["name"].get<std::string>()}};
	}
	for (size_t i = 0; i < g_Plugins.Size(); i++)
	{
		nlohmann::json result;
		if (g_Plugins.Get(i).AdminCommand(FindPluginProtocol(g_Plugins.GetId(i)), command, cmd, result))
			return result;
	}

	return {{"status", "error"}, {"message", "unknown command: " + command}};
}

////////////////////////////////////////////////////////////////////////////////////////
// Auth

nlohmann::json CAdminSocket::CmdAuth(const nlohmann::json &cmd)
{
	if (!cmd.contains("password"))
		return {{"status", "error"}, {"message", "missing password"}};

	std::string pw = cmd["password"];

	// after a wrong password, refuse every attempt for a while, so that
	// passwords can't be guessed through the dashboard at any speed
	const auto now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(m_TokenMutex);
		if (now - m_LastAuthFailure < std::chrono::seconds(ADMIN_AUTH_FAIL_DELAY))
			return {{"status", "error"}, {"message", "too many attempts, try again in a few seconds"}};
	}

	// compare in constant time
	bool match = !m_Password.empty() && (pw.size() == m_Password.size());
	if (match)
	{
		unsigned char diff = 0;
		for (size_t i = 0; i < pw.size(); i++)
			diff |= (unsigned char)pw[i] ^ (unsigned char)m_Password[i];
		match = (0 == diff);
	}
	if (!match)
	{
		std::lock_guard<std::mutex> lock(m_TokenMutex);
		m_LastAuthFailure = now;
		std::cerr << "Admin: authentication failed" << std::endl;
		return {{"status", "error"}, {"message", "invalid password"}};
	}

	std::string token = GenerateToken();

	{
		std::lock_guard<std::mutex> lock(m_TokenMutex);
		m_Tokens[token] = std::chrono::steady_clock::now() + std::chrono::seconds(ADMIN_TOKEN_TTL_SECONDS);

		// Purge expired tokens
		auto now = std::chrono::steady_clock::now();
		for (auto it = m_Tokens.begin(); it != m_Tokens.end(); )
		{
			if (now >= it->second)
				it = m_Tokens.erase(it);
			else
				++it;
		}
	}

	return {{"status", "ok"}, {"token", token}};
}

bool CAdminSocket::ValidateToken(const std::string &token) const
{
	std::lock_guard<std::mutex> lock(m_TokenMutex);
	auto it = m_Tokens.find(token);
	if (it == m_Tokens.end())
		return false;
	return std::chrono::steady_clock::now() < it->second;
}

// 128 bits straight from the system's random source
std::string CAdminSocket::GenerateToken(void)
{
	std::random_device rd;
	std::ostringstream oss;
	oss << std::hex << std::setfill('0');
	for (int i = 0; i < 4; i++)
		oss << std::setw(8) << rd();
	return oss.str();
}

////////////////////////////////////////////////////////////////////////////////////////
// TG commands

nlohmann::json CAdminSocket::CmdTGAdd(const nlohmann::json &cmd)
{
	if (!cmd.contains("protocol") || !cmd.contains("tg") || !cmd.contains("module"))
		return {{"status", "error"}, {"message", "missing required fields: protocol, tg, module"}};

	std::string protocol = cmd["protocol"];
	uint32_t tg = cmd["tg"];
	std::string modStr = cmd["module"];
	if (modStr.empty() || modStr[0] < 'A' || modStr[0] > 'Z')
		return {{"status", "error"}, {"message", "module must be A-Z"}};
	char module = modStr[0];

	uint8_t timeslot = 2;
	if (cmd.contains("ts"))
		timeslot = cmd["ts"];

	int ttl = 900;  // default 15 min
	if (cmd.contains("ttl"))
		ttl = cmd["ttl"];

	// Validate module exists in reflector
	if (!g_Reflector.IsValidModule(module))
		return {{"status", "error"}, {"message", std::string("module ") + module + " is not configured"}};

	if (IsMmdvmClientProtocol(protocol))
	{
		auto &protocols = g_Reflector.GetProtocols();
		protocols.Lock();
		auto *mmdvm = FindMmdvmClient(protocols, protocol, cmd);
		if (!mmdvm)
		{
			protocols.Unlock();
			return {{"status", "error"}, {"message", "MMDVMClient connection not active"}};
		}
		// the network would get the TG from two of its hotspots
		for (auto *other : protocols.MmdvmClients())
			if (other != mmdvm && other->GetNetwork() == mmdvm->GetNetwork() && other->GetTGMap().IsTGMapped(tg))
			{
				protocols.Unlock();
				return {{"status", "error"}, {"message", "TG" + std::to_string(tg) + " is mapped by " + other->GetName() + " in the same network already"}};
			}
		bool ok = mmdvm->GetTGMap().AddDynamic(tg, module, timeslot, ttl);
		protocols.Unlock();

		if (!ok)
			return {{"status", "error"}, {"message", "failed to add TG (module in use or static conflict)"}};

		// BM API / kerchunk / reconnect outside lock (HTTP I/O)
		if (mmdvm->GetBmApi().IsConfigured())
		{
			mmdvm->GetBmApi().AddStaticTG(tg, timeslot);
		}
		else
		{
			mmdvm->RequestKerchunk(tg);
		}
		mmdvm->RequestReconnect();

		std::string method = mmdvm->GetBmApi().IsConfigured() ? "API" : "kerchunk";
		return {{"status", "ok"}, {"message", mmdvm->GetName() + " TG" + std::to_string(tg) + " -> Module " + module + " added via " + method}};
	}
	else if (protocol == "svxreflector")
	{
		auto &protocols = g_Reflector.GetProtocols();
		protocols.Lock();
		auto *proto = protocols.FindByType(EProtocol::svxreflector);
		if (!proto)
		{
			protocols.Unlock();
			return {{"status", "error"}, {"message", "SvxReflector protocol not active"}};
		}
		auto *svx = static_cast<CSvxReflectorProtocol *>(proto);
		bool ok = svx->AddDynamicTG(tg, module, ttl);
		protocols.Unlock();

		if (!ok)
			return {{"status", "error"}, {"message", "failed to add TG (module in use or conflict)"}};

		return {{"status", "ok"}, {"message", "SVX TG" + std::to_string(tg) + " -> Module " + module + " added"}};
	}
	else if (protocol == "svx")
	{
		auto &protocols = g_Reflector.GetProtocols();
		protocols.Lock();
		auto *proto = protocols.FindByType(EProtocol::svx);
		if (!proto)
		{
			protocols.Unlock();
			return {{"status", "error"}, {"message", "SVXServer protocol not active"}};
		}
		auto *svxs = static_cast<CSvxProtocol *>(proto);
		bool ok = svxs->AddDynamicTG(tg, module, ttl);
		protocols.Unlock();

		if (!ok)
			return {{"status", "error"}, {"message", "failed to add TG (module in use or conflict)"}};

		return {{"status", "ok"}, {"message", "SVXServer TG" + std::to_string(tg) + " -> Module " + module + " added"}};
	}

	return {{"status", "error"}, {"message", "unknown protocol: " + protocol}};
}

nlohmann::json CAdminSocket::CmdTGRemove(const nlohmann::json &cmd)
{
	if (!cmd.contains("protocol") || !cmd.contains("tg"))
		return {{"status", "error"}, {"message", "missing required fields: protocol, tg"}};

	std::string protocol = cmd["protocol"];
	uint32_t tg = cmd["tg"];

	if (IsMmdvmClientProtocol(protocol))
	{
		auto &protocols = g_Reflector.GetProtocols();
		protocols.Lock();
		std::string err;
		auto *mmdvm = FindMmdvmClient(protocols, protocol, cmd, tg, &err);
		if (!mmdvm)
		{
			protocols.Unlock();
			return {{"status", "error"}, {"message", err.empty() ? std::string("MMDVMClient connection not active") : err}};
		}
		// Get timeslot before removing (needed for BM API)
		uint8_t ts = 2;
		auto &tgmap = mmdvm->GetTGMap().GetTGMap();
		auto tgit = tgmap.find(tg);
		if (tgit != tgmap.end())
			ts = tgit->second.timeslot;
		bool ok = mmdvm->GetTGMap().RemoveDynamic(tg);
		protocols.Unlock();

		if (!ok)
			return {{"status", "error"}, {"message", "failed to remove TG (not found or static)"}};

		// BM API / reconnect outside lock (HTTP I/O)
		if (mmdvm->GetBmApi().IsConfigured())
			mmdvm->GetBmApi().RemoveStaticTG(tg, ts);
		mmdvm->RequestReconnect();

		return {{"status", "ok"}, {"message", mmdvm->GetName() + " TG" + std::to_string(tg) + " removed"}};
	}
	else if (protocol == "svxreflector")
	{
		auto &protocols = g_Reflector.GetProtocols();
		protocols.Lock();
		auto *proto = protocols.FindByType(EProtocol::svxreflector);
		if (!proto)
		{
			protocols.Unlock();
			return {{"status", "error"}, {"message", "SvxReflector protocol not active"}};
		}
		auto *svx = static_cast<CSvxReflectorProtocol *>(proto);
		bool ok = svx->RemoveDynamicTG(tg);
		protocols.Unlock();

		if (!ok)
			return {{"status", "error"}, {"message", "failed to remove TG (not found or static)"}};

		return {{"status", "ok"}, {"message", "SVX TG" + std::to_string(tg) + " removed"}};
	}
	else if (protocol == "svx")
	{
		auto &protocols = g_Reflector.GetProtocols();
		protocols.Lock();
		auto *proto = protocols.FindByType(EProtocol::svx);
		if (!proto)
		{
			protocols.Unlock();
			return {{"status", "error"}, {"message", "SVXServer protocol not active"}};
		}
		auto *svxs = static_cast<CSvxProtocol *>(proto);
		bool ok = svxs->RemoveDynamicTG(tg);
		protocols.Unlock();

		if (!ok)
			return {{"status", "error"}, {"message", "failed to remove TG (not found or static)"}};

		return {{"status", "ok"}, {"message", "SVXServer TG" + std::to_string(tg) + " removed"}};
	}

	return {{"status", "error"}, {"message", "unknown protocol: " + protocol}};
}

nlohmann::json CAdminSocket::CmdTGList(const nlohmann::json &cmd)
{
	std::string protocol = "all";
	if (cmd.contains("protocol"))
		protocol = cmd["protocol"];

	nlohmann::json result;
	result["status"] = "ok";
	result["mappings"] = nlohmann::json::array();

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();

	if (protocol == "all" || IsMmdvmClientProtocol(protocol))
	{
		// every connection, or the one asked for; "protocol" stays "mmdvmclient"
		// for older dashboards, "client" says which
		for (auto *mmdvm : protocols.MmdvmClients())
		{
			if ((protocol.size() == 12 || (protocol != "all" && cmd.contains("client"))) && mmdvm != FindMmdvmClient(protocols, protocol, cmd))
				continue;
			auto mappings = mmdvm->GetTGMap().GetAllMappings();
			for (const auto &m : mappings)
			{
				result["mappings"].push_back({
					{"protocol", "mmdvmclient"},
					{"client", mmdvm->GetInstance()},
					{"client_name", mmdvm->GetName()},
					{"network", mmdvm->GetNetwork()},
					{"tg", m.tg},
					{"module", std::string(1, m.module)},
					{"ts", m.timeslot},
					{"static", m.is_static},
					{"primary", m.is_primary},
					{"remaining", m.remainingSeconds}
				});
			}
		}
	}

	if (protocol == "all" || protocol == "svxreflector")
	{
		auto *proto = protocols.FindByType(EProtocol::svxreflector);
		if (proto)
		{
			auto *svx = static_cast<CSvxReflectorProtocol *>(proto);
			auto mappings = svx->GetTGMappings();
			for (const auto &m : mappings)
			{
				result["mappings"].push_back({
					{"protocol", "svxreflector"},
					{"tg", m.tg},
					{"module", std::string(1, m.module)},
					{"static", m.is_static},
					{"primary", m.is_primary},
					{"remaining", m.remainingSeconds}
				});
			}
		}
	}

	for (size_t i = 0; i < g_Plugins.Size(); i++)
	{
		auto &plugin = g_Plugins.Get(i);
		if (protocol != "all" && protocol != Lower(plugin.GetName()))
			continue;
		if (auto *proto = protocols.FindByType(g_Plugins.GetId(i)))
			plugin.AdminMappings(proto, result["mappings"]);
	}

	if (protocol == "all" || protocol == "svx")
	{
		auto *proto = protocols.FindByType(EProtocol::svx);
		if (proto)
		{
			auto *svxs = static_cast<CSvxProtocol *>(proto);
			auto mappings = svxs->GetTGMappings();
			for (const auto &m : mappings)
			{
				result["mappings"].push_back({
					{"protocol", "svx"},
					{"tg", m.tg},
					{"module", std::string(1, m.module)},
					{"static", m.is_static},
					{"primary", m.is_primary},
					{"remaining", m.remainingSeconds}
				});
			}
		}
	}

	protocols.Unlock();
	return result;
}

////////////////////////////////////////////////////////////////////////////////////////
// SVX Server User Management

nlohmann::json CAdminSocket::CmdSvxsUserAdd(const nlohmann::json &cmd)
{
	if (!cmd.contains("callsign") || !cmd.contains("password"))
		return {{"status", "error"}, {"message", "missing required fields: callsign, password"}};

	std::string callsign = cmd["callsign"];
	std::string password = cmd["password"];

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::svx);
	if (!proto)
	{
		protocols.Unlock();
		return {{"status", "error"}, {"message", "SVXServer protocol not active"}};
	}
	auto *svxs = static_cast<CSvxProtocol *>(proto);
	protocols.Unlock();
	if (!svxs->AddUser(callsign, password))
		return {{"status", "error"}, {"message", "failed to add user (credential database)"}};

	return {{"status", "ok"}, {"message", "user " + callsign + " added"}};
}

nlohmann::json CAdminSocket::CmdSvxsUserRemove(const nlohmann::json &cmd)
{
	if (!cmd.contains("callsign"))
		return {{"status", "error"}, {"message", "missing required field: callsign"}};

	std::string callsign = cmd["callsign"];

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::svx);
	if (!proto)
	{
		protocols.Unlock();
		return {{"status", "error"}, {"message", "SVXServer protocol not active"}};
	}
	auto *svxs = static_cast<CSvxProtocol *>(proto);
	protocols.Unlock();
	const int r = svxs->RemoveUser(callsign);

	if (r < 0)
		return {{"status", "error"}, {"message", "credential database unavailable, the user may still be there"}};
	if (0 == r)
		return {{"status", "error"}, {"message", "user not found"}};
	return {{"status", "ok"}, {"message", "user " + callsign + " removed"}};
}

nlohmann::json CAdminSocket::CmdSvxsUserList(void)
{
	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::svx);
	// GetUsers may ask the credential database: not with the lock every packet's routing needs
	protocols.Unlock();
	nlohmann::json result;
	result["status"] = "ok";
	result["users"] = nlohmann::json::array();
	if (proto)
	{
		auto *svxs = static_cast<CSvxProtocol *>(proto);
		auto users = svxs->GetUsers();
		// the callsigns stay plain strings, older dashboards read them so
		result["sources"] = nlohmann::json::object();
		for (const auto &u : users)
		{
			result["users"].push_back(u.first);
			result["sources"][u.first] = u.second;
		}
	}
	return result;
}

////////////////////////////////////////////////////////////////////////////////////////
// Status

nlohmann::json CAdminSocket::CmdStatus(void)
{
	nlohmann::json status;
	status["status"] = "ok";
	status["reflector"] = g_Configure.GetString(g_Keys.names.callsign);
	status["version"] = g_Version.GetVersion();
	status["modules"] = g_Reflector.GetModules();
	status["tc_modules"] = g_Reflector.GetTranscodedModules();	// transcoded now
	status["tc_auto"] = g_Configure.GetBoolean(g_Keys.tc.automatic);

	// Report which protocols are active and their block lists
	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();

	status["mmdvm_active"] = (protocols.FindByType(EProtocol::mmdvmclient) != nullptr);
	status["mmdvm_clients"] = nlohmann::json::array();
	for (auto *c : protocols.MmdvmClients())
		status["mmdvm_clients"].push_back({{"client", c->GetInstance()}, {"name", c->GetName()}, {"network", c->GetNetwork()},
			{"dmrid", c->GetDmrId()}, {"master", c->GetMasterAddress()}, {"connected", c->IsConnected()}});
	status["mmdvm_server_active"] = (protocols.FindByType(EProtocol::dmrmmdvm) != nullptr);
	status["svx_active"] = (protocols.FindByType(EProtocol::svxreflector) != nullptr);
	status["svxs_active"] = (protocols.FindByType(EProtocol::svx) != nullptr);
	// the plugins: name, whether they run, and what they add themselves (after the lock)
	std::vector<CProtocol *> pluginProtos;
	status["plugins"] = nlohmann::json::array();
	for (size_t i = 0; i < g_Plugins.Size(); i++)
	{
		auto *proto = protocols.FindByType(g_Plugins.GetId(i));
		pluginProtos.push_back(proto);
		status["plugins"].push_back({{"name", g_Plugins.Get(i).GetName()}, {"description", g_Plugins.Get(i).GetDescription()}, {"active", proto != nullptr}, {"ui", g_Plugins.Get(i).AdminUi()}});
	}
	status["dcsclient_active"] = (protocols.FindByType(EProtocol::dcsclient) != nullptr);
	status["dextraclient_active"] = (protocols.FindByType(EProtocol::dextraclient) != nullptr);
	status["dplusclient_active"] = (protocols.FindByType(EProtocol::dplusclient) != nullptr);
	status["ysfclient_active"] = (protocols.FindByType(EProtocol::ysfclient) != nullptr);

	// List all active protocol names. With several MMDVMClient connections each
	// one is listed as MMDVMClient1, MMDVMClient2 ..., and MMDVMClient once for all
	// of them: the names block and unblock take
	const bool severalMmdvm = protocols.MmdvmClients().size() > 1;
	auto protoName = [&](const CProtocol &p) -> std::string {
		if (severalMmdvm && p.GetMmdvmConnection())
			return "MMDVMClient" + std::to_string(p.GetMmdvmConnection());
		const auto nameIt = ProtoToName().find(p.GetProtocolType());
		return (nameIt != ProtoToName().end()) ? nameIt->second : std::string();
	};
	nlohmann::json activeProtos = nlohmann::json::array();
	if (severalMmdvm)
		activeProtos.push_back("MMDVMClient");
	for (auto it = protocols.begin(); it != protocols.end(); ++it)
	{
		const std::string name = protoName(**it);
		if (!name.empty())
			activeProtos.push_back(name);
	}
	status["active_protocols"] = activeProtos;

	// Collect block rules
	nlohmann::json blocks = nlohmann::json::array();

	for (auto it = protocols.begin(); it != protocols.end(); ++it)
	{
		auto dstType = (*it)->GetProtocolType();
		const std::string dstName = protoName(**it);
		if (dstName.empty()) continue;

		for (const auto &[proto, name] : ProtoToName())
		{
			// skip self-blocks, the client protocols block their own type; between
			// MMDVMClient connections the type means all the others
			if (proto == dstType && !(*it)->GetMmdvmConnection()) continue;
			if ((*it)->IsSourceBlocked(proto))
				blocks.push_back({{"from", name}, {"to", dstName}});
		}
		for (unsigned n : (*it)->GetBlockedMmdvm())
			blocks.push_back({{"from", "MMDVMClient" + std::to_string(n)}, {"to", dstName}});
	}
	status["blocks"] = blocks;

	protocols.Unlock();

	// Peer counts (no protocols lock needed, uses separate clients lock)
	{
		int mmdvmPeerCount = 0;
		int svxsPeerCount = 0;
		auto *clients = g_Reflector.GetClients();
		for (auto it = clients->begin(); it != clients->end(); ++it)
		{
			auto proto = (*it)->GetProtocol();
			if (proto == EProtocol::dmrmmdvm)
				mmdvmPeerCount++;
			else if (proto == EProtocol::svx)
				svxsPeerCount++;
		}
		g_Reflector.ReleaseClients();
		status["mmdvm_peer_count"] = mmdvmPeerCount;
		status["svxs_peer_count"] = svxsPeerCount;
	}
	for (size_t i = 0; i < g_Plugins.Size(); i++)
		g_Plugins.Get(i).AdminStatus(pluginProtos[i], status);

	return status;
}

nlohmann::json CAdminSocket::CmdReconnect(const nlohmann::json &cmd)
{
	if (!cmd.contains("protocol"))
		return {{"status", "error"}, {"message", "missing protocol field"}};

	std::string protocol = cmd["protocol"];

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();

	if (IsMmdvmClientProtocol(protocol))
	{
		// one connection when named, else all of them
		if (protocol.size() == 12 || cmd.contains("client"))
		{
			if (auto *c = FindMmdvmClient(protocols, protocol, cmd))
			{
				c->RequestReconnect();
				protocols.Unlock();
				return {{"status", "ok"}, {"message", c->GetName() + " reconnect requested"}};
			}
		}
		else if (!protocols.MmdvmClients().empty())
		{
			for (auto *c : protocols.MmdvmClients())
				c->RequestReconnect();
			protocols.Unlock();
			return {{"status", "ok"}, {"message", "MMDVMClient reconnect requested"}};
		}
	}
	else if (protocol == "svxreflector")
	{
		auto *proto = protocols.FindByType(EProtocol::svxreflector);
		if (proto)
		{
			static_cast<CSvxReflectorProtocol *>(proto)->RequestReconnect();
			protocols.Unlock();
			return {{"status", "ok"}, {"message", "SVX reconnect requested"}};
		}
	}
	else if (protocol == "dcsclient")
	{
		auto *proto = protocols.FindByType(EProtocol::dcsclient);
		if (proto)
		{
			static_cast<CDcsClientProtocol *>(proto)->RequestReconnect();
			protocols.Unlock();
			return {{"status", "ok"}, {"message", "DCSClient reconnect requested"}};
		}
	}
	else if (protocol == "dextraclient")
	{
		auto *proto = protocols.FindByType(EProtocol::dextraclient);
		if (proto)
		{
			static_cast<CDExtraClientProtocol *>(proto)->RequestReconnect();
			protocols.Unlock();
			return {{"status", "ok"}, {"message", "DExtraClient reconnect requested"}};
		}
	}
	else if (protocol == "dplusclient")
	{
		auto *proto = protocols.FindByType(EProtocol::dplusclient);
		if (proto)
		{
			static_cast<CDPlusClientProtocol *>(proto)->RequestReconnect();
			protocols.Unlock();
			return {{"status", "ok"}, {"message", "DPlusClient reconnect requested"}};
		}
	}
	else if (protocol == "ysfclient")
	{
		auto *proto = protocols.FindByType(EProtocol::ysfclient);
		if (proto)
		{
			static_cast<CYsfClientProtocol *>(proto)->RequestReconnect();
			protocols.Unlock();
			return {{"status", "ok"}, {"message", "YSFClient reconnect requested"}};
		}
	}

	protocols.Unlock();
	return {{"status", "error"}, {"message", "protocol not found: " + protocol}};
}

nlohmann::json CAdminSocket::CmdTCStats(void)
{
	nlohmann::json result;
	result["status"] = "ok";
	result["modules"] = nlohmann::json::array();

	// with Modules = auto every module may be transcoded: list those tcd is connected for
	const std::string tcModules = g_Configure.GetBoolean(g_Keys.tc.automatic) ? g_Reflector.GetTranscodedModules() : g_Configure.GetString(g_Keys.tc.modules);

	for (char mod : tcModules)
	{
		nlohmann::json modInfo;
		modInfo["module"] = std::string(1, mod);
		modInfo["connected"] = g_TCServer.IsModuleConnected(mod);

		auto stream = g_Reflector.GetStream(mod);
		if (stream)
		{
			modInfo["streaming"] = stream->IsOpen();
			if (stream->IsOpen())
			{
				modInfo["stream_id"] = stream->GetStreamId();
				modInfo["user"] = stream->GetUserCallsign().GetCS();
			}

			CCodecStream *cs = stream->GetCodecStream();
			if (cs)
			{
				modInfo["codec_in"] = (int)cs->GetCodecIn();
				modInfo["total_packets"] = cs->GetTotalPackets();
				modInfo["mismatch_count"] = cs->GetMismatchCount();
				modInfo["rt_count"] = cs->GetRTCount();
				if (cs->GetRTCount() > 0 && cs->GetRTMin() >= 0)
				{
					modInfo["rt_min_ms"] = (int)(cs->GetRTMin() * 1000.0);
					modInfo["rt_avg_ms"] = (int)(cs->GetRTAvg() * 1000.0);
					modInfo["rt_max_ms"] = (int)(cs->GetRTMax() * 1000.0);
				}
			}
		}
		else
		{
			modInfo["streaming"] = false;
		}

		result["modules"].push_back(modInfo);
	}

	return result;
}

nlohmann::json CAdminSocket::CmdLog(const nlohmann::json &cmd)
{
	int count = 50;
	if (cmd.contains("lines"))
		count = cmd["lines"];
	if (count < 1) count = 1;
	if (count > 200) count = 200;

	auto lines = g_LogBuffer.GetLines(count);

	nlohmann::json result;
	result["status"] = "ok";
	result["lines"] = nlohmann::json::array();
	for (const auto &line : lines)
		result["lines"].push_back(line);

	return result;
}

////////////////////////////////////////////////////////////////////////////////////////
// Kerchunk — send a brief DMR transmission to keep a TG alive on BrandMeister

nlohmann::json CAdminSocket::CmdKerchunk(const nlohmann::json &cmd)
{
	if (!cmd.contains("tg"))
		return {{"status", "error"}, {"message", "missing 'tg' field"}};

	uint32_t tg = cmd["tg"];

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	std::string err;
	auto *mmdvm = FindMmdvmClient(protocols, cmd.contains("protocol") && cmd["protocol"].is_string() ? cmd["protocol"].get<std::string>() : std::string(), cmd, tg, &err);
	if (!mmdvm)
	{
		protocols.Unlock();
		return {{"status", "error"}, {"message", err.empty() ? std::string("MMDVMClient connection not active") : err}};
	}

	// Verify TG is actually mapped
	char module = mmdvm->GetTGMap().TGToModule(tg);
	if (module == ' ')
	{
		protocols.Unlock();
		return {{"status", "error"}, {"message", "TG not mapped"}};
	}

	mmdvm->RequestKerchunk(tg);
	protocols.Unlock();

	// Also refresh local timer
	mmdvm->GetTGMap().RefreshActivity(tg);

	std::cout << "Admin: kerchunk requested for TG" << tg << std::endl;
	return {{"status", "ok"}, {"tg", tg}, {"module", std::string(1, module)}};
}

////////////////////////////////////////////////////////////////////////////////////////
// Block commands — runtime protocol blocking (not persisted to config)

// one side of a block: a protocol type, or one MMDVMClient connection
struct SBlockEnd
{
	EProtocol type = EProtocol::none;
	unsigned connection = 0;	// 1 to 9 for "MMDVMClient1" ... "MMDVMClient9"
	bool Matches(const CProtocol &p) const
	{
		return p.GetProtocolType() == type && (connection == 0 || p.GetMmdvmConnection() == connection);
	}
};

static bool ParseBlockEnd(const std::string &name, SBlockEnd &e)
{
	if (const unsigned n = MmdvmClientConnection(name))
	{
		e.type = EProtocol::mmdvmclient;
		e.connection = n;
		return true;
	}
	const auto it = s_NameToProto.find(name);
	if (it == s_NameToProto.end())
		return false;
	e.type = it->second;
	return true;
}

nlohmann::json CAdminSocket::CmdBlock(const nlohmann::json &cmd)
{
	if (!cmd.contains("a") || !cmd.contains("b") || !cmd["a"].is_string() || !cmd["b"].is_string())
		return {{"status", "error"}, {"message", "missing 'a' and 'b' fields"}};

	const std::string aName = cmd["a"];
	const std::string bName = cmd["b"];
	SBlockEnd a, b;
	if (!ParseBlockEnd(aName, a))
		return {{"status", "error"}, {"message", "unknown protocol: " + aName}};
	if (!ParseBlockEnd(bName, b))
		return {{"status", "error"}, {"message", "unknown protocol: " + bName}};

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();

	// Bidirectional: B as a source on every A, and A as a source on every B; an
	// MMDVMClient connection named alone (MMDVMClient2) is one connection, not the type
	auto apply = [](CProtocol &p, const SBlockEnd &other) {
		if (other.connection)
			p.SetMmdvmConnectionBlocked(other.connection);
		else
			p.SetSourceBlocked(other.type);
	};
	for (auto it = protocols.begin(); it != protocols.end(); ++it)
	{
		if (a.Matches(**it))
			apply(**it, b);
		if (b.Matches(**it))
			apply(**it, a);
	}

	protocols.Unlock();
	std::cout << "Admin: blocked " << aName << " <-> " << bName << std::endl;
	return {{"status", "ok"}, {"message", aName + " <-> " + bName + " blocked"}};
}

nlohmann::json CAdminSocket::CmdUnblock(const nlohmann::json &cmd)
{
	if (!cmd.contains("a") || !cmd.contains("b") || !cmd["a"].is_string() || !cmd["b"].is_string())
		return {{"status", "error"}, {"message", "missing 'a' and 'b' fields"}};

	const std::string aName = cmd["a"];
	const std::string bName = cmd["b"];
	SBlockEnd a, b;
	if (!ParseBlockEnd(aName, a))
		return {{"status", "error"}, {"message", "unknown protocol: " + aName}};
	if (!ParseBlockEnd(bName, b))
		return {{"status", "error"}, {"message", "unknown protocol: " + bName}};

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();

	// Bidirectional: B as a source on every A, and A as a source on every B; an
	// MMDVMClient connection named alone (MMDVMClient2) is one connection, not the type
	auto apply = [](CProtocol &p, const SBlockEnd &other) {
		if (other.connection)
			p.ClearMmdvmConnectionBlocked(other.connection);
		else
			p.ClearSourceBlocked(other.type);
	};
	for (auto it = protocols.begin(); it != protocols.end(); ++it)
	{
		if (a.Matches(**it))
			apply(**it, b);
		if (b.Matches(**it))
			apply(**it, a);
	}

	protocols.Unlock();
	std::cout << "Admin: unblocked " << aName << " <-> " << bName << std::endl;
	return {{"status", "ok"}, {"message", aName + " <-> " + bName + " unblocked"}};
}

nlohmann::json CAdminSocket::CmdBlockReset(void)
{
	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();

	for (auto it = protocols.begin(); it != protocols.end(); ++it)
		(*it)->ResetBlocksToDefault();

	protocols.Unlock();
	std::cout << "Admin: blocks reset to config defaults" << std::endl;
	return {{"status", "ok"}, {"message", "all blocks reset to config defaults"}};
}

////////////////////////////////////////////////////////////////////////////////////////
// DCSClient mapping management

nlohmann::json CAdminSocket::CmdDcsMapAdd(const nlohmann::json &cmd)
{
	if (!cmd.contains("host") || !cmd.contains("port") || !cmd.contains("remote_module") || !cmd.contains("local_module"))
		return {{"status", "error"}, {"message", "missing required fields: host, port, remote_module, local_module"}};

	std::string host = cmd["host"];
	uint16_t port = cmd["port"];
	std::string remoteMod = cmd["remote_module"];
	std::string localMod = cmd["local_module"];

	if (remoteMod.empty() || localMod.empty() || remoteMod[0] < 'A' || remoteMod[0] > 'Z' || localMod[0] < 'A' || localMod[0] > 'Z')
		return {{"status", "error"}, {"message", "modules must be A-Z"}};

	if (!g_Reflector.IsValidModule(localMod[0]))
		return {{"status", "error"}, {"message", std::string("module ") + localMod[0] + " is not configured"}};

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dcsclient);
	if (!proto)
	{
		protocols.Unlock();
		return {{"status", "error"}, {"message", "DCSClient protocol not active"}};
	}

	auto *dcs = static_cast<CDcsClientProtocol *>(proto);
	bool ok = dcs->AddMapping(host, port, remoteMod[0], localMod[0]);
	protocols.Unlock();

	if (!ok)
		return {{"status", "error"}, {"message", "failed to add mapping (local module already mapped)"}};

	return {{"status", "ok"}, {"message", "DCSClient mapping " + host + ":" + std::to_string(port) + " " + remoteMod[0] + " -> " + localMod[0] + " added"}};
}

nlohmann::json CAdminSocket::CmdDcsMapRemove(const nlohmann::json &cmd)
{
	if (!cmd.contains("local_module"))
		return {{"status", "error"}, {"message", "missing required field: local_module"}};

	std::string localMod = cmd["local_module"];
	if (localMod.empty() || localMod[0] < 'A' || localMod[0] > 'Z')
		return {{"status", "error"}, {"message", "module must be A-Z"}};

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dcsclient);
	if (!proto)
	{
		protocols.Unlock();
		return {{"status", "error"}, {"message", "DCSClient protocol not active"}};
	}

	auto *dcs = static_cast<CDcsClientProtocol *>(proto);
	protocols.Unlock();
	bool ok = dcs->RemoveMapping(localMod[0]);

	if (!ok)
		return {{"status", "error"}, {"message", "mapping not found for module " + localMod}};

	return {{"status", "ok"}, {"message", "DCSClient mapping for module " + localMod + " removed"}};
}

nlohmann::json CAdminSocket::CmdDcsMapList(void)
{
	nlohmann::json result;
	result["status"] = "ok";
	result["mappings"] = nlohmann::json::array();

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dcsclient);
	if (proto)
	{
		auto *dcs = static_cast<CDcsClientProtocol *>(proto);
		auto mappings = dcs->GetMappings();
		for (const auto &m : mappings)
		{
			result["mappings"].push_back({
				{"host", m.host},
				{"port", m.port},
				{"remote_module", std::string(1, m.remoteModule)},
				{"local_module", std::string(1, m.localModule)},
				{"connected", m.connected}
			});
		}
	}
	protocols.Unlock();

	return result;
}

////////////////////////////////////////////////////////////////////////////////////////
// DExtraClient mapping management

nlohmann::json CAdminSocket::CmdDExtraMapAdd(const nlohmann::json &cmd)
{
	if (!cmd.contains("host") || !cmd.contains("port") || !cmd.contains("remote_module") || !cmd.contains("local_module"))
		return {{"status", "error"}, {"message", "missing required fields: host, port, remote_module, local_module"}};

	std::string host = cmd["host"];
	uint16_t port = cmd["port"];
	std::string remoteMod = cmd["remote_module"];
	std::string localMod = cmd["local_module"];

	if (remoteMod.empty() || localMod.empty() || remoteMod[0] < 'A' || remoteMod[0] > 'Z' || localMod[0] < 'A' || localMod[0] > 'Z')
		return {{"status", "error"}, {"message", "modules must be A-Z"}};

	if (!g_Reflector.IsValidModule(localMod[0]))
		return {{"status", "error"}, {"message", std::string("module ") + localMod[0] + " is not configured"}};

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dextraclient);
	if (!proto)
	{
		protocols.Unlock();
		return {{"status", "error"}, {"message", "DExtraClient protocol not active"}};
	}

	auto *dextra = static_cast<CDExtraClientProtocol *>(proto);
	bool ok = dextra->AddMapping(host, port, remoteMod[0], localMod[0]);
	protocols.Unlock();

	if (!ok)
		return {{"status", "error"}, {"message", "failed to add mapping (local module already mapped)"}};

	return {{"status", "ok"}, {"message", "DExtraClient mapping " + host + ":" + std::to_string(port) + " " + remoteMod[0] + " -> " + localMod[0] + " added"}};
}

nlohmann::json CAdminSocket::CmdDExtraMapRemove(const nlohmann::json &cmd)
{
	if (!cmd.contains("local_module"))
		return {{"status", "error"}, {"message", "missing required field: local_module"}};

	std::string localMod = cmd["local_module"];
	if (localMod.empty() || localMod[0] < 'A' || localMod[0] > 'Z')
		return {{"status", "error"}, {"message", "module must be A-Z"}};

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dextraclient);
	if (!proto)
	{
		protocols.Unlock();
		return {{"status", "error"}, {"message", "DExtraClient protocol not active"}};
	}

	auto *dextra = static_cast<CDExtraClientProtocol *>(proto);
	protocols.Unlock();
	bool ok = dextra->RemoveMapping(localMod[0]);

	if (!ok)
		return {{"status", "error"}, {"message", "mapping not found for module " + localMod}};

	return {{"status", "ok"}, {"message", "DExtraClient mapping for module " + localMod + " removed"}};
}

nlohmann::json CAdminSocket::CmdDExtraMapList(void)
{
	nlohmann::json result;
	result["status"] = "ok";
	result["mappings"] = nlohmann::json::array();

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dextraclient);
	if (proto)
	{
		auto *dextra = static_cast<CDExtraClientProtocol *>(proto);
		auto mappings = dextra->GetMappings();
		for (const auto &m : mappings)
		{
			result["mappings"].push_back({
				{"host", m.host},
				{"port", m.port},
				{"remote_module", std::string(1, m.remoteModule)},
				{"local_module", std::string(1, m.localModule)},
				{"connected", m.connected}
			});
		}
	}
	protocols.Unlock();

	return result;
}

////////////////////////////////////////////////////////////////////////////////////////
// DPlusClient mapping management

nlohmann::json CAdminSocket::CmdDPlusMapAdd(const nlohmann::json &cmd)
{
	if (!cmd.contains("host") || !cmd.contains("port") || !cmd.contains("remote_module") || !cmd.contains("local_module"))
		return {{"status", "error"}, {"message", "missing required fields: host, port, remote_module, local_module"}};

	std::string host = cmd["host"];
	uint16_t port = cmd["port"];
	std::string remoteMod = cmd["remote_module"];
	std::string localMod = cmd["local_module"];

	if (remoteMod.empty() || localMod.empty() || remoteMod[0] < 'A' || remoteMod[0] > 'Z' || localMod[0] < 'A' || localMod[0] > 'Z')
		return {{"status", "error"}, {"message", "modules must be A-Z"}};

	if (!g_Reflector.IsValidModule(localMod[0]))
		return {{"status", "error"}, {"message", std::string("module ") + localMod[0] + " is not configured"}};

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dplusclient);
	if (!proto)
	{
		protocols.Unlock();
		return {{"status", "error"}, {"message", "DPlusClient protocol not active"}};
	}

	auto *dplus = static_cast<CDPlusClientProtocol *>(proto);
	bool ok = dplus->AddMapping(host, port, remoteMod[0], localMod[0]);
	protocols.Unlock();

	if (!ok)
		return {{"status", "error"}, {"message", "failed to add mapping (local module already mapped)"}};

	return {{"status", "ok"}, {"message", "DPlusClient mapping " + host + ":" + std::to_string(port) + " " + remoteMod[0] + " -> " + localMod[0] + " added"}};
}

nlohmann::json CAdminSocket::CmdDPlusMapRemove(const nlohmann::json &cmd)
{
	if (!cmd.contains("local_module"))
		return {{"status", "error"}, {"message", "missing required field: local_module"}};

	std::string localMod = cmd["local_module"];
	if (localMod.empty() || localMod[0] < 'A' || localMod[0] > 'Z')
		return {{"status", "error"}, {"message", "module must be A-Z"}};

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dplusclient);
	if (!proto)
	{
		protocols.Unlock();
		return {{"status", "error"}, {"message", "DPlusClient protocol not active"}};
	}

	auto *dplus = static_cast<CDPlusClientProtocol *>(proto);
	protocols.Unlock();
	bool ok = dplus->RemoveMapping(localMod[0]);

	if (!ok)
		return {{"status", "error"}, {"message", "mapping not found for module " + localMod}};

	return {{"status", "ok"}, {"message", "DPlusClient mapping for module " + localMod + " removed"}};
}

nlohmann::json CAdminSocket::CmdDPlusMapList(void)
{
	nlohmann::json result;
	result["status"] = "ok";
	result["mappings"] = nlohmann::json::array();

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dplusclient);
	if (proto)
	{
		auto *dplus = static_cast<CDPlusClientProtocol *>(proto);
		auto mappings = dplus->GetMappings();
		for (const auto &m : mappings)
		{
			result["mappings"].push_back({
				{"host", m.host},
				{"port", m.port},
				{"remote_module", std::string(1, m.remoteModule)},
				{"local_module", std::string(1, m.localModule)},
				{"connected", m.connected}
			});
		}
	}
	protocols.Unlock();

	return result;
}

////////////////////////////////////////////////////////////////////////////////////////
// YSFClient mapping management

nlohmann::json CAdminSocket::CmdYsfMapAdd(const nlohmann::json &cmd)
{
	if (!cmd.contains("host") || !cmd.contains("port") || !cmd.contains("local_module"))
		return {{"status", "error"}, {"message", "missing required fields: host, port, local_module"}};

	std::string host = cmd["host"];
	uint16_t port = cmd["port"];
	std::string localMod = cmd["local_module"];
	uint8_t dgid = cmd.contains("dgid") ? (uint8_t)(int)cmd["dgid"] : 0;

	if (localMod.empty() || localMod[0] < 'A' || localMod[0] > 'Z')
		return {{"status", "error"}, {"message", "module must be A-Z"}};

	if (!g_Reflector.IsValidModule(localMod[0]))
		return {{"status", "error"}, {"message", std::string("module ") + localMod[0] + " is not configured"}};

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::ysfclient);
	if (!proto)
	{
		protocols.Unlock();
		return {{"status", "error"}, {"message", "YSFClient protocol not active"}};
	}

	auto *ysf = static_cast<CYsfClientProtocol *>(proto);
	bool ok = ysf->AddMapping(host, port, localMod[0], dgid);
	protocols.Unlock();

	if (!ok)
		return {{"status", "error"}, {"message", "failed to add mapping (local module already mapped)"}};

	std::string msg = "YSFClient mapping " + host + ":" + std::to_string(port) + " -> " + localMod[0];
	if (dgid > 0) msg += " DG-ID " + std::to_string(dgid);
	return {{"status", "ok"}, {"message", msg + " added"}};
}

nlohmann::json CAdminSocket::CmdYsfMapRemove(const nlohmann::json &cmd)
{
	if (!cmd.contains("local_module"))
		return {{"status", "error"}, {"message", "missing required field: local_module"}};

	std::string localMod = cmd["local_module"];
	if (localMod.empty() || localMod[0] < 'A' || localMod[0] > 'Z')
		return {{"status", "error"}, {"message", "module must be A-Z"}};

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::ysfclient);
	if (!proto)
	{
		protocols.Unlock();
		return {{"status", "error"}, {"message", "YSFClient protocol not active"}};
	}

	auto *ysf = static_cast<CYsfClientProtocol *>(proto);
	protocols.Unlock();
	bool ok = ysf->RemoveMapping(localMod[0]);

	if (!ok)
		return {{"status", "error"}, {"message", "mapping not found for module " + localMod}};

	return {{"status", "ok"}, {"message", "YSFClient mapping for module " + localMod + " removed"}};
}

nlohmann::json CAdminSocket::CmdYsfMapList(void)
{
	nlohmann::json result;
	result["status"] = "ok";
	result["mappings"] = nlohmann::json::array();

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::ysfclient);
	if (proto)
	{
		auto *ysf = static_cast<CYsfClientProtocol *>(proto);
		auto mappings = ysf->GetMappings();
		for (const auto &m : mappings)
		{
			result["mappings"].push_back({
				{"host", m.host},
				{"port", m.port},
				{"local_module", std::string(1, m.localModule)},
				{"dgid", m.dgid},
				{"connected", m.connected}
			});
		}
	}
	protocols.Unlock();

	return result;
}

////////////////////////////////////////////////////////////////////////////////////////
// MMDVM Server User Management

nlohmann::json CAdminSocket::CmdMmdvmUserAdd(const nlohmann::json &cmd)
{
	std::string password;
	if (cmd.contains("password"))
		password = cmd["password"];

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dmrmmdvm);
	protocols.Unlock();
	if (!proto)
		return {{"status", "error"}, {"message", "MMDVM server protocol not active"}};
	auto *mmdvm = static_cast<CDmrmmdvmProtocol *>(proto);

	if (cmd.contains("callsign"))
	{
		std::string callsign = cmd["callsign"];
		if (!mmdvm->AddUserByCallsign(callsign, password))
			return {{"status", "error"}, {"message", "failed to add user by callsign (not found in DMR ID database)"}};
		return {{"status", "ok"}, {"message", "user " + callsign + " added"}};
	}
	else if (cmd.contains("dmrid"))
	{
		uint32_t dmrid = cmd["dmrid"];
		if (!mmdvm->AddUser(dmrid, password))
			return {{"status", "error"}, {"message", "failed to add user"}};
		return {{"status", "ok"}, {"message", "user " + std::to_string(dmrid) + " added"}};
	}

	return {{"status", "error"}, {"message", "missing required field: dmrid or callsign"}};
}

nlohmann::json CAdminSocket::CmdMmdvmUserRemove(const nlohmann::json &cmd)
{
	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dmrmmdvm);
	protocols.Unlock();
	if (!proto)
		return {{"status", "error"}, {"message", "MMDVM server protocol not active"}};
	auto *mmdvm = static_cast<CDmrmmdvmProtocol *>(proto);

	if (cmd.contains("callsign"))
	{
		std::string callsign = cmd["callsign"];
		const int r = mmdvm->RemoveUserByCallsign(callsign);
		if (r < 0)
			return {{"status", "error"}, {"message", "credential database unavailable, the user may still be there"}};
		if (0 == r)
			return {{"status", "error"}, {"message", "user not found"}};
		return {{"status", "ok"}, {"message", "user " + callsign + " removed"}};
	}
	else if (cmd.contains("dmrid"))
	{
		uint32_t dmrid = cmd["dmrid"];
		const int r = mmdvm->RemoveUser(dmrid);
		if (r < 0)
			return {{"status", "error"}, {"message", "credential database unavailable, the user may still be there"}};
		if (0 == r)
			return {{"status", "error"}, {"message", "user not found"}};
		return {{"status", "ok"}, {"message", "user " + std::to_string(dmrid) + " removed"}};
	}

	return {{"status", "error"}, {"message", "missing required field: dmrid or callsign"}};
}

nlohmann::json CAdminSocket::CmdMmdvmUserList(void)
{
	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dmrmmdvm);
	protocols.Unlock();

	nlohmann::json result;
	result["status"] = "ok";
	result["users"] = nlohmann::json::array();
	if (proto)
	{
		auto *mmdvm = static_cast<CDmrmmdvmProtocol *>(proto);
		auto users = mmdvm->GetUsers();
		for (const auto &u : users)
			result["users"].push_back({{"dmrid", u.dmrid}, {"callsign", u.callsign}, {"source", u.source}});
	}
	return result;
}

////////////////////////////////////////////////////////////////////////////////////////
// MMDVM Server TG Management

nlohmann::json CAdminSocket::CmdMmdvmTGAdd(const nlohmann::json &cmd)
{
	if (!cmd.contains("tg") || !cmd.contains("module"))
		return {{"status", "error"}, {"message", "missing required fields: tg, module"}};

	uint32_t tg = cmd["tg"];
	std::string modStr = cmd["module"];
	if (modStr.empty() || modStr[0] < 'A' || modStr[0] > 'Z')
		return {{"status", "error"}, {"message", "module must be A-Z"}};
	char module = modStr[0];

	uint8_t timeslot = 2;
	if (cmd.contains("ts"))
		timeslot = cmd["ts"];

	int ttl = 900;  // default 15 min
	if (cmd.contains("ttl"))
		ttl = cmd["ttl"];

	if (!g_Reflector.IsValidModule(module))
		return {{"status", "error"}, {"message", std::string("module ") + module + " is not configured"}};

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dmrmmdvm);
	protocols.Unlock();
	if (!proto)
		return {{"status", "error"}, {"message", "MMDVM server protocol not active"}};
	auto *mmdvm = static_cast<CDmrmmdvmProtocol *>(proto);
	bool ok = mmdvm->GetTGMap().AddDynamic(tg, module, timeslot, ttl);

	if (!ok)
		return {{"status", "error"}, {"message", "failed to add TG (module in use or static conflict)"}};

	return {{"status", "ok"}, {"message", "MMDVM server TG" + std::to_string(tg) + " -> Module " + module + " added"}};
}

nlohmann::json CAdminSocket::CmdMmdvmTGRemove(const nlohmann::json &cmd)
{
	if (!cmd.contains("tg"))
		return {{"status", "error"}, {"message", "missing required field: tg"}};

	uint32_t tg = cmd["tg"];

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dmrmmdvm);
	protocols.Unlock();
	if (!proto)
		return {{"status", "error"}, {"message", "MMDVM server protocol not active"}};
	auto *mmdvm = static_cast<CDmrmmdvmProtocol *>(proto);
	bool ok = mmdvm->GetTGMap().RemoveDynamic(tg);

	if (!ok)
		return {{"status", "error"}, {"message", "failed to remove TG (not found or static)"}};

	return {{"status", "ok"}, {"message", "MMDVM server TG" + std::to_string(tg) + " removed"}};
}

nlohmann::json CAdminSocket::CmdMmdvmTGList(void)
{
	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dmrmmdvm);
	protocols.Unlock();

	nlohmann::json result;
	result["status"] = "ok";
	result["mappings"] = nlohmann::json::array();
	if (proto)
	{
		auto *mmdvm = static_cast<CDmrmmdvmProtocol *>(proto);
		auto mappings = mmdvm->GetTGMap().GetAllMappings();
		for (const auto &m : mappings)
		{
			result["mappings"].push_back({
				{"tg", m.tg},
				{"module", std::string(1, m.module)},
				{"ts", m.timeslot},
				{"static", m.is_static},
				{"primary", m.is_primary},
				{"remaining", m.remainingSeconds}
			});
		}
	}
	return result;
}

////////////////////////////////////////////////////////////////////////////////////////
// MMDVM Server Peer List

nlohmann::json CAdminSocket::CmdMmdvmPeerList(void)
{
	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::dmrmmdvm);
	protocols.Unlock();

	nlohmann::json result;
	result["status"] = "ok";
	result["peers"] = nlohmann::json::array();
	if (proto)
	{
		auto *mmdvm = static_cast<CDmrmmdvmProtocol *>(proto);
		const auto peerInfoMap = mmdvm->GetPeerInfo();

		// Iterate clients for this protocol
		auto *clients = g_Reflector.GetClients();
		for (auto it = clients->begin(); it != clients->end(); ++it)
		{
			if ((*it)->GetProtocol() != EProtocol::dmrmmdvm)
				continue;

			nlohmann::json peer;
			peer["callsign"] = (*it)->GetCallsign().GetCS();
			peer["ip"] = std::string((*it)->GetIp().GetAddress());
			peer["module"] = std::string(1, (*it)->GetReflectorModule());
			peer["connected_since"] = (int64_t)(*it)->GetConnectTime();

			// Look up peer info by IP
			std::string ipStr = (*it)->GetIp().GetAddress();
			auto infoIt = peerInfoMap.find(ipStr);
			if (infoIt != peerInfoMap.end() && infoIt->second.populated)
			{
				const auto &info = infoIt->second;
				peer["peer_info"] = {
					{"dmrid", info.dmrid},
					{"callsign", info.callsign},
					{"rxFreq", info.rxFreq},
					{"txFreq", info.txFreq},
					{"txPower", info.txPower},
					{"colorCode", info.colorCode},
					{"latitude", info.latitude},
					{"longitude", info.longitude},
					{"height", info.height},
					{"location", info.location},
					{"description", info.description},
					{"slots", info.slots},
					{"url", info.url},
					{"softwareId", info.softwareId},
					{"packageId", info.packageId}
				};
			}

			result["peers"].push_back(peer);
		}
		g_Reflector.ReleaseClients();
	}
	return result;
}

////////////////////////////////////////////////////////////////////////////////////////
// SVX Server Peer List

nlohmann::json CAdminSocket::CmdSvxsPeerList(void)
{
	nlohmann::json result;
	result["status"] = "ok";
	result["peers"] = nlohmann::json::array();

	auto &protocols = g_Reflector.GetProtocols();
	protocols.Lock();
	auto *proto = protocols.FindByType(EProtocol::svx);
	if (proto)
	{
		auto *svxs = static_cast<CSvxProtocol *>(proto);
		auto peers = svxs->GetConnectedPeers();
		for (const auto &p : peers)
		{
			nlohmann::json peer;
			peer["callsign"] = p.callsign;
			peer["ip"] = p.ip;
			peer["udp_discovered"] = p.udpDiscovered;

			// subscribed TGs
			nlohmann::json tgs = nlohmann::json::array();
			for (uint32_t tg : p.subscribedTGs)
				tgs.push_back(tg);
			peer["subscribed_tgs"] = tgs;
			peer["monitored_tgs"] = p.monitoredTGs;
			peer["connected_since"] = (int64_t)p.connectedSince;

			// node info if available
			if (p.nodeInfo.populated)
			{
				peer["node_info"] = {
					{"software", p.nodeInfo.software},
					{"location", p.nodeInfo.location},
					{"qth", p.nodeInfo.qth},
					{"rx_site", p.nodeInfo.rxSiteName},
					{"tx_site", p.nodeInfo.txSiteName}
				};
			}

			result["peers"].push_back(peer);
		}
	}
	protocols.Unlock();
	return result;
}

// forgets cached database logins: all, or one with "service" (mmdvm, svx, a plugin's) and "user"
nlohmann::json CAdminSocket::CmdCredentialsFlush(const nlohmann::json &cmd)
{
	if (!cmd.contains("service") && !cmd.contains("user"))
	{
		g_Credentials.Flush();
		return {{"status", "ok"}, {"message", "credential cache flushed"}};
	}
	if (!cmd.contains("service") || !cmd.contains("user") || !cmd["service"].is_string() || !cmd["user"].is_string())
		return {{"status", "error"}, {"message", "service and user go together"}};
	const std::string service = cmd["service"];
	if (!ECredService::Valid(service))
		return {{"status", "error"}, {"message", "service must be mmdvm, svx or the one of a plugin"}};
	g_Credentials.Flush(ECredService(service), cmd["user"].get<std::string>());
	return {{"status", "ok"}, {"message", "cached login of " + cmd["user"].get<std::string>() + " flushed"}};
}

nlohmann::json CAdminSocket::CmdCredentialsStatus(void)
{
	const auto st = g_Credentials.GetStatus();
	return {{"status", "ok"}, {"backend", st.backend}, {"connected", st.connected}, {"cached", st.cached},
		{"hits", st.hits}, {"misses", st.misses}, {"errors", st.errors}, {"built", CCredentialStore::BuiltBackends()}};
}

