#!/bin/bash
# Build and install tcd from this repository
# Automatically installs all dependencies, builds and deploys.
# Run as root or with sudo. setup.sh in the repository root calls this.
#
# Usage:
#   sudo bash tcd/build.sh               # build without md380 (requires ≥2 AMBE devices)
#   sudo bash tcd/build.sh --with-md380  # build with md380 software vocoder
#
# The AMBE devices are used through the kernel's ftdi_sio serial driver
# (/dev/serial/by-id), no FTDI D2XX library is needed.
#
# Dependencies (auto-installed):
#   - libimbe_vocoder — from jcmerg/imbe_vocoder
#   - libmd380_vocoder (only with --with-md380) — armhf native (jcmerg/md380_vocoder)
#                        or x86_64/aarch64 via dynarmic (jcmerg/md380_vocoder_dynarmic)
# The urfd sources tcd shares (TCPacketDef.h, TCSocket.*, ...) come from
# ../reflector in the same repository.

set -e

# Parse arguments
WITH_MD380=false
for arg in "$@"; do
    case "$arg" in
        --with-md380) WITH_MD380=true ;;
        *) echo "Unknown argument: $arg"; echo "Usage: $0 [--with-md380]"; exit 1 ;;
    esac
done

TCD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILDDIR="/tmp/tcd-build"
IMBE_REPO="https://github.com/jcmerg/imbe_vocoder.git"
MD380_ARM_REPO="https://github.com/jcmerg/md380_vocoder.git"
MD380_X64_REPO="https://github.com/jcmerg/md380_vocoder_dynarmic.git"
INSTALL_DIR="/usr/local/bin"

# Detect actual toolchain target, not kernel arch (handles armhf-on-aarch64)
CC_ARCH=$(g++ -dumpmachine 2>/dev/null || true)
case "$CC_ARCH" in
    x86_64*)          ARCH="x86_64" ;;
    aarch64*|arm64*)  ARCH="aarch64" ;;
    arm*hf*|arm*eabi*) ARCH="armhf" ;;
    *)                ARCH=$(uname -m) ;;   # fallback
esac

echo "=== Building tcd ==="
echo "Architecture: $ARCH (compiler: $CC_ARCH)"
echo "MD380 software vocoder: $WITH_MD380"

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

# Ensure speexdsp is available (noise suppression for SVX/USRP input)
if ! dpkg -l libspeexdsp-dev 2>/dev/null | grep -q '^ii'; then
    echo "Installing libspeexdsp-dev..."
    apt-get update && apt-get install -y libspeexdsp-dev
fi

mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

# ---------------------------------------------------------------
# 1. libimbe_vocoder (P25 IMBE codec)
# ---------------------------------------------------------------
if [ ! -f /usr/local/lib/libimbe_vocoder.a ]; then
    echo "Building imbe_vocoder..."
    if [ -d imbe_vocoder ]; then
        cd imbe_vocoder && git pull && cd ..
    else
        git clone --depth 1 "$IMBE_REPO" imbe_vocoder
    fi
    cd imbe_vocoder
    make clean && make -j$(nproc)
    cp libimbe_vocoder.a /usr/local/lib/
    cp imbe_vocoder_api.h /usr/local/include/
    cd "$BUILDDIR"
    echo "imbe_vocoder installed"
else
    echo "imbe_vocoder: already installed"
fi

# ---------------------------------------------------------------
# 2. libmd380_vocoder (DMR/YSF software codec) — only with --with-md380
# ---------------------------------------------------------------
if [ "$WITH_MD380" = "true" ]; then
    if [ ! -f /usr/local/lib/libmd380_vocoder.a ]; then
        echo "Building md380_vocoder..."
        if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "aarch64" ]; then
            # x86_64/aarch64: use dynarmic (ARM Cortex-M JIT emulation)
            for cmd in cmake unzip python3 xxd; do
                if ! command -v $cmd &>/dev/null; then
                    echo "Installing cmake and tools..."
                    apt-get update && apt-get install -y cmake unzip python3 xxd libboost-dev
                    break
                fi
            done

            if [ -d md380_vocoder_dynarmic ]; then
                cd md380_vocoder_dynarmic && git pull && cd ..
            else
                git clone --depth 1 "$MD380_X64_REPO" md380_vocoder_dynarmic
            fi
            cd md380_vocoder_dynarmic
            mkdir -p build && cd build
            cmake .. && make -j$(nproc)
            sh ../makelib.sh
            cp libmd380_vocoder.a /usr/local/lib/
            cp ../md380_vocoder.h /usr/local/include/
            cd "$BUILDDIR"
        else
            # ARM: native md380 firmware
            if [ -d md380_vocoder ]; then
                cd md380_vocoder && git pull && cd ..
            else
                git clone --depth 1 "$MD380_ARM_REPO" md380_vocoder
            fi
            cd md380_vocoder
            make clean && make -j$(nproc)
            cp libmd380_vocoder.a /usr/local/lib/
            cp md380_vocoder.h /usr/local/include/
            cd "$BUILDDIR"
        fi
        ldconfig
        echo "md380_vocoder installed"
    else
        echo "md380_vocoder: already installed"
    fi
else
    echo "md380_vocoder: skipped (use --with-md380 to enable)"
fi

# ---------------------------------------------------------------
# 3. Build tcd from this repository
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
if [ "$WITH_MD380" = "true" ]; then
    as_owner make -j"$(nproc)" md380=true
else
    as_owner make -j"$(nproc)"
fi

# ---------------------------------------------------------------
# 4. Install
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
