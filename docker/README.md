# urfd Docker Deployment

Docker deployment for the urfd reflector.

The tcd transcoder (in [`../tcd`](../tcd/)) runs here too, with AMBE sticks or in software with a codec plugin: `Dockerfile.tcd` and the `tcd` service behind the profile `tcd`, see "tcd in Docker" in [tcd/README.md](../tcd/README.md#tcd-in-docker). `build.sh` builds and starts what this machine has a configuration for (urfd with `$URFD_DATA/config/urfd.ini`, tcd with `$URFD_DATA/tcd/tcd.ini`), with sticks it adds `docker-compose.tcd-sticks.yml`; nginx passes `/tcd/` to the tcd monitor (`TCD_MONITOR_PORT`, which `build.sh` takes from the `MonitorHttpPort` of that tcd.ini, default 8080).

## Quick Start

The easiest way is [`setup.sh`](../setup.sh) in the repository root, see [docs/INSTALL.md](../docs/INSTALL.md). It creates the configuration and runs the steps below; the configuration by hand is in [docs/INSTALL-ADVANCED.md](../docs/INSTALL-ADVANCED.md#reflector-in-docker).

```bash
# Build and start what this machine has a configuration for
docker/build.sh start

# Or manually, naming the service
cd docker
docker compose build urfd && docker compose up -d urfd                                      # urfd only
docker compose --profile tcd build tcd && docker compose --profile tcd up -d tcd            # tcd only (software)
docker compose -f docker-compose.yml -f docker-compose.tcd-sticks.yml --profile tcd up -d tcd   # tcd with AMBE sticks
```

`build.sh` decides by the files in `$URFD_DATA`: `config/urfd.ini` builds urfd, `tcd/tcd.ini` builds tcd, both build both; on a transcoder machine it builds tcd alone. Which build script does what (`docker/build.sh`, `reflector/build.sh`, `tcd/build.sh`) and why each part runs one way only: [docs/INSTALL-ADVANCED.md](../docs/INSTALL-ADVANCED.md#what-gets-built).

## Configuration

The image is built from this checkout; configuration and state live outside it, in `$URFD_DATA` (default `/opt/urfd`), so a `git pull` never touches them:

| Host path | In the container | Content |
|---|---|---|
| `$URFD_DATA/config/` | `/usr/local/etc/urfd` | `urfd.ini` (with the dashboard's `[Dashboard]`), interlink, black/whitelist, downloaded ID databases |
| `$URFD_DATA/xlxd-ch/` | `/var/lib/xlxd-ch` | nginx's copy of the XLX directory's reflector list |
| `$URFD_DATA/tcd/` | `/usr/local/etc/tcd` (tcd service) | `tcd.ini`, only with `--profile tcd` |

`urfd.ini` must exist. See the main [README](../README.md) and the commented templates in [config/](../config/) for details.

### Dashboard

The dashboard is static and has no PHP: nginx serves its files, passes `/ws` to urfd's WebSocket and fetches the XLX directory's reflector list (kept in `/var/lib/xlxd-ch/nginx-cache`, fresh for 5 minutes, an older copy while the directory is down) and the QuadNet feed. At start the container puts the system's resolver into the line marked `RESOLVER` of `nginx-dashboard.conf`, so nginx starts without DNS. Before urfd 4.38 the image had php-fpm and a volume `urfd-xml` for the status file; the volume is no longer used (`docker volume rm urfd_urfd-xml`).

Note that `supervisorctl` is not usable inside the container — `supervisord.conf`
defines no `unix_http_server` socket. Restart with `docker restart urfd`.

## Files

| File | Purpose |
|---|---|
| `Dockerfile` | Multi-stage build (Ubuntu 24.04) |
| `docker-compose.yml` | Compose file, builds from this checkout, data in `$URFD_DATA` |
| `Dockerfile.dockerignore` | Keeps `.git`, `tcd/` and build output out of the build context |
| `build.sh` | Build + restart helper for urfd and/or tcd, whichever has its configuration in `$URFD_DATA` |
| `docker-compose.tcd-sticks.yml` | Added by `build.sh` for tcd with AMBE sticks: `/dev` read only and the USB serial devices |
| `supervisord.conf` | Process manager (urfd, nginx) |
| `nginx-dashboard.conf` | Dashboard web server config: the static dashboard at `/`, urfd's WebSocket at `/ws`, the reflector list and QuadNet proxies, the monitor of the Docker tcd at `/tcd/` (`TCD_MONITOR_PORT`, default 8080) |

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
