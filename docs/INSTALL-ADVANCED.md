# Installing urfd and tcd by hand

**English** | [Deutsch](INSTALL-ADVANCED.de.md)

This guide is for people who know their way around Linux and want to set things up themselves: the reflector in Docker without the setup script, the reflector without Docker, the dashboard on their own web server, the transcoder alone. The easy way is [`setup.sh`](../setup.sh), see [INSTALL.md](INSTALL.md); it does the Docker steps below for you.

## Contents

- [Overview](#overview)
- [What gets built](#what-gets-built)
- [Reflector in Docker](#reflector-in-docker)
- [Reflector without Docker](#reflector-without-docker)
- [Dashboard without Docker](#dashboard-without-docker)
- [Transcoder](#transcoder)
- [Updates](#updates)

## Overview

| | Docker (as `setup.sh` does it) | Without Docker |
|---|---|---|
| urfd, `inicheck`, `dbutil` | in the `urfd` image, built from the checkout | `/usr/local/bin`, from `reflector/build.sh` |
| Configuration | `$URFD_DATA/config/` (default `/opt/urfd/config`), mounted as `/usr/local/etc/urfd` | `/usr/local/etc/urfd/` |
| Dashboard | in the container (nginx + php-fpm, port 8363), settings in `$URFD_DATA/dashboard/config.inc.php` | your web server, e.g. `/var/www/urfd-dashboard` |
| Service | container `urfd`, `restart: unless-stopped` | systemd unit `urfd` |
| tcd | container `tcd` with `$URFD_DATA/tcd/tcd.ini` (`docker/build.sh`, see [tcd in Docker](../tcd/README.md#tcd-in-docker)) | systemd unit `tcd`, from `tcd/build.sh` |

Requirements: Debian 12, Ubuntu 22.04 or newer, or Raspberry Pi OS; x86_64, arm64 or armhf. The ports to open are listed in [INSTALL.md](INSTALL.md#ports).

The templates in [`config/`](../config/) explain every key. Before starting, check `urfd.ini` with `inicheck -q urfd.ini`: it prints warnings and errors and fails on errors.

## What gets built

Not sure what you need? Use [`setup.sh`](../setup.sh), see [INSTALL.md](INSTALL.md): it asks what runs on this machine and builds exactly that. The rest of this section is for setting it up yourself.

There are three build scripts, one per way of running things:

| Script | Builds | Runs as | Configuration |
|---|---|---|---|
| `docker/build.sh` | urfd (with the dashboard and the plugins in `plugins/`) and/or tcd (with the codec plugins in `tcd/plugins/`) as images | containers `urfd` and `tcd` | `$URFD_DATA` (default `/opt/urfd`) |
| `reflector/build.sh` | urfd, `inicheck`, `dbutil` | systemd unit `urfd` | `/usr/local/etc/urfd/` |
| `tcd/build.sh` | tcd, `tcdmon`, the codec plugins | systemd unit `tcd` | `/usr/local/etc/tcd.ini` |

`docker/build.sh` has no switches: it builds what this machine has a configuration for.

| In `$URFD_DATA` | `docker/build.sh` builds and starts |
|---|---|
| `config/urfd.ini` | urfd |
| `tcd/tcd.ini` | tcd; with a `DeviceSerial` or without a `CodecPlugin` it adds `docker-compose.tcd-sticks.yml`, so the container gets the AMBE sticks |
| both | both |

So a reflector machine gets `config/urfd.ini` ([Reflector in Docker](#reflector-in-docker)), a transcoder machine only `tcd/tcd.ini` ([tcd in Docker](../tcd/README.md#tcd-in-docker)), and `docker/build.sh start` builds the right thing on each. Another data directory: `URFD_DATA=/srv/urfd docker/build.sh start`.

The same with `docker compose`, naming the service yourself (in `docker/`):

```bash
sudo docker compose build urfd && sudo docker compose up -d urfd                     # urfd only
sudo docker compose --profile tcd build tcd && sudo docker compose --profile tcd up -d tcd   # tcd in software
sudo docker compose -f docker-compose.yml -f docker-compose.tcd-sticks.yml --profile tcd up -d tcd   # tcd with sticks
```

tcd sits behind the profile `tcd`, and the sticks file has to be named by hand. Next to tcd, give the urfd container `TCD_MONITOR_PORT=<MonitorHttpPort of tcd.ini>` for the dashboard's `/tcd/` (default 8080; `docker/build.sh` reads it from `tcd.ini`).

Run each part one way only: tcd either as a container or with `tcd/build.sh`, urfd either as a container or with `reflector/build.sh`. `tcd/build.sh` enables its systemd unit, so on a machine with the tcd container it would start a second tcd at the next boot; undo it with `sudo systemctl disable --now tcd`. Updating keeps to the way it was installed, see [Updates](#updates).

## Reflector in Docker

The image is built from the checkout; configuration and state stay outside it in `$URFD_DATA`, so a `git pull` never touches them. Details on the image: [docker/README.md](../docker/README.md).

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd

# configuration, the paths in urfd.ini are the ones inside the container
sudo mkdir -p /opt/urfd/config /opt/urfd/dashboard /opt/urfd/xlxd-ch
sudo cp config/urfd.ini config/urfd.interlink config/urfd.whitelist config/urfd.blacklist config/urfd.terminal /opt/urfd/config/
sudo sed -i 's#/home/user/#/usr/local/etc/urfd/#' /opt/urfd/config/urfd.ini
sudo cp dashboard/pgs/config.inc.php /opt/urfd/dashboard/

# urfd.ini holds passwords; the dashboard (www-data, uid 33) owns its config and cache
sudo chmod 600 /opt/urfd/config/urfd.ini
sudo chown 33:33 /opt/urfd/dashboard/config.inc.php /opt/urfd/xlxd-ch
```

Edit `/opt/urfd/config/urfd.ini` (at least `[Names]`, `[IP Addresses]`, `[Modules]`, `[Transcoder]`) and `/opt/urfd/dashboard/config.inc.php`, then build and start:

```bash
sudo docker/build.sh start     # or: cd docker && sudo docker compose build && sudo docker compose up -d
sudo docker exec urfd inicheck -q /usr/local/etc/urfd/urfd.ini
sudo docker logs -f urfd
```

Another data directory: `sudo URFD_DATA=/srv/urfd docker/build.sh start`, the same for `docker compose`. The container uses the host network, so the ports in `urfd.ini` are the ports on the host; the dashboard listens on 8363.

Reload after changing mappings or lists (what a reload covers: [README](../README.md#sighup-configuration-reload)):

```bash
sudo docker exec urfd pkill -HUP -x urfd   # not docker kill -s HUP: that reaches supervisord
sudo docker restart urfd                   # everything else
```

## Reflector without Docker

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd
sudo bash reflector/build.sh            # with OpenDHT
sudo bash reflector/build.sh --no-dht   # where libopendht-dev is not available
```

`build.sh` installs the build packages (`build-essential`, `nlohmann-json3-dev`, `libcurl4-gnutls-dev`, `libopus-dev`, `libssl-dev`, `libwebsockets-dev`, `libopendht-dev`, and `pkg-config`, `libsqlite3-dev`, `libmariadb-dev`, `libpq-dev` for the credential database), builds as the owner of the checkout and installs `urfd`, `inicheck` and `dbutil` to `/usr/local/bin` and the systemd unit `urfd`, with realtime scheduling. The first run puts the templates into `/usr/local/etc/urfd/` with the paths pointing there and `urfd.ini` readable by root only; later runs keep the configuration.

The build options are in `reflector/urfd.mk`, copied from [`config/urfd.mk`](../config/urfd.mk) at the first build and kept afterwards: `BINDIR`, `debug`, `DHT`.

```bash
sudo nano /usr/local/etc/urfd/urfd.ini
sudo inicheck -q /usr/local/etc/urfd/urfd.ini
sudo systemctl start urfd
journalctl -u urfd -f
```

Reload: `sudo pkill -HUP -x urfd`, restart: `sudo systemctl restart urfd`. To remove: `cd reflector && sudo make uninstall`, then delete `/usr/local/etc/urfd`.

By hand, with the packages installed:

```bash
cd urfd/reflector
cp ../config/urfd.mk ../config/urfd.service .
make -j$(nproc)          # DHT=false without OpenDHT
sudo make install        # urfd and the unit only, and starts it; copy inicheck, dbutil and the configuration first
```

## Dashboard without Docker

The dashboard reads the reflector's status file (`[Files] XmlPath`, default `/var/log/xlxd.xml`) and talks to the admin socket (`[Admin]`, port 10101), so it runs on the reflector's machine. It needs PHP with a web server; the shipped nginx configuration also routes the [JSON API](../README.md#json-api).

```bash
sudo apt install nginx php-fpm
sudo cp -r ~/urfd/dashboard /var/www/urfd-dashboard
sudo cp -r ~/urfd/dashboard-ws/static /var/www/urfd-dashboard-ws   # the live dashboard at /live/
sudo mkdir -p /var/lib/xlxd-ch
sudo chown -R www-data:www-data /var/www/urfd-dashboard /var/lib/xlxd-ch

sudo cp ~/urfd/docker/nginx-dashboard.conf /etc/nginx/sites-available/urfd-dashboard.conf
# the configuration is for PHP 8.3 (Ubuntu 24.04); use the socket this system has, e.g. php8.2 on Debian 12
PHPSOCK=$(ls /run/php/php[0-9]*-fpm.sock)
sudo sed -i "s#/var/run/php/php8.3-fpm.sock#$PHPSOCK#" /etc/nginx/sites-available/urfd-dashboard.conf
sudo ln -s /etc/nginx/sites-available/urfd-dashboard.conf /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
```

The dashboard is then on port 8363; change `listen` in the configuration for another port, or put it behind your existing web server. The Docker image also raises `pm.max_children` and lowers `default_socket_timeout` in PHP, see [docker/README.md](../docker/README.md#php-tuning); worth doing on a busy reflector. Settings are in `/var/www/urfd-dashboard/pgs/config.inc.php`; keep a copy of it for updates. The live dashboard at `/live/` needs `[WebSocket] Enable = true` in `urfd.ini`; the configuration passes `/ws` to it, see [WEBSOCKET.md](WEBSOCKET.md).

**Admin page**: `$Admin['Enable'] = true` in the dashboard and `[Admin] Enable = true` in `urfd.ini` (restart urfd). Log in with the `[Admin] Password` from `urfd.ini`; the dashboard keeps no copy of it. The page is behind the π symbol in the footer.

**Apache** works as well, but needs `mod_rewrite` and `AllowOverride FileInfo` for the JSON API, see the [README](../README.md#json-api).

## Transcoder

tcd runs natively with systemd (below) or in Docker ([tcd in Docker](../tcd/README.md#tcd-in-docker)), on the reflector's machine or on another one; it connects to the reflector's `[Transcoder]` port (10100/tcp).

```bash
sudo bash tcd/build.sh
sudo nano /usr/local/etc/tcd.ini     # ServerAddress, Port, Password, Modules, devices
sudo systemctl start tcd
```

The modules in `tcd.ini` must match `[Transcoder] Modules` in `urfd.ini`, or that says `auto` and tcd decides alone (urfd 4.25.0). If tcd runs on another machine, set the same long random `Password` in `tcd.ini` and in `[Transcoder]` of `urfd.ini` (no `#`, no spaces): without it, anyone who reaches port 10100 can take over the transcoded modules. Devices, gains, AGC and monitoring: [tcd/README.md](../tcd/README.md).

## Updates

```bash
cd ~/urfd && git pull
```

| Installed with | Then |
|---|---|
| `setup.sh` | `sudo ./setup.sh update` |
| Docker by hand | `sudo docker/build.sh` (rebuilds, restarts a running container) |
| `reflector/build.sh` | `sudo bash reflector/build.sh` (restarts a running urfd after checking `urfd.ini`) |
| Dashboard by hand | save `pgs/config.inc.php`, `sudo cp -r ~/urfd/dashboard/. /var/www/urfd-dashboard/`, carry your settings over into the new `pgs/config.inc.php` |
| `tcd/build.sh` | `sudo bash tcd/build.sh` (restarts a running tcd) |

New keys show up in the templates in `config/` and `tcd/config/`; `inicheck -q` shows what is missing or wrong in `urfd.ini`.
