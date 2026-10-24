// CredentialSqlite -- the SQLite backend of the credential store
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "CredentialStore.h"

#ifndef CRED_SQLITE

std::unique_ptr<CCredentialBackend> NewSqliteBackend(const SCredConfig &)
{
	return nullptr;
}

#else

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <initializer_list>
#include <sqlite3.h>

class CSqliteBackend : public CCredentialBackend
{
public:
	CSqliteBackend(const SCredConfig &cfg) : m_Cfg(cfg) {}
	~CSqliteBackend() { Disconnect(); }

	const char *Name(void) const override { return "SQLite"; }
	bool IsConnected(void) const override { return m_Db != nullptr; }

	bool Connect(std::string &err) override
	{
		// the passwords are plain text: a new file is for its owner only
		const int fd = open(m_Cfg.path.c_str(), O_RDWR | O_CREAT, 0600);
		if (fd < 0)
		{
			err = m_Cfg.path + ": " + strerror(errno);
			return false;
		}
		close(fd);
		if (SQLITE_OK != sqlite3_open_v2(m_Cfg.path.c_str(), &m_Db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr))
		{
			err = m_Db ? sqlite3_errmsg(m_Db) : "out of memory";
			Disconnect();
			return false;
		}
		// another program writing the file holds it for a moment
		sqlite3_busy_timeout(m_Db, int(m_Cfg.timeout * 1000));
		return true;
	}

	void Disconnect(void) override
	{
		if (m_Db)
			sqlite3_close_v2(m_Db);
		m_Db = nullptr;
	}

	bool CreateTable(std::string &err) override
	{
		const std::string sql = "CREATE TABLE IF NOT EXISTS " + m_Cfg.table + " ("
			"service TEXT NOT NULL, username TEXT NOT NULL, password TEXT NOT NULL, callsign TEXT, "
			"enabled INTEGER NOT NULL DEFAULT 1, PRIMARY KEY (service, username))";
		char *msg = nullptr;
		if (SQLITE_OK == sqlite3_exec(m_Db, sql.c_str(), nullptr, nullptr, &msg))
			return true;
		err = msg ? msg : sqlite3_errmsg(m_Db);
		sqlite3_free(msg);
		return false;
	}

	int Lookup(const std::string &service, const std::string &user, SCredential &cred, std::string &err) override
	{
		SStmt st(m_Db, "SELECT password, callsign FROM " + m_Cfg.table + " WHERE service = ?1 AND username = ?2 AND enabled <> 0");
		if (!st.Bind({ &service, &user }, err))
			return -1;
		const int rc = sqlite3_step(st.s);
		if (SQLITE_DONE == rc)
			return 0;
		if (SQLITE_ROW != rc)
		{
			err = sqlite3_errmsg(m_Db);
			return -1;
		}
		cred.password = Text(st.s, 0);
		cred.callsign = Text(st.s, 1);
		return 1;
	}

	bool Store(const std::string &service, const std::string &user, const SCredential &cred, std::string &err) override
	{
		SStmt st(m_Db, "INSERT INTO " + m_Cfg.table + " (service, username, password, callsign, enabled) VALUES (?1, ?2, ?3, ?4, 1) "
			"ON CONFLICT (service, username) DO UPDATE SET password = excluded.password, callsign = excluded.callsign, enabled = 1");
		if (!st.Bind({ &service, &user, &cred.password, cred.callsign.empty() ? nullptr : &cred.callsign }, err))
			return false;
		if (SQLITE_DONE == sqlite3_step(st.s))
			return true;
		err = sqlite3_errmsg(m_Db);
		return false;
	}

	int Remove(const std::string &service, const std::string &user, std::string &err) override
	{
		SStmt st(m_Db, "DELETE FROM " + m_Cfg.table + " WHERE service = ?1 AND username = ?2");
		if (!st.Bind({ &service, &user }, err))
			return -1;
		if (SQLITE_DONE != sqlite3_step(st.s))
		{
			err = sqlite3_errmsg(m_Db);
			return -1;
		}
		return sqlite3_changes(m_Db) > 0 ? 1 : 0;
	}

	bool List(const std::string &service, std::vector<std::pair<std::string, std::string>> &out, std::string &err) override
	{
		SStmt st(m_Db, "SELECT username, callsign FROM " + m_Cfg.table + " WHERE service = ?1 AND enabled <> 0 ORDER BY username");
		if (!st.Bind({ &service }, err))
			return false;
		int rc;
		while (SQLITE_ROW == (rc = sqlite3_step(st.s)))
			out.emplace_back(Text(st.s, 0), Text(st.s, 1));
		if (SQLITE_DONE == rc)
			return true;
		err = sqlite3_errmsg(m_Db);
		return false;
	}

private:
	// a prepared statement that finalizes itself
	struct SStmt
	{
		sqlite3 *db;
		sqlite3_stmt *s = nullptr;
		SStmt(sqlite3 *d, const std::string &sql) : db(d) { sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr); }
		~SStmt() { sqlite3_finalize(s); }
		// nullptr binds NULL
		bool Bind(std::initializer_list<const std::string *> params, std::string &err)
		{
			if (!s)
			{
				err = sqlite3_errmsg(db);
				return false;
			}
			int i = 1;
			for (const std::string *p : params)
			{
				const int rc = p ? sqlite3_bind_text(s, i, p->c_str(), int(p->size()), SQLITE_TRANSIENT) : sqlite3_bind_null(s, i);
				if (SQLITE_OK != rc)
				{
					err = sqlite3_errmsg(db);
					return false;
				}
				i++;
			}
			return true;
		}
	};

	static std::string Text(sqlite3_stmt *s, int col)
	{
		const unsigned char *t = sqlite3_column_text(s, col);
		return t ? std::string(reinterpret_cast<const char *>(t), size_t(sqlite3_column_bytes(s, col))) : std::string();
	}

	SCredConfig m_Cfg;
	sqlite3 *m_Db = nullptr;
};

std::unique_ptr<CCredentialBackend> NewSqliteBackend(const SCredConfig &cfg)
{
	return std::make_unique<CSqliteBackend>(cfg);
}

#endif
