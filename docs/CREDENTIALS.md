# Credential Database

urfd keeps the logins of its servers for hotspots and radios in `urfd.ini`:
`[MMDVM]` (DMR ID = password), `[SVXServer]` (callsign = password) and
`[HyTalk]` (account = password,callsign). Since 4.20.0 they can also come from
a database, so another program (a sign-up page, a club's member database) can
manage them. SQLite, MySQL/MariaDB and PostgreSQL work.

## How it works

- **urfd.ini first.** A user in `urfd.ini` logs in with that password, even if
  the database has the user as well. Only a user who is not in `urfd.ini` is
  looked up in the database.
- **Cache.** A login from the database stays in memory for `CacheTTL` seconds
  (default 300), an unknown user for `NegativeTTL` seconds (default 60), so a
  hotspot that retries with a wrong ID does not query the database each time.
  At most `CacheSize` logins (default 1000) are kept, the least recently used go
  first. A password changed in the database counts at the latest after
  `CacheTTL` seconds, or right away after a flush (see below).
- **Database down.** A query waits at most `Timeout` seconds (default 3), twice
  that when the connection broke, as it is tried once more. After
  a failure urfd leaves the database alone for 10 seconds. In that time, and
  until it answers again, logins that are in the cache still work, even expired
  ones; all other logins from the database are refused. Users in `urfd.ini`
  are not affected. urfd starts when the database is down and connects later.
- **Blocking.** The lookup runs on the thread of the protocol the login came
  in on (MMDVM, SVXServer, HyTalk), not on a thread of its own. While it waits
  for the database, that protocol's audio and keepalives wait too. With the
  database up a lookup takes milliseconds; with it down, one connection
  attempt every 10 seconds can hold the protocol for up to `Timeout` seconds.
  Keep `Timeout` short (1 or 2) for a database on another host. Logins of
  users in `urfd.ini` and cached ones never wait.
- **Idle connections.** MySQL and PostgreSQL close idle connections; a query
  that fails on an old connection is repeated once on a new one.
- **Admin page.** With a database, the admin page adds new users to the
  database. A user who is in `urfd.ini` stays there (changing the password
  changes it in `urfd.ini`). Removing a user removes it from both. The user
  lists show both, the API marks each with `"source": "ini"` or `"db"`.
- **Plain text passwords.** The three protocols log in with a challenge and
  response (SHA-256 for MMDVM, HMAC-SHA1 for SVXServer, a digest for HyTalk),
  so urfd needs the password itself, as in `urfd.ini`. A hash such as bcrypt
  does not work. Protect the database like `urfd.ini`.

## Configuration

```ini
[Credentials]
Backend = sqlite           # ini (no database, default), sqlite, mysql or postgresql
Path = /usr/local/etc/urfd/credentials.db   # sqlite, default: next to urfd.ini
# Host = localhost         # mysql, postgresql
# Port = 3306              # default: 3306 (mysql), 5432 (postgresql)
# Database = urfd
# User = urfd
# Password = changeme
# Table = urfd_credentials # letters, digits and '_'
# Timeout = 3              # seconds for the connection and a query (1 to 30)
# CacheTTL = 300           # seconds, 0 = no cache
# NegativeTTL = 60         # seconds, 0 = unknown users are not remembered
# CacheSize = 1000
```

With a database, `[SVXServer]` and `[HyTalk]` start without any user in
`urfd.ini`. `[Credentials]` changes need a restart; a reload (SIGHUP) flushes
the cache.

In the Docker setup `/usr/local/etc/urfd` is the configuration directory
(`/opt/urfd/config` on the host), and the SQLite file sits next to `urfd.ini`
unless `Path` says otherwise. urfd creates a new file readable by its owner
only, as it holds the passwords.
The container uses the host network, so a MySQL or PostgreSQL server on the
same host is `localhost` (for PostgreSQL write `127.0.0.1` to use TCP instead of
its socket, which is not in the container).

## Building

The Docker image (`docker/Dockerfile`) and `reflector/build.sh` install the
three client libraries. For a build of your own, `config/urfd.mk` has one
switch per backend:

```make
CRED_SQLITE = auto    # libsqlite3-dev
CRED_MYSQL = auto     # libmariadb-dev (or libmysqlclient-dev)
CRED_PGSQL = auto     # libpq-dev
```

`auto` builds a backend when `pkg-config` finds its library, `true` requires
it, `false` leaves it out. An urfd without the backend set in `Backend` does
not start and says so. The admin command `credentials_status` lists the
backends of the running build.

## The table

urfd creates the table at its first connection when the database user may
(`CREATE TABLE IF NOT EXISTS`). Otherwise create it yourself:

```sql
-- MySQL / MariaDB
CREATE TABLE urfd_credentials (
  service  VARCHAR(16)  NOT NULL,   -- 'mmdvm', 'svx' or 'hytalk'
  username VARCHAR(64)  NOT NULL,
  password VARCHAR(128) NOT NULL,
  callsign VARCHAR(16)  NULL,
  enabled  SMALLINT     NOT NULL DEFAULT 1,
  PRIMARY KEY (service, username)
) DEFAULT CHARSET = utf8mb4;

-- PostgreSQL: the same without "DEFAULT CHARSET = utf8mb4"
-- SQLite: TEXT for the strings, INTEGER for enabled
```

| Column | Meaning |
|---|---|
| `service` | `mmdvm`, `svx` or `hytalk` |
| `username` | MMDVM: the 7 digit base DMR ID (it covers the 9 digit hotspot IDs, as in `urfd.ini`; a callsign does not work here). SVXServer: the callsign as the node sends it, or its base callsign (`N0CALL-HS`, then `N0CALL`). HyTalk: the account number without `@realm` |
| `password` | plain text. MMDVM: empty = any password, as in `urfd.ini`. SVXServer and HyTalk: empty = the login is refused |
| `callsign` | HyTalk only: the callsign shown for the account; NULL = the number |
| `enabled` | `0` refuses the login and hides the user from the lists |

urfd only reads, inserts, updates and deletes rows of its own table. The
database user needs `SELECT`, `INSERT`, `UPDATE` and `DELETE` on it (`CREATE`
once, if urfd should make it). Read-only is fine when the admin page does not
manage users.

Example rows:

```sql
INSERT INTO urfd_credentials (service, username, password) VALUES ('mmdvm', '1234567', 'changeme');
INSERT INTO urfd_credentials (service, username, password) VALUES ('svx', 'N0CALL', 'changeme');
INSERT INTO urfd_credentials (service, username, password, callsign) VALUES ('hytalk', '1234567', 'changeme', 'N0CALL');
```

## Admin commands

```json
{"cmd": "credentials_status", "token": "..."}
{"cmd": "credentials_flush", "token": "..."}
{"cmd": "credentials_flush", "token": "...", "service": "mmdvm", "user": "1234567"}
```

The dashboard's admin page (v2.12.3 and later) shows the status in a
"Credential database" card with a "Flush cache" button.

`credentials_status` answers with the backend, whether it is connected, the
number of cached logins, cache hits, misses, database errors and the backends
built in. `credentials_flush` forgets all cached logins, or one with `service`
and `user`, so a change in the database counts at the next login.
