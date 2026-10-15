# urfd und tcd von Hand installieren

[English](INSTALL-ADVANCED.md) | **Deutsch**

Diese Anleitung ist für alle, die sich mit Linux auskennen und selbst einrichten wollen: den Reflektor in Docker ohne Setup-Script, den Reflektor ohne Docker, das Dashboard auf dem eigenen Webserver, den Transcoder allein. Einfacher geht es mit [`setup.sh`](../setup.sh), siehe [INSTALL.de.md](INSTALL.de.md); es erledigt die Docker-Schritte unten selbst.

## Inhalt

- [Überblick](#überblick)
- [Reflektor in Docker](#reflektor-in-docker)
- [Reflektor ohne Docker](#reflektor-ohne-docker)
- [Dashboard ohne Docker](#dashboard-ohne-docker)
- [Transcoder](#transcoder)
- [Aktualisieren](#aktualisieren)

## Überblick

| | Docker (wie `setup.sh` es macht) | Ohne Docker |
|---|---|---|
| urfd, `inicheck`, `dbutil` | im Image `urfd`, gebaut aus dem Checkout | `/usr/local/bin`, aus `reflector/build.sh` |
| Konfiguration | `$URFD_DATA/config/` (Standard `/opt/urfd/config`), eingebunden als `/usr/local/etc/urfd` | `/usr/local/etc/urfd/` |
| Dashboard | im Container (nginx + php-fpm, Port 8363), Einstellungen in `$URFD_DATA/dashboard/config.inc.php` | eigener Webserver, z. B. `/var/www/urfd-dashboard` |
| Dienst | Container `urfd`, `restart: unless-stopped` | systemd-Unit `urfd` |
| tcd | in beiden Fällen nativ mit systemd (`tcd/build.sh`) | |

Voraussetzungen: Debian 12, Ubuntu 22.04 oder neuer oder Raspberry Pi OS; x86_64, arm64 oder armhf. Die zu öffnenden Ports stehen in [INSTALL.de.md](INSTALL.de.md#ports).

Die Vorlagen in [`config/`](../config/) erklären jeden Schlüssel. Vor dem Start `urfd.ini` mit `inicheck -q urfd.ini` prüfen: Es zeigt Warnungen und Fehler und endet bei Fehlern mit einem Fehlercode.

## Reflektor in Docker

Das Image wird aus dem Checkout gebaut; Konfiguration und Zustand liegen außerhalb in `$URFD_DATA`, ein `git pull` berührt sie also nie. Details zum Image: [docker/README.md](../docker/README.md).

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd

# Konfiguration, die Pfade in urfd.ini sind die im Container
sudo mkdir -p /opt/urfd/config /opt/urfd/dashboard /opt/urfd/xlxd-ch
sudo cp config/urfd.ini config/urfd.interlink config/urfd.whitelist config/urfd.blacklist config/urfd.terminal /opt/urfd/config/
sudo sed -i 's#/home/user/#/usr/local/etc/urfd/#' /opt/urfd/config/urfd.ini
sudo cp dashboard/pgs/config.inc.php /opt/urfd/dashboard/

# urfd.ini enthält Passwörter; dem Dashboard (www-data, uid 33) gehören seine Konfiguration und sein Cache
sudo chmod 600 /opt/urfd/config/urfd.ini
sudo chown 33:33 /opt/urfd/dashboard/config.inc.php /opt/urfd/xlxd-ch
```

`/opt/urfd/config/urfd.ini` anpassen (mindestens `[Names]`, `[IP Addresses]`, `[Modules]`, `[Transcoder]`) und `/opt/urfd/dashboard/config.inc.php`, dann bauen und starten:

```bash
sudo docker/build.sh start     # oder: cd docker && sudo docker compose build && sudo docker compose up -d
sudo docker exec urfd inicheck -q /usr/local/etc/urfd/urfd.ini
sudo docker logs -f urfd
```

Anderes Datenverzeichnis: `sudo URFD_DATA=/srv/urfd docker/build.sh start`, ebenso für `docker compose`. Der Container nutzt das Netz des Hosts, die Ports in `urfd.ini` sind also die Ports des Hosts; das Dashboard lauscht auf 8363.

Neu laden nach geänderten Zuordnungen oder Listen (was ein Neuladen umfasst: [README](../README.de.md#konfiguration-per-sighup-neu-laden)):

```bash
sudo docker exec urfd pkill -HUP -x urfd   # nicht docker kill -s HUP: das erreicht supervisord
sudo docker restart urfd                   # alles andere
```

## Reflektor ohne Docker

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd
sudo bash reflector/build.sh            # mit OpenDHT
sudo bash reflector/build.sh --no-dht   # wo es libopendht-dev nicht gibt
```

`build.sh` installiert die Pakete zum Bauen (`build-essential`, `nlohmann-json3-dev`, `libcurl4-gnutls-dev`, `libopus-dev`, `libssl-dev`, `libwebsockets-dev`, `libopendht-dev`), baut als Besitzer des Checkouts und installiert `urfd`, `inicheck` und `dbutil` nach `/usr/local/bin` sowie die systemd-Unit `urfd` mit Echtzeit-Scheduling. Der erste Lauf legt die Vorlagen in `/usr/local/etc/urfd/` ab, mit Pfaden dorthin und `urfd.ini` nur für root lesbar; spätere Läufe behalten die Konfiguration.

Die Build-Optionen stehen in `reflector/urfd.mk`, beim ersten Bauen aus [`config/urfd.mk`](../config/urfd.mk) kopiert und danach behalten: `BINDIR`, `debug`, `DHT`.

```bash
sudo nano /usr/local/etc/urfd/urfd.ini
sudo inicheck -q /usr/local/etc/urfd/urfd.ini
sudo systemctl start urfd
journalctl -u urfd -f
```

Neu laden: `sudo pkill -HUP -x urfd`, neu starten: `sudo systemctl restart urfd`. Entfernen: `cd reflector && sudo make uninstall`, danach `/usr/local/etc/urfd` löschen.

Von Hand, mit installierten Paketen:

```bash
cd urfd/reflector
cp ../config/urfd.mk ../config/urfd.service .
make -j$(nproc)          # DHT=false ohne OpenDHT
sudo make install        # nur urfd und die Unit, und startet ihn; inicheck, dbutil und die Konfiguration vorher selbst kopieren
```

## Dashboard ohne Docker

Das Dashboard liest die Statusdatei des Reflektors (`[Files] XmlPath`, Standard `/var/log/xlxd.xml`) und spricht mit dem Admin-Socket (`[Admin]`, Port 10101), läuft also auf dem Rechner des Reflektors. Es braucht PHP mit einem Webserver; die mitgelieferte nginx-Konfiguration leitet auch die [JSON-API](../README.de.md#json-api) weiter.

```bash
sudo apt install nginx php-fpm
sudo cp -r ~/urfd/dashboard /var/www/urfd-dashboard
sudo mkdir -p /var/lib/xlxd-ch
sudo chown -R www-data:www-data /var/www/urfd-dashboard /var/lib/xlxd-ch

sudo cp ~/urfd/docker/nginx-dashboard.conf /etc/nginx/sites-available/urfd-dashboard.conf
# die Konfiguration ist für PHP 8.3 (Ubuntu 24.04); den Socket dieses Systems eintragen, z. B. php8.2 unter Debian 12
PHPSOCK=$(ls /run/php/php[0-9]*-fpm.sock)
sudo sed -i "s#/var/run/php/php8.3-fpm.sock#$PHPSOCK#" /etc/nginx/sites-available/urfd-dashboard.conf
sudo ln -s /etc/nginx/sites-available/urfd-dashboard.conf /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
```

Das Dashboard ist dann auf Port 8363; für einen anderen Port `listen` in der Konfiguration ändern oder es hinter den vorhandenen Webserver stellen. Das Docker-Image erhöht außerdem `pm.max_children` und senkt `default_socket_timeout` in PHP, siehe [docker/README.md](../docker/README.md#php-tuning); auf einem viel genutzten Reflektor lohnt sich das. Die Einstellungen stehen in `/var/www/urfd-dashboard/pgs/config.inc.php`; für Updates eine Kopie davon aufheben.

**Admin-Seite**: `$Admin['Enable'] = true` im Dashboard und `[Admin] Enable = true` in `urfd.ini` (urfd neu starten). Anmelden mit dem `[Admin] Password` aus `urfd.ini`; das Dashboard hält keine Kopie davon. Die Seite liegt hinter dem π-Symbol in der Fußzeile.

**Apache** geht auch, braucht für die JSON-API aber `mod_rewrite` und `AllowOverride FileInfo`, siehe [README](../README.de.md#json-api).

## Transcoder

tcd läuft nativ mit systemd, auf dem Rechner des Reflektors oder auf einem anderen; er verbindet sich mit dem `[Transcoder]`-Port des Reflektors (10100/tcp).

```bash
sudo bash tcd/build.sh               # zwei oder mehr AMBE-Geräte
sudo bash tcd/build.sh --with-md380  # ein einzelner DV3000, DMR/YSF in Software
sudo nano /usr/local/etc/tcd.ini     # ServerAddress, Port, Modules, Geräte
sudo systemctl start tcd
```

Die Module in `tcd.ini` müssen zu `[Transcoder] Modules` in `urfd.ini` passen. Geräte, Verstärkungen, AGC und Monitoring: [tcd/README.de.md](../tcd/README.de.md).

## Aktualisieren

```bash
cd ~/urfd && git pull
```

| Installiert mit | Dann |
|---|---|
| `setup.sh` | `sudo ./setup.sh update` |
| Docker von Hand | `sudo docker/build.sh` (baut neu, startet einen laufenden Container neu) |
| `reflector/build.sh` | `sudo bash reflector/build.sh` (startet einen laufenden urfd nach Prüfung von `urfd.ini` neu) |
| Dashboard von Hand | `pgs/config.inc.php` sichern, `sudo cp -r ~/urfd/dashboard/. /var/www/urfd-dashboard/`, die eigenen Einstellungen in die neue `pgs/config.inc.php` übernehmen |
| `tcd/build.sh` | `sudo bash tcd/build.sh` (mit `--with-md380`, falls genutzt; startet einen laufenden tcd neu) |

Neue Schlüssel erscheinen in den Vorlagen in `config/` und `tcd/config/`; `inicheck -q` zeigt, was in `urfd.ini` fehlt oder falsch ist.
