##############################################################################
#  URFD Build Options
##############################################################################

# Installation directory for the urfd binary
BINDIR = /usr/local/bin

# Enable debug build (adds -ggdb3 and extra log messages)
debug = false

# Enable OpenDHT support for peer discovery without IP addresses
DHT = true

# Credential database backends for [Credentials] in urfd.ini
# auto  = built when the client library is installed (found with pkg-config)
# true  = always built (the build fails without the library), false = never
# SQLite: libsqlite3-dev, MySQL/MariaDB: libmariadb-dev, PostgreSQL: libpq-dev
CRED_SQLITE = auto
CRED_MYSQL = auto
CRED_PGSQL = auto
