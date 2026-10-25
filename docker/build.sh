#!/bin/bash
# Rebuild the urfd image from this checkout and restart the container; the tcd
# image and container too when $URFD_DATA/tcd/tcd.ini exists (tcd in Docker, see
# "tcd in Docker" in tcd/README.md). Unchanged sources come from the build cache.
#
# Usage:
#   docker/build.sh          # rebuild, restart if running
#   docker/build.sh start    # rebuild and start
#
# Configuration stays in $URFD_DATA (default /opt/urfd), see docker-compose.yml.
set -e

cd "$(dirname "${BASH_SOURCE[0]}")"

PROFILE=()
NAMES="^urfd$"
TCD_INI="${URFD_DATA:-/opt/urfd}/tcd/tcd.ini"
if [ -f "$TCD_INI" ]; then
    PROFILE=(--profile tcd)
    NAMES="^(urfd|tcd)$"
    # the dashboard's /tcd/ goes to the monitor port of this tcd.ini
    port=$(sed -n 's/^[[:space:]]*MonitorHttpPort[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$TCD_INI" | tail -1)
    export TCD_MONITOR_PORT="${TCD_MONITOR_PORT:-${port:-8080}}"
    echo "=== Building the urfd and tcd Docker images ==="
else
    echo "=== Building urfd Docker image ==="
fi
docker compose "${PROFILE[@]}" build

if docker ps -q -f name=^urfd$ | grep -q . || [ "$1" = "start" ]; then
    echo "=== (Re)starting the containers ==="
    docker compose "${PROFILE[@]}" up -d --force-recreate
else
    echo "Container not running. Run '$0 start' to start it."
fi

docker ps --filter "name=$NAMES"
