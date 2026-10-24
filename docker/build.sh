#!/bin/bash
# Rebuild the urfd image from this checkout and restart the container.
#
# Usage:
#   docker/build.sh          # rebuild, restart if running
#   docker/build.sh start    # rebuild and start
#
# Configuration stays in $URFD_DATA (default /opt/urfd), see docker-compose.yml.
set -e

cd "$(dirname "${BASH_SOURCE[0]}")"

echo "=== Building urfd Docker image ==="
docker compose build

if docker ps -q -f name=^urfd$ | grep -q . || [ "$1" = "start" ]; then
    echo "=== (Re)starting urfd container ==="
    docker compose up -d --force-recreate
else
    echo "Container not running. Run '$0 start' to start it."
fi

docker ps -f name=^urfd$
