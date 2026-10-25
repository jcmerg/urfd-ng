#!/bin/bash
# Rebuild the images from this checkout and restart the containers, for what this
# machine has a configuration for in $URFD_DATA (default /opt/urfd):
#
#   urfd   config/urfd.ini
#   tcd    tcd/tcd.ini (tcd in Docker, see "tcd in Docker" in tcd/README.md); with a
#          DeviceSerial or without a CodecPlugin it uses AMBE sticks and the
#          container gets them (docker-compose.tcd-sticks.yml)
#
# So the same script serves a reflector, a reflector with tcd and a machine with
# only tcd. Unchanged sources come from the build cache.
#
# Usage:
#   docker/build.sh          # rebuild, restart what runs
#   docker/build.sh start    # rebuild and start
set -e

cd "$(dirname "${BASH_SOURCE[0]}")"

DATA="${URFD_DATA:-/opt/urfd}"
TCD_INI="$DATA/tcd/tcd.ini"
FILES=(-f docker-compose.yml)
SERVICES=()

[ -f "$DATA/config/urfd.ini" ] && SERVICES+=(urfd)
if [ -f "$TCD_INI" ]; then
    SERVICES+=(tcd)
    if grep -qE '^[[:space:]]*DeviceSerial[[:space:]]*=' "$TCD_INI" || ! grep -qE '^[[:space:]]*CodecPlugin[[:space:]]*=' "$TCD_INI"; then
        echo "=== tcd with AMBE sticks: the container gets /dev (docker-compose.tcd-sticks.yml) ==="
        FILES+=(-f docker-compose.tcd-sticks.yml)
    fi
    # the dashboard's /tcd/ goes to the monitor port of this tcd.ini
    port=$(sed -n 's/^[[:space:]]*MonitorHttpPort[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$TCD_INI" | tail -1)
    export TCD_MONITOR_PORT="${TCD_MONITOR_PORT:-${port:-8080}}"
fi
[ ${#SERVICES[@]} -gt 0 ] || { echo "Neither $DATA/config/urfd.ini nor $TCD_INI exists, nothing to build"; exit 1; }

echo "=== Building ${SERVICES[*]} ==="
docker compose "${FILES[@]}" --profile tcd build "${SERVICES[@]}"

running=false
for s in "${SERVICES[@]}"; do
    docker ps -q -f name="^$s\$" | grep -q . && running=true
done
if [ "$running" = true ] || [ "$1" = "start" ]; then
    echo "=== (Re)starting ${SERVICES[*]} ==="
    docker compose "${FILES[@]}" --profile tcd up -d --force-recreate "${SERVICES[@]}"
else
    echo "Not running. Run '$0 start' to start ${SERVICES[*]}."
fi

docker ps --filter "name=^($(IFS='|'; echo "${SERVICES[*]}"))\$"
