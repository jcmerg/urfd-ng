#!/bin/bash
# Build and install tcd from this repository
# Automatically installs all dependencies, builds and deploys.
# Run as root or with sudo. setup.sh in the repository root calls this.
#
# Usage:
#   sudo bash tcd/build.sh
#
# The AMBE devices are used through the kernel's ftdi_sio serial driver
# (/dev/serial/by-id), no FTDI D2XX library is needed. Codec2 and the P25 IMBE
# vocoder are part of the source (codec2/, imbe/); speexdsp, libwebsockets and
# ncurses come from the distribution. A single AMBE device needs a codec plugin for DMR
# (CodecPlugin in tcd.ini, see CodecPlugin.h), which is built on its own.
# The urfd sources tcd shares (TCPacketDef.h, TCSocket.*, ...) come from
# ../reflector in the same repository.

set -e

for arg in "$@"; do
    case "$arg" in
        --with-md380)
            echo "NOTE: --with-md380 is gone, the software vocoder for DMR is a codec plugin now (CodecPlugin in tcd.ini)" ;;
        *) echo "Unknown argument: $arg"; echo "Usage: $0"; exit 1 ;;
    esac
done

TCD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="/usr/local/bin"

echo "=== Building tcd ==="

# Check build tools
for cmd in g++ make git; do
    if ! command -v $cmd &>/dev/null; then
        echo "Installing build tools..."
        apt-get update && apt-get install -y build-essential git libncurses-dev
        break
    fi
done

# Ensure ncurses is available (for tcdmon)
if ! dpkg -l libncurses-dev 2>/dev/null | grep -q '^ii'; then
    echo "Installing libncurses-dev..."
    apt-get update && apt-get install -y libncurses-dev
fi

# Ensure speexdsp (noise suppression for SVX/USRP input) and libwebsockets
# (the web dashboard) are available
for pkg in libspeexdsp-dev libwebsockets-dev; do
    if ! dpkg -l "$pkg" 2>/dev/null | grep -q '^ii'; then
        echo "Installing $pkg..."
        apt-get update && apt-get install -y "$pkg"
    fi
done

# ---------------------------------------------------------------
# 1. Build tcd from this repository
# ---------------------------------------------------------------
cd "$TCD_DIR"

# compile as the owner of the checkout, so no root owned files are left in it
OWNER=$(stat -c %U "$TCD_DIR")
as_owner() {
    if [ "$OWNER" != "root" ] && command -v sudo >/dev/null; then
        sudo -u "$OWNER" "$@"
    else
        "$@"
    fi
}

echo "Compiling tcd..."
as_owner make clean
as_owner make -j"$(nproc)"

# ---------------------------------------------------------------
# 2. Install
# ---------------------------------------------------------------
echo "Installing..."
if systemctl is-active --quiet tcd 2>/dev/null; then
    systemctl stop tcd
    cp tcd tcdmon "$INSTALL_DIR/"
    cp tools/agc-analyze.py "$INSTALL_DIR/agc-analyze"
    chmod +x "$INSTALL_DIR/agc-analyze"
    if [ ! -f /usr/local/etc/tcd.ini ]; then
        cp config/tcd.ini /usr/local/etc/tcd.ini
        echo "Default tcd.ini installed — edit /usr/local/etc/tcd.ini before starting!"
    fi
    if [ ! -f /etc/systemd/system/tcd.service ]; then
        cp config/tcd.service /etc/systemd/system/
        systemctl daemon-reload
        systemctl enable tcd
    fi
    systemctl start tcd
    echo "=== tcd restarted ==="
else
    cp tcd tcdmon "$INSTALL_DIR/"
    cp tools/agc-analyze.py "$INSTALL_DIR/agc-analyze"
    chmod +x "$INSTALL_DIR/agc-analyze"
    if [ ! -f /usr/local/etc/tcd.ini ]; then
        mkdir -p /usr/local/etc
        cp config/tcd.ini /usr/local/etc/tcd.ini
        echo "Default tcd.ini installed — edit /usr/local/etc/tcd.ini before starting!"
    fi
    if [ ! -f /etc/systemd/system/tcd.service ]; then
        cp config/tcd.service /etc/systemd/system/
        systemctl daemon-reload
        systemctl enable tcd
    fi
    echo "=== tcd installed (start with: systemctl start tcd) ==="
fi

echo "=== Build complete ==="
