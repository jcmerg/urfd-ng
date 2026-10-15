# Installing urfd and tcd by hand

**English** | [Deutsch](INSTALL-ADVANCED.de.md)

This guide is for people who know their way around Linux and want to set things up themselves: the reflector in Docker without the setup script, the reflector without Docker, the dashboard on their own web server, the transcoder alone. The easy way is [`setup.sh`](../setup.sh), see [INSTALL.md](INSTALL.md); it does the Docker steps below for you.

## Contents

- [Overview](#overview)
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
| tcd | natively with systemd, in both cases (`tcd/build.sh`) | |

Requirements: Debian 12, Ubuntu 22.04 or newer, or Raspberry Pi OS; x86_64, arm64 or armhf. The ports to open are listed in [INSTALL.md](INSTALL.md#ports).

The templates in [`config/`](../config/) explain every key. Before starting, check `urfd.ini` with `inicheck -q urfd.ini`: it prints warnings and errors and fails on errors.

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

`build.sh` installs the build packages (`build-essential`, `nlohmann-json3-dev`, `libcurl4-gnutls-dev`, `libopus-dev`, `libssl-dev`, `libwebsockets-dev`, `libopendht-dev`), builds as the owner of the checkout and installs `urfd`, `inicheck` and `dbutil` to `/usr/local/bin` and the systemd unit `urfd`, with realtime scheduling. The first run puts the templates into `/usr/local/etc/urfd/` with the paths pointing there and `urfd.ini` readable by root only; later runs keep the configuration.

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
sudo mkdir -p /var/lib/xlxd-ch
sudo chown -R www-data:www-data /var/www/urfd-dashboard /var/lib/xlxd-ch

sudo cp ~/urfd/docker/nginx-dashboard.conf /etc/nginx/sites-available/urfd-dashboard.conf
# the configuration is for PHP 8.3 (Ubuntu 24.04); use the socket this system has, e.g. php8.2 on Debian 12
PHPSOCK=$(ls /run/php/php[0-9]*-fpm.sock)
sudo sed -i "s#/var/run/php/php8.3-fpm.sock#$PHPSOCK#" /etc/nginx/sites-available/urfd-dashboard.conf
sudo ln -s /etc/nginx/sites-available/urfd-dashboard.conf /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
```

The dashboard is then on port 8363; change `listen` in the configuration for another port, or put it behind your existing web server. The Docker image also raises `pm.max_children` and lowers `default_socket_timeout` in PHP, see [docker/README.md](../docker/README.md#php-tuning); worth doing on a busy reflector. Settings are in `/var/www/urfd-dashboard/pgs/config.inc.php`; keep a copy of it for updates.

**Admin page**: `$Admin['Enable'] = true` in the dashboard and `[Admin] Enable = true` in `urfd.ini` (restart urfd). Log in with the `[Admin] Password` from `urfd.ini`; the dashboard keeps no copy of it. The page is behind the π symbol in the footer.

**Apache** works as well, but needs `mod_rewrite` and `AllowOverride FileInfo` for the JSON API, see the [README](../README.md#json-api).

## Transcoder

tcd runs natively with systemd, on the reflector's machine or on another one; it connects to the reflector's `[Transcoder]` port (10100/tcp).

```bash
sudo bash tcd/build.sh               # two or more AMBE devices
sudo bash tcd/build.sh --with-md380  # a single DV3000, DMR/YSF in software
sudo nano /usr/local/etc/tcd.ini     # ServerAddress, Port, Modules, devices
sudo systemctl start tcd
```

The modules in `tcd.ini` must match `[Transcoder] Modules` in `urfd.ini`. Devices, gains, AGC and monitoring: [tcd/README.md](../tcd/README.md).

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
| `tcd/build.sh` | `sudo bash tcd/build.sh` (with `--with-md380` if used; restarts a running tcd) |

New keys show up in the templates in `config/` and `tcd/config/`; `inicheck -q` shows what is missing or wrong in `urfd.ini`.
