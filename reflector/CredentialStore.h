#pragma once

// CredentialStore -- logins of the MMDVM, SVXServer and HyTalk servers from a
// database: SQLite, MySQL/MariaDB or PostgreSQL ([Credentials] in urfd.ini)
//
// The users in urfd.ini stay valid and are checked first, by each protocol.
// Only a user that is not in the ini file is looked up here. A looked-up login
// (or the fact that there is none) stays in an LRU cache for CacheTTL
// (NegativeTTL) seconds, so a login does not hit the database every time.
// The admin socket flushes the cache, and so does a reload (SIGHUP).
//
// All three protocols authenticate with a challenge and response (SHA-256,
// HMAC-SHA1, digest), so the database holds the passwords as plain text.
//
// One table, by default urfd_credentials:
//   service  'mmdvm', 'svx' or 'hytalk'
//   username the DMR ID (MMDVM, base ID without the hotspot suffix), the
//            callsign (SVXServer) or the account number (HyTalk)
//   password plain text; empty = any password (MMDVM only, as in the ini file)
//   callsign HyTalk only: the callsign shown for the account, NULL = the number
//   enabled  0 = the login is refused
// urfd creates the table when it does not exist and the database user may.
//
// A lookup blocks the calling protocol thread for at most Timeout seconds,
// twice that when a connection broke (the query is tried once more). It runs on
// the protocol's own thread, so that protocol's audio waits as long; with the
// database down that is one connect attempt every CRED_RETRY_SECONDS.
// After a failed query the database is left alone for 10 seconds: logins in
// the cache still work (also expired ones), all others are refused.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <cstdint>
#include <string>
#include <vector>
#include <list>
#include <memory>
#include <mutex>
#include <chrono>
#include <unordered_map>

enum class ECredService { mmdvm, svx, hytalk };

struct SCredential
{
	std::string password;
	std::string callsign;	// HyTalk only, may be empty
};

struct SCredConfig
{
	std::string backend;	// "ini" (no database), "sqlite", "mysql", "postgresql"
	std::string path;		// SQLite file
	std::string host, database, user, password, table;
	unsigned port = 0;		// 0 = the client library's default
	unsigned timeout = 3;	// seconds, connect and query
	unsigned cachettl = 300, negativettl = 60, cachesize = 1000;
};

// one database connection, used under CCredentialStore's lock
class CCredentialBackend
{
public:
	virtual ~CCredentialBackend() {}
	virtual const char *Name(void) const = 0;
	virtual bool Connect(std::string &err) = 0;
	virtual void Disconnect(void) = 0;
	virtual bool IsConnected(void) const = 0;
	virtual bool CreateTable(std::string &err) = 0;
	// 1 = found, 0 = no such enabled login, -1 = database error
	virtual int Lookup(const std::string &service, const std::string &user, SCredential &cred, std::string &err) = 0;
	virtual bool Store(const std::string &service, const std::string &user, const SCredential &cred, std::string &err) = 0;
	// 1 = removed, 0 = not there, -1 = database error
	virtual int Remove(const std::string &service, const std::string &user, std::string &err) = 0;
	// username and callsign of the enabled logins of a service
	virtual bool List(const std::string &service, std::vector<std::pair<std::string, std::string>> &out, std::string &err) = 0;
};

// factories, nullptr when urfd was built without that client library
std::unique_ptr<CCredentialBackend> NewSqliteBackend(const SCredConfig &cfg);
std::unique_ptr<CCredentialBackend> NewMysqlBackend(const SCredConfig &cfg);
std::unique_ptr<CCredentialBackend> NewPgsqlBackend(const SCredConfig &cfg);

class CCredentialStore
{
public:
	enum class EResult { found, notfound, error };

	struct SStatus
	{
		std::string backend;
		bool connected;
		size_t cached;
		uint64_t hits, misses, errors;
	};

	// the backends this urfd was built with, e.g. "sqlite mysql postgresql"
	static std::string BuiltBackends(void);

	// false when the backend is not built in; a database that is down is no
	// error here, it is tried again on the next lookup
	bool Open(const SCredConfig &cfg);
	void Close(void);
	bool Enabled(void) const { return m_Backend != nullptr; }

	EResult Lookup(ECredService service, const std::string &user, SCredential &cred);
	bool Store(ECredService service, const std::string &user, const SCredential &cred);
	// 1 = removed, 0 = not there, -1 = database error
	int Remove(ECredService service, const std::string &user);
	bool List(ECredService service, std::vector<std::pair<std::string, std::string>> &out);

	void Flush(void);
	void Flush(ECredService service, const std::string &user);
	SStatus GetStatus(void) const;

	static const char *ServiceName(ECredService service);

private:
	struct SEntry
	{
		std::string key;
		bool found;
		SCredential cred;
		std::chrono::steady_clock::time_point expires;
	};

	static std::string Key(ECredService service, const std::string &user);
	static bool ValidUser(const std::string &user);
	void CachePut(const std::string &key, bool found, const SCredential &cred);
	// with m_DbMutex held: connects if needed, false while the database rests
	bool Ready(std::string &err);
	void Failed(const std::string &what, const std::string &err);
	template <typename F> int Run(const char *what, F query);

	SCredConfig m_Cfg;
	std::unique_ptr<CCredentialBackend> m_Backend;

	// LRU: the front is the most recently used
	mutable std::mutex m_CacheMutex;
	std::list<SEntry> m_Lru;
	std::unordered_map<std::string, std::list<SEntry>::iterator> m_Index;
	uint64_t m_Hits = 0, m_Misses = 0, m_Errors = 0;

	mutable std::mutex m_DbMutex;
	std::chrono::steady_clock::time_point m_RetryAt;
};
