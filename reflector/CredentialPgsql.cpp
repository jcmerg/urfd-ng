// CredentialPgsql -- the PostgreSQL backend of the credential store (libpq)
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "CredentialStore.h"

#ifndef CRED_PGSQL

std::unique_ptr<CCredentialBackend> NewPgsqlBackend(const SCredConfig &)
{
	return nullptr;
}

#else

#include <initializer_list>
#include <libpq-fe.h>

class CPgsqlBackend : public CCredentialBackend
{
public:
	CPgsqlBackend(const SCredConfig &cfg) : m_Cfg(cfg) {}
	~CPgsqlBackend() { Disconnect(); }

	const char *Name(void) const override { return "PostgreSQL"; }
	bool IsConnected(void) const override { return m_Db != nullptr; }

	bool Connect(std::string &err) override
	{
		const std::string port = m_Cfg.port ? std::to_string(m_Cfg.port) : std::string();
		const std::string timeout = std::to_string(m_Cfg.timeout);
		// a query that hangs ends after the timeout as well; no NOTICE when the table exists
		const std::string options = "-c statement_timeout=" + std::to_string(m_Cfg.timeout * 1000) + " -c client_min_messages=warning";
		// statement_timeout only works while the server answers: unacknowledged
		// data on a dead link (dropped packets) ends the connection after the
		// timeout as well, keepalives notice an idle one
		const std::string tcpTimeout = std::to_string(m_Cfg.timeout * 1000);
		const char *keys[] = { "host", "port", "dbname", "user", "password", "connect_timeout", "options", "application_name",
			"tcp_user_timeout", "keepalives", "keepalives_idle", "keepalives_interval", "keepalives_count", nullptr };
		const char *vals[] = { m_Cfg.host.c_str(), port.c_str(), m_Cfg.database.c_str(), m_Cfg.user.c_str(), m_Cfg.password.c_str(),
			timeout.c_str(), options.c_str(), "urfd", tcpTimeout.c_str(), "1", "60", "10", "3", nullptr };
		m_Db = PQconnectdbParams(keys, vals, 0);
		if (m_Db && CONNECTION_OK == PQstatus(m_Db))
			return true;
		err = m_Db ? Trimmed(PQerrorMessage(m_Db)) : std::string("out of memory");
		Disconnect();
		return false;
	}

	void Disconnect(void) override
	{
		if (m_Db)
			PQfinish(m_Db);
		m_Db = nullptr;
	}

	bool CreateTable(std::string &err) override
	{
		SResult r(PQexec(m_Db, ("CREATE TABLE IF NOT EXISTS " + m_Cfg.table + " ("
			"service VARCHAR(16) NOT NULL, username VARCHAR(64) NOT NULL, password VARCHAR(128) NOT NULL, callsign VARCHAR(16), "
			"enabled SMALLINT NOT NULL DEFAULT 1, PRIMARY KEY (service, username))").c_str()));
		return Ok(r, PGRES_COMMAND_OK, err);
	}

	int Lookup(const std::string &service, const std::string &user, SCredential &cred, std::string &err) override
	{
		SResult r(Exec("SELECT password, callsign FROM " + m_Cfg.table + " WHERE service = $1 AND username = $2 AND enabled <> 0", { &service, &user }));
		if (!Ok(r, PGRES_TUPLES_OK, err))
			return -1;
		if (PQntuples(r.r) < 1)
			return 0;
		cred.password = PQgetvalue(r.r, 0, 0);
		cred.callsign = PQgetisnull(r.r, 0, 1) ? "" : PQgetvalue(r.r, 0, 1);
		return 1;
	}

	bool Store(const std::string &service, const std::string &user, const SCredential &cred, std::string &err) override
	{
		SResult r(Exec("INSERT INTO " + m_Cfg.table + " (service, username, password, callsign, enabled) VALUES ($1, $2, $3, $4, 1) "
			"ON CONFLICT (service, username) DO UPDATE SET password = EXCLUDED.password, callsign = EXCLUDED.callsign, enabled = 1",
			{ &service, &user, &cred.password, cred.callsign.empty() ? nullptr : &cred.callsign }));
		return Ok(r, PGRES_COMMAND_OK, err);
	}

	int Remove(const std::string &service, const std::string &user, std::string &err) override
	{
		SResult r(Exec("DELETE FROM " + m_Cfg.table + " WHERE service = $1 AND username = $2", { &service, &user }));
		if (!Ok(r, PGRES_COMMAND_OK, err))
			return -1;
		const char *n = PQcmdTuples(r.r);
		return (n && *n && std::string(n) != "0") ? 1 : 0;
	}

	bool List(const std::string &service, std::vector<std::pair<std::string, std::string>> &out, std::string &err) override
	{
		SResult r(Exec("SELECT username, callsign FROM " + m_Cfg.table + " WHERE service = $1 AND enabled <> 0 ORDER BY username", { &service }));
		if (!Ok(r, PGRES_TUPLES_OK, err))
			return false;
		for (int i = 0; i < PQntuples(r.r); i++)
			out.emplace_back(PQgetvalue(r.r, i, 0), PQgetisnull(r.r, i, 1) ? "" : PQgetvalue(r.r, i, 1));
		return true;
	}

private:
	struct SResult
	{
		PGresult *r;
		SResult(PGresult *p) : r(p) {}
		~SResult() { PQclear(r); }
	};

	// text parameters, nullptr is NULL
	PGresult *Exec(const std::string &sql, std::initializer_list<const std::string *> params)
	{
		std::vector<const char *> v;
		for (const std::string *p : params)
			v.push_back(p ? p->c_str() : nullptr);
		return PQexecParams(m_Db, sql.c_str(), int(v.size()), nullptr, v.data(), nullptr, nullptr, 0);
	}

	bool Ok(const SResult &r, ExecStatusType want, std::string &err)
	{
		if (r.r && want == PQresultStatus(r.r))
			return true;
		err = Trimmed(r.r ? PQresultErrorMessage(r.r) : PQerrorMessage(m_Db));
		return false;
	}

	static std::string Trimmed(const char *msg)
	{
		std::string s(msg ? msg : "");
		while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
			s.pop_back();
		return s;
	}

	SCredConfig m_Cfg;
	PGconn *m_Db = nullptr;
};

std::unique_ptr<CCredentialBackend> NewPgsqlBackend(const SCredConfig &cfg)
{
	return std::make_unique<CPgsqlBackend>(cfg);
}

#endif
