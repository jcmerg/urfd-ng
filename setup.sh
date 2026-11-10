#!/bin/bash
#
# setup.sh -- install or update the urfd reflector and the tcd transcoder
#
#   sudo ./setup.sh            first install: asks a few questions, writes the
#                              configuration, builds and starts everything.
#                              On an installed machine it offers an update.
#   sudo ./setup.sh update     pull the latest version and rebuild, keeping the
#                              configuration.
#   sudo ./setup.sh status|start|stop|restart|logs|reload|check
#                              day-to-day commands, see 'setup.sh help'.
#
# The reflector runs in Docker (with its dashboard), the transcoder in Docker or
# natively with systemd, with or without AMBE USB sticks. Both can run on this
# machine, or the transcoder on a second one (e.g. a Raspberry Pi with the AMBE
# sticks). docker/build.sh builds and starts whatever of the two has its
# configuration in $URFD_DATA here.
#
# Supported: Debian, Ubuntu, Raspberry Pi OS (x86_64, arm64, armhf).
# Guide: docs/INSTALL.md

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="${URFD_DATA:-/opt/urfd}"
CFG_DIR="$DATA_DIR/config"
# the transcoder's configuration: natively (systemd) or in Docker
TCD_INI_NATIVE="/usr/local/etc/tcd.ini"
TCD_INI_DOCKER="$DATA_DIR/tcd/tcd.ini"
if [ -f "$TCD_INI_DOCKER" ]; then TCD_DOCKER=true; TCD_INI="$TCD_INI_DOCKER"; else TCD_DOCKER=false; TCD_INI="$TCD_INI_NATIVE"; fi
LOG_FILE="/var/log/urfd-setup.log"
export URFD_DATA="$DATA_DIR"

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
info()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33mWARNING:\033[0m %s\n' "$*"; }
die()   { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ask VAR "question" "default" [regex] [hint]
ask() {
    local var=$1 question=$2 default=${3:-} regex=${4:-} hint=${5:-} answer
    while true; do
        if [ -n "$default" ]; then
            read -r -p "$question [$default]: " answer </dev/tty || die "no terminal to answer on, run setup.sh in an interactive session (ssh -t)"
            answer=${answer:-$default}
        else
            read -r -p "$question: " answer </dev/tty || die "no terminal to answer on, run setup.sh in an interactive session (ssh -t)"
        fi
        if [ -z "$regex" ] || [[ "$answer" =~ $regex ]]; then
            printf -v "$var" '%s' "$answer"
            return
        fi
        echo "  ${hint:-Invalid input, please try again.}"
    done
}

# ask_yn "question" y|n -> exit status 0 for yes
ask_yn() {
    local question=$1 default=$2 answer prompt="[y/N]"
    [ "$default" = "y" ] && prompt="[Y/n]"
    while true; do
        read -r -p "$question $prompt: " answer </dev/tty || die "no terminal to answer on, run setup.sh in an interactive session (ssh -t)"
        answer=${answer:-$default}
        case "${answer,,}" in
            y|yes) return 0 ;;
            n|no)  return 1 ;;
        esac
    done
}

# set_ini FILE SECTION KEY VALUE -- set "KEY = VALUE" in [SECTION], replacing
# the active or commented-out key, keeping all other lines and comments
set_ini() {
    python3 - "$@" <<'PY'
import re, sys
path, section, key, value = sys.argv[1:5]
lines = open(path).read().split('\n')
start = next((i for i, l in enumerate(lines) if l.strip() == '[' + section + ']'), None)
if start is None:
    # append the section, keeping the file's final newline
    tail = [''] if lines and lines[-1] == '' else []
    lines = (lines[:-1] if tail else lines) + ['', '[' + section + ']', key + ' = ' + value] + tail
else:
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith('[')), len(lines))
    active = re.compile(r'^\s*' + re.escape(key) + r'\s*=')
    commented = re.compile(r'^\s*#\s*' + re.escape(key) + r'\s*=')
    hit = next((i for i in range(start + 1, end) if active.match(lines[i])), None)
    if hit is None:
        hit = next((i for i in range(start + 1, end) if commented.match(lines[i])), None)
    if hit is None:
        lines.insert(start + 1, key + ' = ' + value)
    else:
        lines[hit] = key + ' = ' + value
open(path, 'w').write('\n'.join(lines))
PY
}

# del_ini FILE SECTION KEY -- remove the active KEY from [SECTION]
del_ini() {
    python3 - "$@" <<'PY'
import re, sys
path, section, key = sys.argv[1:4]
lines = open(path).read().split('\n')
start = next((i for i, l in enumerate(lines) if l.strip() == '[' + section + ']'), None)
if start is not None:
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith('[')), len(lines))
    active = re.compile(r'^\s*' + re.escape(key) + r'\s*=')
    lines = [l for i, l in enumerate(lines) if not (start < i < end and active.match(l))]
open(path, 'w').write('\n'.join(lines))
PY
}

local_ip() {
    ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i=1; i<NF; i++) if ($i == "src") print $(i+1)}' | head -1
}

# run git as the owner of the checkout, not as root
repo_git() {
    local owner
    owner=$(stat -c %U "$REPO_DIR")
    if [ "$owner" != "root" ] && command -v sudo >/dev/null; then
        sudo -u "$owner" git -C "$REPO_DIR" "$@"
    else
        git -C "$REPO_DIR" "$@"
    fi
}

# ---------------------------------------------------------------------------
# checks and packages
# ---------------------------------------------------------------------------

check_system() {
    [ "$(id -u)" -eq 0 ] || die "please run as root: sudo ./setup.sh"
    [ -r /etc/os-release ] || die "unknown Linux distribution"
    . /etc/os-release
    case "${ID:-} ${ID_LIKE:-}" in
        *debian*|*ubuntu*|*raspbian*) ;;
        *) die "this script supports Debian, Ubuntu and Raspberry Pi OS, not '${PRETTY_NAME:-$ID}'" ;;
    esac
    command -v systemctl >/dev/null || die "systemd is required"
}

install_base_packages() {
    local missing=()
    for p in git curl python3 ca-certificates; do
        dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
    done
    if [ ${#missing[@]} -gt 0 ]; then
        info "Installing ${missing[*]}"
        apt-get update -qq
        apt-get install -y -qq "${missing[@]}"
    fi
}

install_docker() {
    if command -v docker >/dev/null && docker compose version >/dev/null 2>&1; then
        return
    fi
    echo
    echo "The reflector (and the transcoder in Docker) runs in Docker, which is not installed (or lacks 'docker compose')."
    ask_yn "Install Docker from download.docker.com now?" y || die "Docker is required"
    info "Installing Docker"
    curl -fsSL https://get.docker.com | sh
    systemctl enable --now docker
    docker compose version >/dev/null 2>&1 || die "'docker compose' is still missing after the Docker install"
}

# ---------------------------------------------------------------------------
# questions
# ---------------------------------------------------------------------------

ask_mode() {
    echo
    bold "What should run on this machine?"
    echo "  1) Reflector and transcoder (the AMBE USB sticks are plugged into this machine)"
    echo "  2) Reflector only (no transcoder, or the transcoder runs on another machine)"
    echo "  3) Transcoder only (the reflector runs on another machine)"
    ask MODE "Choice" "1" '^[123]$' "Please enter 1, 2 or 3."
    case "$MODE" in
        1) WITH_URFD=true;  WITH_TCD=true;  TCD_REMOTE=false ;;
        2) WITH_URFD=true;  WITH_TCD=false
           echo
           echo "A transcoder converts audio between D-Star, DMR/YSF, M17, P25 and NXDN."
           echo "Without one, users only hear stations that use the same mode."
           if ask_yn "Will a transcoder on another machine connect to this reflector?" n; then
               TCD_REMOTE=true
           else
               TCD_REMOTE=false
           fi ;;
        3) WITH_URFD=false; WITH_TCD=true;  TCD_REMOTE=false ;;
    esac
}

ask_reflector() {
    echo
    bold "Reflector"
    echo "Reflector callsigns are URF plus 3 letters or digits, e.g. URF363."
    echo "Pick one that is not taken yet, see https://xlxapi.rlx.lu/api/exportreflectors."
    ask CALLSIGN "Reflector callsign" "" '^URF[A-Z0-9]{3}$' "Use URF followed by 3 capital letters or digits, e.g. URF363."
    ask EMAIL "Sysop e-mail address" "" '^[^@ ]+@[^@ ]+\.[^@ ]+$' "Please enter a valid e-mail address."
    ask COUNTRY "Country code (2 letters, e.g. DE, AT, US)" "" '^[A-Z]{2}$' "Two capital letters, e.g. DE."
    ask SPONSOR "Sponsor or club (shown on the dashboard)" "$CALLSIGN"
    ask DASHURL "Dashboard address" "http://$(local_ip):8363"
    echo
    echo "Modules are the 'rooms' of the reflector, letters A to Z."
    ask MODULES "Modules" "ADMSZ" '^[A-Z]{1,26}$' "Capital letters A-Z, e.g. ADMSZ."
    MODULES=$(echo "$MODULES" | fold -w1 | awk '!seen[$0]++' | tr -d '\n')
    ECHO_MODULE=""
    if ask_yn "Use one module as echo/parrot (plays back what you say)?" y; then
        local def=""
        [[ "$MODULES" == *Z* ]] && def="Z"
        ask ECHO_MODULE "Echo module" "$def" "^[$MODULES]$" "Pick one of $MODULES."
    fi
    TC_MODULES=""
    if [ "$WITH_TCD" = true ] || [ "$TCD_REMOTE" = true ]; then
        ask_tc_modules
    fi
    TC_PASSWORD=""
    if [ "$TCD_REMOTE" = true ]; then
        TC_PASSWORD=$(python3 -c 'import secrets, string; print("".join(secrets.choice(string.ascii_letters + string.digits) for _ in range(24)))')
    fi
    if ask_yn "Enable the admin page in the dashboard (talkgroups, live log)?" y; then
        ADMIN_PASSWORD=$(python3 -c 'import secrets, string; print("".join(secrets.choice(string.ascii_letters + string.digits) for _ in range(12)))')
    else
        ADMIN_PASSWORD=""
    fi
    echo
    echo "The XLX directory lists the reflector in Pi-Star, WPSD and other hotspot software."
    echo "Register only once the reflector is reachable from the internet."
    if ask_yn "Register the reflector in the XLX directory now?" n; then
        CALLHOME=true
        ask CH_COUNTRY "Country name for the directory (e.g. Germany)" ""
        ask CH_COMMENT "Short description for the directory" "$CALLSIGN reflector"
    else
        CALLHOME=false
    fi
}

ask_tc_modules() {
    echo
    echo "Transcoded modules get audio converted between all modes. The AMBE devices"
    echo "handle: one device 1 module (with a codec plugin for DMR), DV3000+DV3003 2,"
    echo "two DV3000 1, two DV3003 up to 3; more modules need a codec plugin for D-Star"
    echo "and DMR (not part of this repository, see 'Codec plugins' in tcd/README.md)."
    ask TC_MODULES "Transcoded modules" "${MODULES:0:1}" "^[$MODULES]{1,26}$" "Letters out of $MODULES."
}

ask_transcoder() {
    echo
    bold "Transcoder"
    echo "Run the transcoder"
    echo "  1) in Docker, next to the reflector or on its own (with or without AMBE sticks)"
    echo "  2) natively with systemd"
    ask TCD_WHERE "Choice" "1" '^[12]$' "Please enter 1 or 2."
    if [ "$TCD_WHERE" = "1" ]; then TCD_DOCKER=true; TCD_INI="$TCD_INI_DOCKER"; else TCD_DOCKER=false; TCD_INI="$TCD_INI_NATIVE"; fi
    if [ "$WITH_URFD" = true ]; then
        TCD_SERVER="127.0.0.1"
    else
        ask TCD_SERVER "IP address or host name of the reflector" "" '^[0-9a-fA-F:]+$|^[a-zA-Z0-9]([a-zA-Z0-9.-]*[a-zA-Z0-9])?$' "Please enter an IP address or a host name."
        echo "Enter the modules to transcode: any of the reflector's modules when its urfd.ini"
        echo "says [Transcoder] Modules = auto (setup.sh writes that), else exactly that list."
        ask TC_MODULES "Transcoded modules" "A" '^[A-Z]{1,26}$' "Capital letters A-Z."
        echo "Enter the reflector's urfd.ini [Transcoder] Password, empty if it has none."
        ask TC_PASSWORD "Transcoder password" "" '^[^#[:space:]]*$' "No '#' and no spaces."
    fi
    echo
    echo "Which AMBE hardware is plugged in?"
    echo "  1) one device: a DV3000 / ThumbDV or a DV3003   (DMR/YSF then needs a codec plugin)"
    echo "  2) two devices: DV3000 + DV3003, two DV3000 or two DV3003"
    echo "  3) none: D-Star and DMR/YSF from a codec plugin, in software"
    ask HW "Choice" "1" '^[123]$' "Please enter 1, 2 or 3."
    # how many modules the devices cover; the rest needs a codec plugin
    local cover=1
    [ "$HW" = "2" ] && cover=3
    [ "$HW" = "3" ] && cover=0
    if [ "$HW" != "2" ] || [ ${#TC_MODULES} -gt $cover ]; then
        ask_codec_plugin
    fi
    if [ "$HW" = "2" ] && [ ${#TC_MODULES} -gt 1 ]; then
        echo "  (DV3000 + DV3003 cover 2 modules, two DV3000 1, two DV3003 3; tcd tells which at start)"
    fi
}

# A codec plugin does D-Star and DMR/YSF (and P25) in software. None is part of
# this repository: software AMBE/AMBE2+ may touch patents ("Codec plugins" in
# tcd/README.md), so using one is the operator's decision and setup.sh fetches
# tcd-mbelib-neo-plugin only when asked to. Docker and tcd/build.sh build every
# tcd/plugins/<name>/ with tcd, write_tcd_config loads them, update pulls them.
ask_codec_plugin() {
    local d
    for d in "$REPO_DIR"/tcd/plugins/*/; do
        if [ -f "$d/Makefile" ]; then
            info "Codec plugin in ${d%/}: it is built with tcd and loaded in $TCD_INI"
            return 0
        fi
    done
    echo
    echo "This needs a codec plugin, a software vocoder for D-Star and DMR/YSF. None is part"
    echo "of this repository: software AMBE and AMBE2+ may touch patents, so using one is your"
    echo "decision (see 'Codec plugins' in tcd/README.md). One with mbelib-neo is"
    echo "https://github.com/jcmerg/tcd-mbelib-neo-plugin, read its patent notice first."
    if ask_yn "Fetch tcd-mbelib-neo-plugin into tcd/plugins/mbelib-neo now?" n; then
        repo_git clone -q https://github.com/jcmerg/tcd-mbelib-neo-plugin.git tcd/plugins/mbelib-neo \
            || die "could not fetch tcd-mbelib-neo-plugin"
        info "tcd-mbelib-neo-plugin fetched, it is built with tcd and loaded in $TCD_INI"
    else
        warn "without a codec plugin tcd does not start for these modules. Put one into $REPO_DIR/tcd/plugins/<name>/, add 'CodecPlugin = /usr/local/lib/tcd/<file>.so' to $TCD_INI and run 'sudo ./setup.sh update'"
        ask_yn "Continue?" y || exit 0
    fi
}

# the codec plugins built with tcd, one path per line
built_codec_plugins() {
    if [ "$TCD_DOCKER" = true ]; then
        docker run --rm --entrypoint sh urfd-tcd:latest -c 'ls /usr/local/lib/tcd/*.so 2>/dev/null' 2>/dev/null || true
    else
        ls /usr/local/lib/tcd/*.so 2>/dev/null || true
    fi
}

# ---------------------------------------------------------------------------
# configuration
# ---------------------------------------------------------------------------

write_urfd_config() {
    info "Writing the reflector configuration to $CFG_DIR"
    mkdir -p "$CFG_DIR"
    local ini="$CFG_DIR/urfd.ini"
    cp "$REPO_DIR/config/urfd.ini" "$ini"
    for f in urfd.interlink urfd.whitelist urfd.blacklist urfd.terminal; do
        [ -f "$CFG_DIR/$f" ] || cp "$REPO_DIR/config/$f" "$CFG_DIR/$f"
    done

    set_ini "$ini" Names Callsign "$CALLSIGN"
    set_ini "$ini" Names SysopEmail "$EMAIL"
    set_ini "$ini" Names Country "$COUNTRY"
    set_ini "$ini" Names Sponsor "$SPONSOR"
    set_ini "$ini" Names DashboardUrl "$DASHURL"

    set_ini "$ini" Modules Modules "$MODULES"
    # drop the template's descriptions of modules that do not exist
    local m
    for m in {A..Z}; do
        [[ "$MODULES" == *$m* ]] || del_ini "$ini" Modules "Description$m"
    done
    [ -n "$ECHO_MODULE" ] && set_ini "$ini" Modules "Description$ECHO_MODULE" "Echo"

    if [ -n "$TC_MODULES" ]; then
        set_ini "$ini" Transcoder Port 10100
        # tcd's Modules decide (urfd 4.25.0): a module is transcoded while tcd is connected for it
        set_ini "$ini" Transcoder Modules auto
        if [ "$TCD_REMOTE" = true ]; then
            set_ini "$ini" Transcoder BindingAddress 0.0.0.0
            set_ini "$ini" Transcoder Password "$TC_PASSWORD"
        else
            set_ini "$ini" Transcoder BindingAddress 127.0.0.1
        fi
    else
        set_ini "$ini" Transcoder Port 0
        del_ini "$ini" Transcoder Modules
    fi

    if [ -n "$ECHO_MODULE" ]; then
        set_ini "$ini" Echo Enable true
        set_ini "$ini" Echo Module "$ECHO_MODULE"
    else
        set_ini "$ini" Echo Enable false
    fi

    set_ini "$ini" YSF RegistrationID "$((RANDOM % 90000 + 10000))"
    set_ini "$ini" YSF RegistrationName "$COUNTRY $CALLSIGN"
    set_ini "$ini" YSF RegistrationDescription "URF Reflector"

    if [ -n "$ADMIN_PASSWORD" ]; then
        set_ini "$ini" Admin Enable true
        set_ini "$ini" Admin Password "$ADMIN_PASSWORD"
        set_ini "$ini" Dashboard Admin true
    fi

    # the dashboard reads everything from here (loopback, nginx passes /ws)
    set_ini "$ini" WebSocket Enable true

    # paths inside the container
    local c=/usr/local/etc/urfd
    set_ini "$ini" "DMR ID DB" Mode both
    set_ini "$ini" "DMR ID DB" FilePath "$c/dmrid.dat"
    set_ini "$ini" "NXDN ID DB" Mode both
    set_ini "$ini" "NXDN ID DB" FilePath "$c/nxdn.dat"
    set_ini "$ini" "YSF TX/RX DB" Mode both
    set_ini "$ini" "YSF TX/RX DB" FilePath "$c/ysfnode.dat"
    set_ini "$ini" Files PidPath /var/run/xlxd.pid
    set_ini "$ini" Files WhitelistPath "$c/urfd.whitelist"
    set_ini "$ini" Files BlacklistPath "$c/urfd.blacklist"
    set_ini "$ini" Files InterlinkPath "$c/urfd.interlink"
    set_ini "$ini" Files G3TerminalPath "$c/urfd.terminal"

    # the reflector registers itself in the XLX directory ([CallingHome] in urfd.ini)
    if [ "$CALLHOME" = true ]; then
        set_ini "$ini" CallingHome Enable true
        set_ini "$ini" CallingHome Country "$CH_COUNTRY"
        set_ini "$ini" CallingHome Comment "${CH_COMMENT:0:100}"
    fi
    secure_files
}

# urfd.ini holds the admin, hotspot and BrandMeister passwords, so only root
# (urfd in the container) reads it
secure_files() {
    chmod 600 "$CFG_DIR/urfd.ini"
}

write_tcd_config() {
    info "Writing the transcoder configuration to $TCD_INI"
    mkdir -p "$(dirname "$TCD_INI")"
    cp "$REPO_DIR/tcd/config/tcd.ini" "$TCD_INI"
    # a password for the monitor page (and tcdmon from other hosts)
    TCD_MONITOR_PASSWORD=$(python3 -c 'import secrets, string; print("".join(secrets.choice(string.ascii_letters + string.digits) for _ in range(12)))')
    # every codec plugin built with tcd gets its CodecPlugin line
    local plugins
    plugins=$(built_codec_plugins | tr '\n' ' ')
    python3 - "$TCD_INI" "$TCD_SERVER" "$TC_MODULES" "$TCD_SERIALS" "${TC_PASSWORD:-}" "$TCD_MONITOR_PASSWORD" "$plugins" <<'PY'
import re, sys
path, server, modules, serials, password, monpass, plugins = sys.argv[1:8]
lines = open(path).read().split('\n')
out = []
for l in lines:
    if re.match(r'^\s*ServerAddress\s*=', l):
        l = 'ServerAddress = ' + server
    elif re.match(r'^\s*Modules\s*=', l):
        l = 'Modules = ' + modules
    elif password and re.match(r'^\s*#?\s*Password\s*=', l):
        l = 'Password = ' + password
    elif re.match(r'^\s*#?\s*MonitorPassword\s*=', l):
        l = 'MonitorPassword = ' + monpass
    elif re.match(r'^\s*#?\s*DeviceSerial\s*=', l):
        continue
    elif plugins.split() and re.match(r'^\s*#\s*CodecPlugin\s*=', l):
        out += ['CodecPlugin = ' + p for p in plugins.split()]
        continue
    out.append(l)
    # the serials go right after the comment that explains them
    if 'tcd --list-devices' in l:
        out += ['DeviceSerial = ' + s for s in serials.split()]
open(path, 'w').write('\n'.join(out))
PY
    chmod 600 "$TCD_INI"
}

check_urfd_config() {
    info "Checking the configuration with inicheck"
    if ! docker run --rm -v "$CFG_DIR:/usr/local/etc/urfd:ro" urfd:latest \
            inicheck -q /usr/local/etc/urfd/urfd.ini; then
        die "the configuration has errors (see above), fix $CFG_DIR/urfd.ini and run setup.sh again"
    fi
}

# plugins cloned into plugins/<name>/ and tcd/plugins/<name>/ (git ignores them
# there) are pulled with the project, so they are rebuilt from a version that
# fits; a plugin that cannot be pulled (e.g. no access) is only noted
pull_plugins() {
    local d
    for d in "$REPO_DIR"/plugins/*/ "$REPO_DIR"/tcd/plugins/*/; do
        [ -e "$d.git" ] || continue
        info "Updating the plugin in ${d%/}"
        repo_git -C "$d" pull --ff-only || warn "could not update the plugin in ${d%/}, it is built as it is"
    done
}

# ---------------------------------------------------------------------------
# build and start
# ---------------------------------------------------------------------------

build_urfd() {
    info "Building the reflector image (this takes a few minutes, longer on a Raspberry Pi)"
    (cd "$REPO_DIR/docker" && docker compose build urfd)
}

# builds and (re)starts the reflector and/or the transcoder in Docker, whichever
# has its configuration in $DATA_DIR
start_docker() {
    info "Starting the containers"
    bash "$REPO_DIR/docker/build.sh" start
}

build_tcd() {
    info "Building the transcoder"
    if [ "$TCD_DOCKER" = true ]; then
        install_docker
        (cd "$REPO_DIR/docker" && docker compose --profile tcd build tcd)
    else
        bash "$REPO_DIR/tcd/build.sh"
    fi
}

ask_tcd_serials() {
    TCD_SERIALS=""
    # no sticks: everything in software, from the codec plugin
    [ "$HW" = "3" ] && return 0
    echo
    echo "Connected USB serial devices (the AMBE sticks show up here):"
    if [ "$TCD_DOCKER" = true ]; then
        docker run --rm -v /dev:/dev:ro --device-cgroup-rule='c 188:* rmw' urfd-tcd:latest tcd --list-devices || true
    else
        /usr/local/bin/tcd --list-devices || true
    fi
    echo
    echo "Enter the serial number(s) of the AMBE device(s), separated by spaces."
    if [ "$TCD_DOCKER" = true ] && [ -n "$(built_codec_plugins)" ]; then
        # docker/build.sh gives the container the sticks for a tcd.ini with a DeviceSerial,
        # or one without a codec plugin (op25 and codec2 are tcd's own); next to a
        # plugin only the serials tell it
        echo "Next to a codec plugin the container gets the sticks only when tcd.ini names them."
        ask TCD_SERIALS "Serial numbers" "" '^ *[A-Za-z0-9]+( +[A-Za-z0-9]+)* *$' "At least one serial number, letters and digits."
    else
        echo "Leave empty to use every device tcd finds by a DVSI name (ThumbDV, DVstick, USB-30xx);"
        echo "a clone named FT232R_USB_UART or FT230X_Basic_UART needs its serial number."
        ask TCD_SERIALS "Serial numbers" "" '^[A-Za-z0-9 ]*$' "Letters and digits only."
    fi
}

start_tcd() {
    info "Starting the transcoder"
    systemctl daemon-reload
    systemctl enable tcd >/dev/null 2>&1
    systemctl restart tcd
}

# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------

summary() {
    echo
    bold "========================================================================"
    bold " Done"
    bold "========================================================================"
    if [ "$WITH_URFD" = true ]; then
        cat <<EOF

 Reflector $CALLSIGN
   Dashboard:      $DASHURL   (a TLS proxy in front must pass /ws as WebSocket)
   Configuration:  $CFG_DIR/urfd.ini   (more protocols, BrandMeister, interlinks)
   After changes:  docker restart urfd
   Logs:           docker logs -f urfd
EOF
        [ -n "$ADMIN_PASSWORD" ] && echo "   Admin page:     π symbol in the dashboard footer, password: $ADMIN_PASSWORD"
        if [ -n "$ADMIN_PASSWORD" ] && [[ "$DASHURL" == http://* ]]; then
            echo "   WARNING:        over plain http the admin password and its login token cross the"
            echo "                   network unencrypted: put a TLS proxy (Caddy, nginx) in front of port 8363"
        fi
        cat <<EOF

 Open these ports in your router / firewall (UDP unless noted):
   8363/tcp dashboard, 17000 M17, 42000 YSF, 8880 DMR+, 20001 DPlus,
   30001 DExtra, 30051 DCS, 41000 P25, 41400 NXDN, 10017 URF
   (62030 for DMR hotspots once you enable [MMDVM] in urfd.ini,
   62035 for a DMR network's server once you enable [OpenBridge])
EOF
        if [ "$TCD_REMOTE" = true ]; then
            echo "   10100/tcp for the transcoder, from the transcoder machine only"
            echo
            echo " Transcoder password: $TC_PASSWORD"
            echo "   enter it when you install tcd on the other machine (Password in tcd.ini)"
        fi
    fi
    if [ "$WITH_TCD" = true ] && [ "$TCD_DOCKER" = true ]; then
        cat <<EOF

 Transcoder in Docker (modules $TC_MODULES)
   Configuration:  $TCD_INI
   After changes:  docker restart tcd
   Logs:           docker logs -f tcd
   Monitor:        'docker exec -it tcd tcdmon' in a terminal, or http://localhost:8080 on this machine
EOF
        [ "$WITH_URFD" = true ] && echo "                   and ${DASHURL%/}/tcd/ (a TLS proxy in front passes /tcd/ like /ws)"
        echo "   Monitor password: ${TCD_MONITOR_PASSWORD:-see MonitorPassword in $TCD_INI}"
    elif [ "$WITH_TCD" = true ]; then
        cat <<EOF

 Transcoder (modules $TC_MODULES)
   Configuration:  $TCD_INI
   After changes:  systemctl restart tcd
   Logs:           journalctl -u tcd -f
   Monitor:        'tcdmon' in a terminal, or http://localhost:8080 on this machine
                   (MonitorBindAddress in tcd.ini opens it to the network)
   Monitor password: ${TCD_MONITOR_PASSWORD:-see MonitorPassword in $TCD_INI}
EOF
    fi
    cat <<EOF

 Update later with:  sudo $REPO_DIR/setup.sh update
 Setup log:          $LOG_FILE

EOF
}

# ---------------------------------------------------------------------------
# update
# ---------------------------------------------------------------------------

# Since urfd 4.3.0 the reflector registers itself in the XLX directory and the
# dashboard no longer does. Carry a dashboard registration over to urfd.ini once,
# so an updated reflector does not silently drop out of the directory. An
# existing [CallingHome] section in urfd.ini is the operator's choice and stays.
migrate_callinghome() {
    local ini="$CFG_DIR/urfd.ini" php="$DATA_DIR/dashboard/config.inc.php"
    [ -f "$ini" ] && [ -f "$php" ] || return 0
    grep -q '^\[CallingHome\]' "$ini" && return 0
    python3 - "$php" >"$DATA_DIR/.callinghome" <<'PY' || { rm -f "$DATA_DIR/.callinghome"; return 0; }
import re, sys
s = open(sys.argv[1]).read()
def get(key):
    m = re.search(r"^\s*\$CallingHome\['" + key + r"'\]\s*=\s*(?:'([^']*)'|\"([^\"]*)\"|(\w+))\s*;", s, re.M)
    return (m.group(1) or m.group(2) or m.group(3) or '') if m else ''
if get('Active').lower() != 'true':
    sys.exit(1)
for key in ('Country', 'Comment', 'MyDashBoardURL', 'OverrideIPAddress'):
    print(get(key))
PY
    local country comment url overrideip
    { read -r country; read -r comment; read -r url; read -r overrideip; } <"$DATA_DIR/.callinghome"
    rm -f "$DATA_DIR/.callinghome"
    info "Moving the XLX directory registration from the dashboard to the reflector"
    set_ini "$ini" CallingHome Enable true
    set_ini "$ini" CallingHome Country "$country"
    set_ini "$ini" CallingHome Comment "${comment//#/}"
    [ -n "$overrideip" ] && set_ini "$ini" CallingHome OverrideIP "$overrideip"
    # the directory entry keeps the URL it had
    [ -n "$url" ] && set_ini "$ini" Names DashboardUrl "$url"
}

# Since dashboard 3.0 (urfd 4.38) the dashboard reads everything from urfd's
# WebSocket, so it has to be on: a dashboard without it stays empty. Unlike
# before 4.38 an existing "Enable = false" is switched on as well: it came from
# the template of the time (the WebSocket served only the /live/ dashboard).
# Who wants no dashboard sets it back to false after the update.
migrate_websocket() {
    local ini="$CFG_DIR/urfd.ini"
    [ -f "$ini" ] || return 0
    if ! grep -q '^\[WebSocket\]' "$ini"; then
        info "Enabling the WebSocket for the dashboard ([WebSocket] in urfd.ini)"
        set_ini "$ini" WebSocket Enable true
        set_ini "$ini" WebSocket Port 10102
    elif python3 - "$ini" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
m = re.search(r'^\[WebSocket\]\s*$(.*?)(?=^\[|\Z)', s, re.M | re.S)
e = re.search(r'^\s*Enable\s*=\s*(\S+)', m.group(1), re.M) if m else None
sys.exit(0 if not e or e.group(1)[0] not in 'tT1yY' else 1)
PY
    then
        info "Enabling the WebSocket, the dashboard reads everything from it ([WebSocket] Enable in urfd.ini)"
        set_ini "$ini" WebSocket Enable true
    fi
}

# Since dashboard 3.0 (urfd 4.38) the dashboard is static and its settings are
# [Dashboard] in urfd.ini; urfd writes no XML file any more. Carry the settings
# of the old dashboard's config.inc.php over once, while there is no [Dashboard].
migrate_dashboard() {
    local ini="$CFG_DIR/urfd.ini" php="$DATA_DIR/dashboard/config.inc.php"
    [ -f "$ini" ] || return 0
    del_ini "$ini" Files XmlPath
    del_ini "$ini" Files JsonPath
    grep -q '^\[Dashboard\]' "$ini" && return 0
    info "Moving the dashboard settings to [Dashboard] in urfd.ini"
    python3 - "$php" >"$DATA_DIR/.dashboard" <<'PY' || true
import os, re, sys
s = open(sys.argv[1]).read() if os.path.isfile(sys.argv[1]) else ''
def get(key, default=''):
    # a key written with ' or " ($Admin["Enable"])
    k = re.escape(key).replace("'", "['\"]")
    m = re.search(r"^\s*\$" + k + r"\s*=\s*(?:'([^']*)'|\"([^\"]*)\"|([\w.]+))\s*;", s, re.M)
    return next((g for g in m.groups() if g is not None), default) if m else default
modes = ('HideIP', 'ShowLast1ByteOfIP', 'ShowLast2ByteOfIP', 'ShowLast3ByteOfIP', 'ShowFullIP')
# what the old dashboard showed; without its file, nothing (urfd's default HideIP)
def mode(key):
    if not s:
        return ''
    v = get(key, 'HideIP')
    return v if v in modes else 'HideIP'
def rows(key, default):
    v = get(key)
    return str(int(v) + 1) if v.isdigit() else default
mask = get("PageOptions['RepeatersPage']['MasqueradeCharacter']", '*')
print(get("PageOptions['MOTD']").replace('#', '').strip())
print(rows("PageOptions['LastHeardPage']['LimitTo']", '40'))
print('false' if get("PageOptions['UserPage']['ShowFilter']", 'true').lower() == 'false' else 'true')
print(rows("PageOptions['RepeatersPage']['LimitTo']", '100'))
print(rows("PageOptions['PeerPage']['LimitTo']", '100'))
print(mode("PageOptions['RepeatersPage']['IPModus']"))
print(mode("PageOptions['PeerPage']['IPModus']"))
print(mask if len(mask) == 1 and mask != '#' else '*')
print('true' if get("Admin['Enable']", 'false').lower() == 'true' else 'false')
PY
    local motd lastheard filter links peers linksip peersip mask admin
    { read -r motd; read -r lastheard; read -r filter; read -r links; read -r peers; read -r linksip; read -r peersip; read -r mask; read -r admin; } <"$DATA_DIR/.dashboard" || true
    rm -f "$DATA_DIR/.dashboard"
    [ -n "$motd" ] && set_ini "$ini" Dashboard Motd "$motd"
    set_ini "$ini" Dashboard LastHeard "${lastheard:-40}"
    set_ini "$ini" Dashboard Filter "${filter:-true}"
    set_ini "$ini" Dashboard Links "${links:-100}"
    set_ini "$ini" Dashboard Peers "${peers:-100}"
    [ -n "$linksip" ] && set_ini "$ini" Dashboard LinksIP "$linksip"
    [ -n "$peersip" ] && set_ini "$ini" Dashboard PeersIP "$peersip"
    set_ini "$ini" Dashboard IPMask "${mask:-*}"
    # the admin page as before: the old dashboard had a switch of its own for it
    set_ini "$ini" Dashboard Admin "${admin:-false}"
    if [ -f "$php" ]; then
        # dashboards before 2.8.2 kept a copy of the admin password in there
        sed -E -i "/^[[:space:]]*\\\$Admin\\[[\"']Password[\"']\\]/d" "$php"
        mv "$php" "$php.old"
        chmod 600 "$php.old"
        info "The old dashboard settings are kept as $php.old, they are not used any more"
    fi
}

update() {
    local have_urfd=false have_tcd=false
    [ -f "$CFG_DIR/urfd.ini" ] && have_urfd=true
    [ -f "$TCD_INI" ] && have_tcd=true
    [ "$have_urfd" = true ] || [ "$have_tcd" = true ] || die "nothing installed yet, run 'sudo ./setup.sh' first"

    # the project moved from jcmerg/urfd (archived) to jcmerg/urfd-ng, same history
    local origin
    origin=$(repo_git remote get-url origin 2>/dev/null || true)
    case "$origin" in
        *github.com[:/]jcmerg/urfd|*github.com[:/]jcmerg/urfd.git|*github.com[:/]jcmerg/urfd/)
            info "The project moved to jcmerg/urfd-ng, switching the checkout over"
            repo_git remote set-url origin "$(echo "${origin%/}" | sed 's#jcmerg/urfd\(\.git\)\{0,1\}$#jcmerg/urfd-ng.git#')"
            ;;
    esac

    info "Updating the source in $REPO_DIR"
    repo_git pull --ff-only
    pull_plugins

    if [ "$have_urfd" = true ]; then
        install_docker
        migrate_callinghome
        migrate_websocket
        migrate_dashboard
        secure_files
    fi
    if [ "$have_urfd" = true ] || [ "$TCD_DOCKER" = true ]; then
        install_docker
        start_docker   # builds and restarts urfd and/or tcd in Docker
    fi
    if [ "$have_tcd" = true ] && [ "$TCD_DOCKER" != true ]; then
        build_tcd   # restarts tcd if it was running
        systemctl is-active --quiet tcd || systemctl start tcd
    fi
    info "Update complete"
}

# ---------------------------------------------------------------------------
# day-to-day commands: status, start, stop, restart, logs, reload, check
# ---------------------------------------------------------------------------

has_urfd() { command -v docker >/dev/null && docker container inspect urfd >/dev/null 2>&1; }
has_tcd()  {
    if [ "$TCD_DOCKER" = true ]; then command -v docker >/dev/null && docker container inspect tcd >/dev/null 2>&1
    else systemctl cat tcd >/dev/null 2>&1; fi
}

# which of urfd/tcd a command acts on: the one named, or every one installed
targets() {
    case "${1:-}" in
        urfd) has_urfd || die "the reflector (Docker container 'urfd') is not installed here"; echo urfd ;;
        tcd)  has_tcd  || die "the transcoder (Docker container or systemd service 'tcd') is not installed here";  echo tcd ;;
        "")   has_urfd && echo urfd; has_tcd && echo tcd; has_urfd || has_tcd || die "neither urfd nor tcd is installed here" ;;
        *)    die "unknown target '$1', use urfd or tcd" ;;
    esac
}

cmd_status() {
    local t list
    list=$(targets "${1:-}")
    for t in $list; do
        if [ "$t" = urfd ]; then
            bold "Reflector (Docker container urfd)"
            docker ps -a -f name='^urfd$' --format '{{.Status}}' | sed 's/^/  /'
            docker logs urfd 2>&1 | grep 'Starting URF' | tail -1 | sed 's/^/  /' || true
            echo "  Configuration: $CFG_DIR/urfd.ini"
        elif [ "$TCD_DOCKER" = true ]; then
            bold "Transcoder (Docker container tcd)"
            docker ps -a -f name='^tcd$' --format '{{.Status}}' | sed 's/^/  /'
            echo "  Configuration: $TCD_INI"
        else
            bold "Transcoder (systemd service tcd)"
            echo "  $(systemctl is-active tcd) since $(systemctl show -p ActiveEnterTimestamp --value tcd)"
            echo "  Configuration: $TCD_INI"
        fi
    done
    echo
    echo "Source: $REPO_DIR ($(repo_git log -1 --format='%h %s' 2>/dev/null || echo 'not a git checkout'))"
}

# start, stop or restart
cmd_service() {
    local action=$1 t list
    list=$(targets "${2:-}")
    for t in $list; do
        info "${action^} $t"
        if [ "$t" = urfd ] || [ "$TCD_DOCKER" = true ]; then docker "$action" "$t" >/dev/null; else systemctl "$action" tcd; fi
    done
}

cmd_logs() {
    local t list
    list=$(targets "${1:-}")
    t=${list%%$'\n'*}
    [ -z "${1:-}" ] && has_urfd && has_tcd && echo "(reflector log; 'setup.sh logs tcd' for the transcoder)"
    if [ "$t" = urfd ] || [ "$TCD_DOCKER" = true ]; then docker logs -f --tail 100 "$t"; else journalctl -u tcd -f -n 100; fi
}

# SIGHUP rereads TG mappings, whitelist, blacklist and interlink. It goes to urfd itself:
# PID 1 in the container is supervisord, which restarts everything on SIGHUP.
cmd_reload() {
    targets urfd >/dev/null
    docker exec urfd pkill -HUP -x urfd || die "urfd is not running"
    info "urfd reloads its configuration (docker logs urfd shows the result)"
}

cmd_check() {
    targets urfd >/dev/null
    if docker run --rm -v "$CFG_DIR:/usr/local/etc/urfd:ro" urfd:latest inicheck -q /usr/local/etc/urfd/urfd.ini; then
        info "$CFG_DIR/urfd.ini has no errors"
    else
        die "$CFG_DIR/urfd.ini has errors (see above)"
    fi
}

usage() {
    cat <<EOF
Usage: sudo $0 [command] [urfd|tcd]

  (none)            install, or offer an update on an installed machine
  update            pull the latest version and rebuild, keeping the configuration
  status            show what runs here and in which version
  start|stop|restart [urfd|tcd]
  logs [urfd|tcd]   follow the log (Ctrl-C ends it)
  reload            urfd rereads its config: TG mappings, lists, interlink (SIGHUP)
  check             check urfd.ini with inicheck

Without urfd or tcd, start/stop/restart/status act on everything installed here.
EOF
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

main() {
    case "${1:-}" in
        ""|update) ;;
        help|-h|--help) usage; return ;;
        status|start|stop|restart|logs|reload|check)
            check_system
            case "$1" in
                status)              cmd_status "${2:-}" ;;
                start|stop|restart)  cmd_service "$1" "${2:-}" ;;
                logs)                cmd_logs "${2:-}" ;;
                reload)              cmd_reload ;;
                check)               cmd_check ;;
            esac
            return ;;
        *) usage >&2; exit 1 ;;
    esac

    check_system
    # the log ends up holding the admin password
    touch "$LOG_FILE" && chmod 600 "$LOG_FILE"
    exec > >(tee -a "$LOG_FILE") 2>&1
    echo "---- setup.sh $(date -Is) ----" >>"$LOG_FILE"
    install_base_packages

    if [ "${1:-}" = "update" ]; then
        update
        return
    fi
    if [ -f "$CFG_DIR/urfd.ini" ] || [ -f "$TCD_INI" ]; then
        echo
        echo "An existing installation was found."
        if ask_yn "Update it, keeping the configuration?" y; then
            update
            return
        fi
        ask_yn "Set it up again? This overwrites urfd.ini and tcd.ini." n || exit 0
    fi

    bold "urfd / tcd setup"
    ask_mode
    [ "$WITH_URFD" = true ] && ask_reflector
    [ "$WITH_TCD" = true ] && ask_transcoder

    if [ "$WITH_URFD" = true ]; then
        install_docker
        write_urfd_config
        build_urfd
        check_urfd_config
    fi
    if [ "$WITH_TCD" = true ]; then
        build_tcd
        ask_tcd_serials
        write_tcd_config
    fi
    if [ "$WITH_URFD" = true ] || [ "$TCD_DOCKER" = true ]; then
        start_docker
    fi
    if [ "$WITH_TCD" = true ] && [ "$TCD_DOCKER" != true ]; then
        start_tcd
    fi
    summary
}

main "$@"
