#!/bin/bash
# Build and install urfd from this repository, without Docker.
# Installs the build dependencies, builds and installs the reflector with its
# systemd unit. Run as root or with sudo.
#
# Usage:
#   sudo bash reflector/build.sh            # with OpenDHT
#   sudo bash reflector/build.sh --no-dht   # without OpenDHT (no libopendht-dev)
#
# Installs urfd, inicheck and dbutil to /usr/local/bin and the unit to
# /etc/systemd/system/urfd.service. The first install puts the configuration
# templates into /usr/local/etc/urfd, with the paths pointing there; later
# runs keep the configuration and restart a running urfd after checking
# urfd.ini with inicheck.

set -e

DHT=true
for arg in "$@"; do
    case "$arg" in
        --no-dht) DHT=false ;;
        *) echo "Unknown argument: $arg"; echo "Usage: $0 [--no-dht]"; exit 1 ;;
    esac
done

REFLECTOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$REFLECTOR_DIR")"
INSTALL_DIR="/usr/local/bin"
CFG_DIR="/usr/local/etc/urfd"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo bash $0 $*"
    exit 1
fi

echo "=== Building urfd ==="
echo "OpenDHT: $DHT"

# ---------------------------------------------------------------
# 1. Dependencies
# ---------------------------------------------------------------
PKGS="build-essential git nlohmann-json3-dev libcurl4-gnutls-dev libopus-dev libssl-dev libwebsockets-dev"
[ "$DHT" = "true" ] && PKGS="$PKGS libopendht-dev"
MISSING=""
for p in $PKGS; do
    dpkg -s "$p" 2>/dev/null | grep -q '^Status: install ok installed' || MISSING="$MISSING $p"
done
if [ -n "$MISSING" ]; then
    echo "Installing$MISSING..."
    apt-get update
    if ! apt-get install -y $MISSING; then
        if [ "$DHT" = "true" ]; then
            echo "Could not install the packages. Without libopendht-dev, run: $0 --no-dht"
        fi
        exit 1
    fi
fi

# ---------------------------------------------------------------
# 2. Build
# ---------------------------------------------------------------
cd "$REFLECTOR_DIR"

# compile as the owner of the checkout, so no root owned files are left in it
OWNER=$(stat -c %U "$REFLECTOR_DIR")
as_owner() {
    if [ "$OWNER" != "root" ] && command -v sudo >/dev/null; then
        sudo -u "$OWNER" "$@"
    else
        "$@"
    fi
}

# the build options; a urfd.mk changed by hand is kept
[ -f urfd.mk ] || as_owner cp "$REPO_DIR/config/urfd.mk" urfd.mk

echo "Compiling urfd..."
as_owner make clean
as_owner make -j"$(nproc)" DHT="$DHT"

# ---------------------------------------------------------------
# 3. Configuration (first install only)
# ---------------------------------------------------------------
FIRST=false
if [ ! -f "$CFG_DIR/urfd.ini" ]; then
    FIRST=true
    mkdir -p "$CFG_DIR"
    cp "$REPO_DIR/config/urfd.ini" "$CFG_DIR/urfd.ini"
    sed -i "s#/home/user/#$CFG_DIR/#" "$CFG_DIR/urfd.ini"
    chmod 600 "$CFG_DIR/urfd.ini"   # holds the admin, hotspot and BrandMeister passwords
fi
for f in urfd.interlink urfd.whitelist urfd.blacklist urfd.terminal; do
    [ -f "$CFG_DIR/$f" ] || cp "$REPO_DIR/config/$f" "$CFG_DIR/$f"
done

# ---------------------------------------------------------------
# 4. Install
# ---------------------------------------------------------------
echo "Installing..."
RUNNING=false
systemctl is-active --quiet urfd 2>/dev/null && RUNNING=true

if [ "$RUNNING" = "true" ] && ! ./inicheck -q "$CFG_DIR/urfd.ini"; then
    echo "urfd.ini has errors (see above), urfd keeps running the old version."
    exit 1
fi

[ "$RUNNING" = "true" ] && systemctl stop urfd
install -m 755 urfd inicheck dbutil "$INSTALL_DIR/"   # replaces the files, a running binary is no problem
if [ ! -f /etc/systemd/system/urfd.service ]; then
    cp "$REPO_DIR/config/urfd.service" /etc/systemd/system/
    systemctl daemon-reload
    systemctl enable urfd
fi

if [ "$RUNNING" = "true" ]; then
    systemctl start urfd
    echo "=== urfd restarted ==="
elif [ "$FIRST" = "true" ]; then
    echo "=== urfd installed ==="
    echo "Edit $CFG_DIR/urfd.ini, check it with: inicheck -q $CFG_DIR/urfd.ini"
    echo "then start with: systemctl start urfd"
else
    echo "=== urfd installed (start with: systemctl start urfd) ==="
fi
