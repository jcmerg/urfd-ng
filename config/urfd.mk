##############################################################################
#  URFD Build Options
##############################################################################

# Installation directory for the urfd binary
BINDIR = /usr/local/bin

# Enable debug build (adds -ggdb3 and extra log messages)
debug = false

# Enable OpenDHT support for peer discovery without IP addresses
DHT = true

# WebSocket for the live dashboard, [WebSocket] in urfd.ini (needs libwebsockets-dev)
WEBSOCKET = true
