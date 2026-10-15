##############################################################################
#  URFD Build Options
##############################################################################

# Installation directory for the urfd binary
BINDIR = /usr/local/bin

# Enable debug build (adds -ggdb3 and extra log messages)
debug = false

# Enable OpenDHT support for peer discovery without IP addresses
DHT = true

# Optional notification backend linkage controls (set to true to link against system libraries)
# USE_MOSQUITTO enables MQTT backend (links -lmosquitto)
# USE_LIBWEBSOCKETS enables WebSocket backend (links -lwebsockets)
USE_MOSQUITTO = false
USE_LIBWEBSOCKETS = false
