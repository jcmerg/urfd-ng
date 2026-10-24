// CredentialMysql -- the MySQL/MariaDB backend of the credential store
//
// Built against MariaDB Connector/C (libmariadb-dev) or libmysqlclient-dev.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "CredentialStore.h"

#ifndef CRED_MYSQL

std::unique_ptr<CCredentialBackend> NewMysqlBackend(const SCredConfig &)
{
	return nullptr;
}

#else

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <type_traits>
#include <mysql.h>

class CMysqlBackend : public CCredentialBackend
{
public:
	CMysqlBackend(const SCredConfig &cfg) : m_Cfg(cfg) {}
	~CMysqlBackend() { Disconnect(); }

	const char *Name(void) const override { return "MySQL"; }
	bool IsConnected(void) const override { return m_Db != nullptr; }

	bool Connect(std::string &err) override
	{
		m_Db = mysql_init(nullptr);
		if (!m_Db)
		{
			err = "out of memory";
			return false;
		}
		unsigned int t = m_Cfg.timeout;
		mysql_options(m_Db, MYSQL_OPT_CONNECT_TIMEOUT, &t);
		mysql_options(m_Db, MYSQL_OPT_READ_TIMEOUT, &t);
		mysql_options(m_Db, MYSQL_OPT_WRITE_TIMEOUT, &t);
		mysql_options(m_Db, MYSQL_SET_CHARSET_NAME, "utf8mb4");
		if (!mysql_real_connect(m_Db, m_Cfg.host.c_str(), m_Cfg.user.c_str(), m_Cfg.password.c_str(), m_Cfg.database.c_str(), m_Cfg.port, nullptr, 0))
		{
			err = mysql_error(m_Db);
			Disconnect();
			return false;
		}
		return true;
	}

	void Disconnect(void) override
	{
		if (m_Db)
			mysql_close(m_Db);
		m_Db = nullptr;
	}

	bool CreateTable(std::string &err) override
	{
		const std::string sql = "CREATE TABLE IF NOT EXISTS " + m_Cfg.table + " ("
			"service VARCHAR(16) NOT NULL, username VARCHAR(64) NOT NULL, password VARCHAR(128) NOT NULL, callsign VARCHAR(16) NULL, "
			"enabled SMALLINT NOT NULL DEFAULT 1, PRIMARY KEY (service, username)) DEFAULT CHARSET = utf8mb4";
		if (0 == mysql_query(m_Db, sql.c_str()))
			return true;
		err = mysql_error(m_Db);
		return false;
	}

	int Lookup(const std::string &service, const std::string &user, SCredential &cred, std::string &err) override
	{
		SStmt st(m_Db);
		if (!st.Execute("SELECT password, callsign FROM " + m_Cfg.table + " WHERE service = ? AND username = ? AND enabled <> 0", { &service, &user }, err))
			return -1;
		SRow row;
		const int r = st.Fetch(row, err);
		if (r > 0)
		{
			cred.password = row.Get(0);
			cred.callsign = row.Get(1);
		}
		return r;
	}

	bool Store(const std::string &service, const std::string &user, const SCredential &cred, std::string &err) override
	{
		SStmt st(m_Db);
		return st.Execute("INSERT INTO " + m_Cfg.table + " (service, username, password, callsign, enabled) VALUES (?, ?, ?, ?, 1) "
			"ON DUPLICATE KEY UPDATE password = VALUES(password), callsign = VALUES(callsign), enabled = 1",
			{ &service, &user, &cred.password, cred.callsign.empty() ? nullptr : &cred.callsign }, err);
	}

	int Remove(const std::string &service, const std::string &user, std::string &err) override
	{
		SStmt st(m_Db);
		if (!st.Execute("DELETE FROM " + m_Cfg.table + " WHERE service = ? AND username = ?", { &service, &user }, err))
			return -1;
		return mysql_stmt_affected_rows(st.s) > 0 ? 1 : 0;
	}

	bool List(const std::string &service, std::vector<std::pair<std::string, std::string>> &out, std::string &err) override
	{
		SStmt st(m_Db);
		if (!st.Execute("SELECT username, callsign FROM " + m_Cfg.table + " WHERE service = ? AND enabled <> 0 ORDER BY username", { &service }, err))
			return false;
		SRow row;
		int r;
		while (0 < (r = st.Fetch(row, err)))
			out.emplace_back(row.Get(0), row.Get(1));
		return 0 == r;
	}

private:
	// my_bool in MariaDB Connector/C, bool in libmysqlclient 8
	using NullFlag = std::remove_pointer_t<decltype(MYSQL_BIND::is_null)>;

	// two string columns
	struct SRow
	{
		char buf[2][256];
		unsigned long len[2];
		NullFlag isnull[2];
		std::string Get(int i) const { return isnull[i] ? std::string() : std::string(buf[i], std::min<size_t>(len[i], sizeof(buf[i]))); }
	};

	// a prepared statement that closes itself
	struct SStmt
	{
		MYSQL *db;
		MYSQL_STMT *s;
		bool bound = false;
		SStmt(MYSQL *d) : db(d), s(mysql_stmt_init(d)) {}
		~SStmt() { if (s) mysql_stmt_close(s); }

		// nullptr binds NULL
		bool Execute(const std::string &sql, std::initializer_list<const std::string *> params, std::string &err)
		{
			if (!s)
			{
				err = mysql_error(db);
				return false;
			}
			if (mysql_stmt_prepare(s, sql.c_str(), sql.size()))
			{
				err = mysql_stmt_error(s);
				return false;
			}
			std::vector<MYSQL_BIND> b(params.size());
			std::vector<unsigned long> lens(params.size());
			memset(b.data(), 0, b.size() * sizeof(MYSQL_BIND));
			size_t i = 0;
			for (const std::string *p : params)
			{
				if (p)
				{
					b[i].buffer_type = MYSQL_TYPE_STRING;
					b[i].buffer = const_cast<char *>(p->data());
					b[i].buffer_length = lens[i] = p->size();
					b[i].length = &lens[i];
				}
				else
					b[i].buffer_type = MYSQL_TYPE_NULL;
				i++;
			}
			if (mysql_stmt_bind_param(s, b.data()) || mysql_stmt_execute(s))
			{
				err = mysql_stmt_error(s);
				return false;
			}
			return true;
		}

		// 1 = a row, 0 = no more rows, -1 = error
		int Fetch(SRow &row, std::string &err)
		{
			if (!bound)
			{
				MYSQL_BIND b[2];
				memset(b, 0, sizeof(b));
				for (int i = 0; i < 2; i++)
				{
					b[i].buffer_type = MYSQL_TYPE_STRING;
					b[i].buffer = row.buf[i];
					b[i].buffer_length = sizeof(row.buf[i]);
					b[i].length = &row.len[i];
					b[i].is_null = &row.isnull[i];
				}
				if (mysql_stmt_bind_result(s, b) || mysql_stmt_store_result(s))
				{
					err = mysql_stmt_error(s);
					return -1;
				}
				bound = true;
			}
			const int rc = mysql_stmt_fetch(s);
			if (0 == rc || MYSQL_DATA_TRUNCATED == rc)
				return 1;
			if (MYSQL_NO_DATA == rc)
				return 0;
			err = mysql_stmt_error(s);
			return -1;
		}
	};

	SCredConfig m_Cfg;
	MYSQL *m_Db = nullptr;
};

std::unique_ptr<CCredentialBackend> NewMysqlBackend(const SCredConfig &cfg)
{
	return std::make_unique<CMysqlBackend>(cfg);
}

#endif
