// CredentialStore -- logins from a database, see CredentialStore.h
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <iostream>
#include <algorithm>
#include <cctype>
#include "CredentialStore.h"

#define CRED_RETRY_SECONDS 10	// the database rests this long after a failure

std::string CCredentialStore::BuiltBackends(void)
{
	std::string s;
#ifdef CRED_SQLITE
	s += "sqlite ";
#endif
#ifdef CRED_MYSQL
	s += "mysql ";
#endif
#ifdef CRED_PGSQL
	s += "postgresql ";
#endif
	if (!s.empty())
		s.pop_back();
	return s;
}

const ECredService ECredService::mmdvm("mmdvm");
const ECredService ECredService::svx("svx");

bool ECredService::Valid(const std::string &name)
{
	return !name.empty() && name.size() <= 16 && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::islower(c) || std::isdigit(c); });
}

std::string CCredentialStore::Key(const ECredService &service, const std::string &user)
{
	return std::string(ServiceName(service)) + '\x1f' + user;
}

// what a client sends goes into a query as a parameter, but keep it sane anyway
bool CCredentialStore::ValidUser(const std::string &user)
{
	if (user.empty() || user.size() > 64)
		return false;
	for (unsigned char c : user)
		if (c < 0x21 || c > 0x7e)
			return false;
	return true;
}

bool CCredentialStore::Open(const SCredConfig &cfg)
{
	m_Cfg = cfg;
	if (cfg.backend == "sqlite")
		m_Backend = NewSqliteBackend(cfg);
	else if (cfg.backend == "mysql")
		m_Backend = NewMysqlBackend(cfg);
	else if (cfg.backend == "postgresql")
		m_Backend = NewPgsqlBackend(cfg);
	else
		return true;	// ini: no database
	if (!m_Backend)
	{
		std::cerr << "Credentials: this urfd was built without " << cfg.backend << " support" << std::endl;
		return false;
	}
	std::cout << "Credentials: " << m_Backend->Name() << " backend, table " << cfg.table << ", cache "
	          << cfg.cachettl << " s for " << cfg.cachesize << " logins, unknown users " << cfg.negativettl << " s" << std::endl;

	// a database that is down is tried again by the next lookup
	std::lock_guard<std::mutex> lock(m_DbMutex);
	std::string err;
	Ready(err);
	return true;
}

void CCredentialStore::Close(void)
{
	std::lock_guard<std::mutex> lock(m_DbMutex);
	if (m_Backend)
		m_Backend->Disconnect();
}

bool CCredentialStore::Ready(std::string &err)
{
	if (m_Backend->IsConnected())
		return true;
	if (std::chrono::steady_clock::now() < m_RetryAt)
	{
		err = "database unavailable";
		return false;
	}
	if (!m_Backend->Connect(err))
	{
		Failed("connect", err);
		return false;
	}
	std::cout << "Credentials: connected to the " << m_Backend->Name() << " database" << std::endl;
	std::string terr;
	if (!m_Backend->CreateTable(terr))
		std::cerr << "Credentials: WARNING - cannot create table " << m_Cfg.table << ": " << terr << std::endl;
	return true;
}

void CCredentialStore::Failed(const std::string &what, const std::string &err)
{
	std::cerr << "Credentials: " << m_Backend->Name() << " " << what << " failed: " << err
	          << ", retrying in " << CRED_RETRY_SECONDS << " s" << std::endl;
	m_Backend->Disconnect();
	m_RetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(CRED_RETRY_SECONDS);
}

// runs a query with m_DbMutex held; MySQL and PostgreSQL close idle
// connections, so a query that fails on an old connection gets a new one
template <typename F>
int CCredentialStore::Run(const char *what, F query)
{
	std::string err;
	for (int attempt = 0; attempt < 2; attempt++)
	{
		const bool wasConnected = m_Backend->IsConnected();
		if (!Ready(err))
			return -1;
		const int r = query(err);
		if (r >= 0)
			return r;
		if (!wasConnected)
			break;
		m_Backend->Disconnect();
	}
	Failed(what, err);
	return -1;
}

void CCredentialStore::CachePut(const std::string &key, bool found, const SCredential &cred)
{
	std::lock_guard<std::mutex> lock(m_CacheMutex);
	auto it = m_Index.find(key);
	if (it != m_Index.end())
	{
		m_Lru.erase(it->second);
		m_Index.erase(it);
	}
	const unsigned ttl = found ? m_Cfg.cachettl : m_Cfg.negativettl;
	if (0 == ttl || 0 == m_Cfg.cachesize)
		return;
	m_Lru.push_front({ key, found, cred, std::chrono::steady_clock::now() + std::chrono::seconds(ttl) });
	m_Index[key] = m_Lru.begin();
	while (m_Lru.size() > m_Cfg.cachesize)
	{
		m_Index.erase(m_Lru.back().key);
		m_Lru.pop_back();
	}
}

CCredentialStore::EResult CCredentialStore::Lookup(const ECredService &service, const std::string &user, SCredential &cred)
{
	if (!m_Backend || !ValidUser(user))
		return EResult::notfound;

	const std::string key = Key(service, user);
	bool haveStale = false;
	SCredential stale;
	{
		std::lock_guard<std::mutex> lock(m_CacheMutex);
		auto it = m_Index.find(key);
		if (it != m_Index.end())
		{
			auto e = it->second;
			if (e->expires > std::chrono::steady_clock::now())
			{
				m_Lru.splice(m_Lru.begin(), m_Lru, e);
				m_Hits++;
				if (!e->found)
					return EResult::notfound;
				cred = e->cred;
				return EResult::found;
			}
			// expired, but better than nothing while the database is down
			if (e->found)
			{
				haveStale = true;
				stale = e->cred;
			}
		}
		m_Misses++;
	}

	SCredential c;
	int r;
	{
		// the answer goes into the cache under the database lock: a Store or
		// Remove from the admin socket cannot slip in between and be overwritten
		std::lock_guard<std::mutex> lock(m_DbMutex);
		r = Run("lookup", [&](std::string &err) { return m_Backend->Lookup(ServiceName(service), user, c, err); });
		if (r >= 0)
			CachePut(key, r == 1, c);
	}
	if (r < 0)
	{
		{
			std::lock_guard<std::mutex> lock(m_CacheMutex);
			m_Errors++;
		}
		if (!haveStale)
			return EResult::error;
		cred = stale;
		return EResult::found;
	}
	if (0 == r)
		return EResult::notfound;
	cred = c;
	return EResult::found;
}

bool CCredentialStore::Store(const ECredService &service, const std::string &user, const SCredential &cred)
{
	if (!m_Backend || !ValidUser(user) || cred.password.size() > 128 || cred.callsign.size() > 16)
		return false;
	std::lock_guard<std::mutex> lock(m_DbMutex);
	const int r = Run("store", [&](std::string &err) { return m_Backend->Store(ServiceName(service), user, cred, err) ? 1 : -1; });
	Flush(service, user);
	return r > 0;
}

int CCredentialStore::Remove(const ECredService &service, const std::string &user)
{
	if (!m_Backend || !ValidUser(user))
		return 0;
	std::lock_guard<std::mutex> lock(m_DbMutex);
	const int r = Run("remove", [&](std::string &err) { return m_Backend->Remove(ServiceName(service), user, err); });
	Flush(service, user);
	return r;
}

bool CCredentialStore::List(const ECredService &service, std::vector<std::pair<std::string, std::string>> &out)
{
	out.clear();
	if (!m_Backend)
		return true;
	std::lock_guard<std::mutex> lock(m_DbMutex);
	return Run("list", [&](std::string &err) { return m_Backend->List(ServiceName(service), out, err) ? 1 : -1; }) > 0;
}

void CCredentialStore::Flush(void)
{
	std::lock_guard<std::mutex> lock(m_CacheMutex);
	m_Lru.clear();
	m_Index.clear();
}

void CCredentialStore::Flush(const ECredService &service, const std::string &user)
{
	std::lock_guard<std::mutex> lock(m_CacheMutex);
	auto it = m_Index.find(Key(service, user));
	if (it == m_Index.end())
		return;
	m_Lru.erase(it->second);
	m_Index.erase(it);
}

CCredentialStore::SStatus CCredentialStore::GetStatus(void) const
{
	SStatus s;
	s.backend = m_Backend ? m_Cfg.backend : std::string("ini");
	{
		std::lock_guard<std::mutex> lock(m_DbMutex);
		s.connected = m_Backend && m_Backend->IsConnected();
	}
	std::lock_guard<std::mutex> lock(m_CacheMutex);
	s.cached = m_Lru.size();
	s.hits = m_Hits;
	s.misses = m_Misses;
	s.errors = m_Errors;
	return s;
}
