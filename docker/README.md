# urfd Docker Deployment

Docker deployment for the urfd reflector.

The tcd transcoder (in [`../tcd`](../tcd/)) runs natively when it uses DVSI AMBE sticks: Docker adds too much USB latency for reliable hardware access. Without sticks, in software with a codec plugin, it can run here too: `Dockerfile.tcd` and the `tcd` service behind the profile `tcd` (`docker compose --profile tcd up -d`), see "tcd in Docker" in [tcd/README.md](../tcd/README.md#tcd-in-docker). `build.sh` builds and restarts it too when `$URFD_DATA/tcd/tcd.ini` exists, and nginx passes `/tcd/` to its monitor (`TCD_MONITOR_PORT`, which `build.sh` takes from the `MonitorHttpPort` of that tcd.ini, default 8080).

## Quick Start

The easiest way is [`setup.sh`](../setup.sh) in the repository root, see [docs/INSTALL.md](../docs/INSTALL.md). It creates the configuration and runs the steps below; the configuration by hand is in [docs/INSTALL-ADVANCED.md](../docs/INSTALL-ADVANCED.md#reflector-in-docker).

```bash
# Build and start
docker/build.sh start

# Or manually
cd docker
docker compose build
docker compose up -d
```

## Configuration

The image is built from this checkout; configuration and state live outside it, in `$URFD_DATA` (default `/opt/urfd`), so a `git pull` never touches them:

| Host path | In the container | Content |
|---|---|---|
| `$URFD_DATA/config/` | `/usr/local/etc/urfd` | `urfd.ini`, interlink, black/whitelist, downloaded ID databases |
| `$URFD_DATA/dashboard/config.inc.php` | dashboard `pgs/config.inc.php` | Dashboard settings |
| `$URFD_DATA/xlxd-ch/` | `/var/lib/xlxd-ch` | Reflector list cache of the dashboard |
| `$URFD_DATA/tcd/` | `/usr/local/etc/tcd` (tcd service) | `tcd.ini`, only with `--profile tcd` |

`urfd.ini` must exist. See the main [README](../README.md) and the commented templates in [config/](../config/) for details.

### PHP tuning

The image changes three stock PHP settings:

| Setting | Stock | Here | Why |
|---|---|---|---|
| `pm.max_children` | 5 | 25 | Five workers are exhausted by a handful of slow requests, which stalls every other PHP request including the admin API |
| `default_socket_timeout` | 60 | 10 | Caps how long an unreachable upstream can hold a worker |
| `session.upload_progress.enabled` | On | Off | The dashboard takes no uploads; one less path into php2shell (CVE-2026-109618/-109619) |

The build also upgrades all packages and fails if `php8.3-fpm` is older than
`8.3.6-0ubuntu0.24.04.13`, the Ubuntu build with the php2shell fix (USN-8948-1),
and nginx answers multipart requests to PHP with 415, as the dashboard takes no uploads.

Note that `supervisorctl` is not usable inside the container — `supervisord.conf`
defines no `unix_http_server` socket. Restart with `docker restart urfd`.

## Files

| File | Purpose |
|---|---|
| `Dockerfile` | Multi-stage build (Ubuntu 24.04) |
| `docker-compose.yml` | Compose file, builds from this checkout, data in `$URFD_DATA` |
| `Dockerfile.dockerignore` | Keeps `.git`, `tcd/` and build output out of the build context |
| `build.sh` | Build + restart helper, the tcd service too when `$URFD_DATA/tcd/tcd.ini` exists |
| `supervisord.conf` | Process manager (urfd, nginx, php-fpm) |
| `nginx-dashboard.conf` | Dashboard web server config: the PHP dashboard at `/`, the live dashboard (`dashboard-ws`) at `/live/`, urfd's WebSocket at `/ws`, the monitor of the Docker tcd at `/tcd/` (`TCD_MONITOR_PORT`, default 8080) |

## Synology NAS

The Synology Container Manager cannot build images locally. Build on a Linux machine and export:

```bash
docker build -t urfd:latest -f docker/Dockerfile --build-context optfiles=docker .
docker save urfd:latest | gzip > urfd-image.tar.gz
```

Import on the NAS via Container Manager > Image > Import, or via SSH:

```bash
docker load < urfd-image.tar.gz
```

Adjust volume paths for NAS (e.g. `/volume1/docker/urfd/config:/usr/local/etc/urfd`).
