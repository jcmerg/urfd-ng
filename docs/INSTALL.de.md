# urfd und tcd installieren

[English](INSTALL.md) | **Deutsch**

Diese Anleitung richtet mit dem Setup-Script einen URF-Reflektor mit Web-Dashboard und auf Wunsch den Transcoder tcd ein. Linux-Kenntnisse brauchst du dafür kaum, nur ein paar Befehle zum Eintippen. Das Script stellt ein paar Fragen, schreibt die Konfiguration für dich und baut und startet alles.

- **urfd** ist der Reflektor. Er läuft zusammen mit dem Dashboard in Docker.
- **tcd** ist der Transcoder. Er wandelt das Audio zwischen D-Star, DMR/YSF, M17, P25 und NXDN um und nutzt dafür DVSI-AMBE-USB-Sticks. Das Setup-Script lässt ihn nach Wahl in Docker oder direkt auf dem Rechner mit systemd laufen; beides geht mit den Sticks (siehe „tcd in Docker“ in [tcd/README.de.md](../tcd/README.de.md#tcd-in-docker)). Ohne Transcoder funktioniert der Reflektor auch, die Nutzer hören dann aber nur Stationen derselben Betriebsart.

## Was du brauchst

- **Einen Rechner, der immer läuft**: einen PC, einen kleinen Server, einen VPS oder einen Raspberry Pi 4/5. 2 GB RAM reichen.
- **Linux**: Debian 12, Ubuntu 22.04 oder neuer, oder Raspberry Pi OS (64 Bit empfohlen). Am besten frisch installiert.
- **Internetzugang** für diesen Rechner, und die Möglichkeit, Ports in deinem Router oder deiner Firewall freizugeben (siehe [Ports](#ports)).
- **Für den Transcoder**: DVSI-AMBE-USB-Sticks (DV3000/ThumbDV oder DV3003), eingesteckt in den Rechner, auf dem tcd läuft.
  - Ein Gerät (DV3000 oder DV3003): 1 transcodiertes Modul, DMR/YSF braucht dann ein [Codec-Plugin](../tcd/README.de.md#codec-plugins).
  - DV3000 + DV3003: 2 Module. Zwei DV3000: 1 Modul. Zwei DV3003: bis zu 3 Module.
  - Mehr Module oder gar kein Stick: Ein [Codec-Plugin](../tcd/README.de.md#codec-plugins) für D-Star und DMR macht sie in Software (nicht Teil dieses Repositorys, `setup.sh` installiert es nicht).
- **Ein Reflektor-Rufzeichen**: `URF` plus drei Buchstaben oder Ziffern, z.B. `URF363`. Prüfe in der [Reflektorliste](https://xlxapi.rlx.lu/api/exportreflectors), dass es noch frei ist.

## Welcher Aufbau?

| Deine Situation | Auswahl im Script |
|---|---|
| Reflektor und AMBE-Sticks am selben Rechner | **1** Reflector and transcoder |
| Reflektor ohne Transcoder | **2** Reflector only, bei "transcoder on another machine" mit **no** antworten |
| Reflektor auf einem Server oder VPS, AMBE-Sticks an einem Raspberry Pi zu Hause | Auf dem Server: **2** Reflector only, mit **yes** antworten. Auf dem Pi: **3** Transcoder only |

## Schritt für Schritt

### 1. Anmelden und die Software holen

Melde dich am Rechner an (direkt oder per `ssh` von einem anderen Rechner) und gib ein:

```bash
sudo apt update
sudo apt install -y git
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd
```

### 2. Das Setup-Script starten

```bash
sudo ./setup.sh
```

Das Script stellt seine Fragen nacheinander (auf Englisch). Ein Wert in `[eckigen Klammern]` ist der Vorschlag: einfach Enter drücken, um ihn zu übernehmen.

| Frage | Was du eingibst |
|---|---|
| What should run on this machine? | 1, 2 oder 3, siehe [Welcher Aufbau?](#welcher-aufbau) |
| Reflector callsign | Dein `URFxxx`-Rufzeichen |
| Sysop e-mail address | Wo dich Nutzer und die Betreiber des Verzeichnisses erreichen |
| Country code | Zwei Buchstaben, z.B. `DE` |
| Sponsor or club | Wird im Dashboard angezeigt |
| Dashboard address | Wie das Dashboard von außen erreichbar ist, z.B. `http://urf123.example.com:8363` oder `http://deine-ip:8363`. Der Vorschlag ist die lokale Adresse, für einen ersten Test reicht sie |
| Modules | Die "Räume" des Reflektors, Buchstaben A bis Z. `ADMSZ` ist ein guter Anfang |
| Echo/parrot module | Ein Modul, das wiedergibt, was du sagst, ideal zum Testen. Meist `Z` |
| Transcoded modules | Die Module, deren Audio zwischen den Betriebsarten umgewandelt wird, so viele, wie deine AMBE-Hardware schafft (mehr mit einem Codec-Plugin) |
| Admin page | Damit verwaltest du Talkgroups und siehst das Live-Log im Dashboard. Das Script erzeugt ein Passwort |
| Register in the XLX directory | Trägt den Reflektor in Pi-Star, WPSD und andere Hotspot-Software ein. Antworte erst mal **no** und trage ihn später ein, wenn er läuft und aus dem Internet erreichbar ist |
| Install Docker | Ja, wenn das Script es anbietet |
| AMBE hardware | 1 für ein einzelnes Gerät (DV3000 oder DV3003), 2 für zwei Geräte, 3 für keines (ein Codec-Plugin macht alles in Software) |
| Serial numbers | Das Script listet die angeschlossenen seriellen USB-Geräte. Gib die Seriennummern deiner AMBE-Sticks ein, oder drück Enter, dann nimmt tcd die AMBE-Sticks, die er erkennt |

Das Bauen dauert ein paar Minuten, auf einem Raspberry Pi bis zu einer halben Stunde. Am Ende zeigt das Script die Dashboard-Adresse, das Admin-Passwort und die freizugebenden Ports. Alles, was es ausgibt, steht auch in `/var/log/urfd-setup.log`.

### 3. Die Ports freigeben

Leite diese Ports in deinem Router auf den Rechner weiter, oder gib sie in der Firewall deines VPS frei. Nötig sind nur die Protokolle, die du anbieten willst.

### Ports

| Port | Protokoll | Wofür |
|---|---|---|
| 8363/tcp | HTTP | Dashboard |
| 17000/udp | M17 | M17-Clients und Kopplung mit M17-Reflektoren |
| 20001/udp | DPlus | D-Star (DPlus) |
| 30001/udp | DExtra | D-Star (DExtra) |
| 30051/udp | DCS | D-Star (DCS) |
| 62030/udp | MMDVM | DMR-Hotspots, erst nachdem `[MMDVM]` in `urfd.ini` eingeschaltet ist (siehe unten) |
| 8880/udp | DMR+ | DMR+ |
| 42000/udp | YSF | Fusion (YSF) |
| 41000/udp | P25 | P25 |
| 41400/udp | NXDN | NXDN |
| 10017/udp | URF | Kopplung mit anderen URF-Reflektoren |
| 10100/tcp | Transcoder | Nur wenn tcd auf einem anderen Rechner läuft, und nur von diesem Rechner aus |

### 4. Ausprobieren

1. Öffne das Dashboard im Browser. Das Live-Dashboard liegt unter `/live/` auf derselben Adresse, z.B. `http://urf123.example.com:8363/live/`; steht das Dashboard hinter einem eigenen TLS-Proxy, muss der `/ws` als WebSocket weiterreichen (`Upgrade`-Header, siehe [docs/WEBSOCKET.md](WEBSOCKET.md#setup)).
2. Verbinde einen Hotspot oder ein Funkgerät mit deinem Reflektor und einem Modul, z.B. `URF123 Z` für das Echo-Modul.
3. Sag etwas und lass los: Nach 3 Sekunden hörst du dich selbst.
4. Teste ein transcodiertes Modul über Betriebsarten hinweg, z.B. D-Star und DMR auf Modul `A`.

### 5. Im XLX-Verzeichnis eintragen (optional)

Wenn alles von außen funktioniert, kann der Reflektor in der Hotspot-Software erscheinen:

1. In `/opt/urfd/config/urfd.ini`, Abschnitt `[CallingHome]`: `Enable = true` setzen, dazu den Namen des Landes bei `Country` (z.B. `Germany`) und einen `Comment`. Die `DashboardUrl` aus `[Names]` wird mitgeschickt.
2. Ziehst du einen Reflektor um, der schon im Verzeichnis steht (eingetragen von einem XLX-Dashboard oder einem älteren urfd)? Dann trage seinen alten Hash im Abschnitt als `Hash = ...` ein, siehe [README](../README.de.md#einen-schon-eingetragenen-reflektor-umziehen). Sonst `Hash` leer lassen: Der Reflektor erzeugt beim ersten Start einen und speichert ihn dort.
3. Neu starten: `docker restart urfd`.

Der Reflektor meldet sich dann alle 5 Minuten beim Verzeichnis; `docker logs urfd | grep CallingHome` zeigt, ob es geklappt hat. Hast du die Frage beim Setup mit yes beantwortet, ist das schon eingeschaltet.

## Im Alltag

| Aufgabe | Befehl |
|---|---|
| Auf die neueste Version aktualisieren | `sudo ~/urfd/setup.sh update` |
| Was hier läuft, in welcher Version | `sudo ~/urfd/setup.sh status` |
| Log des Reflektors | `sudo ~/urfd/setup.sh logs urfd` (oder `docker logs -f urfd`) |
| Reflektor neu starten | `sudo ~/urfd/setup.sh restart urfd` (oder `docker restart urfd`) |
| Listen, Interlink, TG-Zuordnungen neu laden | `sudo ~/urfd/setup.sh reload` |
| Log des Transcoders | `sudo ~/urfd/setup.sh logs tcd` (oder `docker logs -f tcd`, nativ `journalctl -u tcd -f`) |
| Transcoder neu starten | `sudo ~/urfd/setup.sh restart tcd` (oder `docker restart tcd`, nativ `sudo systemctl restart tcd`) |
| Transcoder-Monitor | `docker exec -it tcd tcdmon` (nativ `tcdmon`) im Terminal, neben dem Reflektor `http://<reflektor>:8363/tcd/`, oder `http://localhost:8080` im Browser auf diesem Rechner (von einem anderen Rechner: `ssh -L 8080:localhost:8080 <rechner>`, oder `MonitorBindAddress = 0.0.0.0` in `tcd.ini` in einem vertrauenswürdigen LAN) |

Das Update behält deine Konfiguration. Es holt die neue Version, baut neu und startet neu.

## Die Konfiguration ändern

| Datei | Was drinsteht |
|---|---|
| `/opt/urfd/config/urfd.ini` | Reflektor: Module, Protokolle, Eintrag im XLX-Verzeichnis, BrandMeister-Anbindung, D-Star- und YSF-Verbindungen, Kopplungen. Jede Einstellung ist in der Datei erklärt |
| `/opt/urfd/config/urfd.interlink` | Kopplungen mit anderen Reflektoren |
| `/opt/urfd/config/urfd.blacklist`, `urfd.whitelist` | Rufzeichen, die sich verbinden dürfen oder nicht |
| `/opt/urfd/dashboard/config.inc.php` | Dashboard: Banner-Text, Anzeige der IP-Adressen, Admin-Seite |
| `/opt/urfd/tcd/tcd.ini` (nativ `/usr/local/etc/tcd.ini`) | Transcoder: Adresse des Reflektors, Module, Seriennummern der AMBE-Geräte, Pegel |

Nach dem Ändern den Reflektor (`sudo ~/urfd/setup.sh restart urfd`) oder den Transcoder (`sudo ~/urfd/setup.sh restart tcd`) neu starten. Whitelist, Blacklist, Interlink und TG-Zuordnungen brauchen keinen Neustart: `sudo ~/urfd/setup.sh reload`. `urfd.ini` vorher prüfen mit:

```bash
sudo ~/urfd/setup.sh check
```

Die transcodierten Module stehen in der `tcd.ini` (`Modules`); in der `urfd.ini` steht `[Transcoder] Modules = auto` (schreibt das Setup-Script seit 4.25.0), urfd folgt also tcd. Eine ältere `urfd.ini` mit einer Liste dort braucht dieselben Module wie die `tcd.ini`.

**DMR-Hotspots** (Pi-Star, WPSD und andere MMDVM-Hotspots) verbinden sich über den MMDVM-Server, der nach dem Setup aus ist. Um ihn anzubieten, den Abschnitt `[MMDVM]` in `urfd.ini` einkommentieren, Talkgroups auf Module legen (z.B. `TG9 = A,TS2`) und entweder Nutzer mit Passwörtern eintragen oder `RequireAuth = false` für freien Zugang setzen. Dann Port 62030/udp freigeben und den Reflektor neu starten.

## Fehlersuche

**Das Dashboard lädt nicht.** Prüfe, ob der Container läuft: `docker ps` sollte `urfd` als `healthy` zeigen. Wenn nicht, schau in `docker logs urfd`; meist ist es ein Fehler in `urfd.ini`.

**Nutzer können sich nicht verbinden.** Der Port ihres Protokolls ist nicht freigegeben oder weitergeleitet, siehe [Ports](#ports). Teste von außerhalb deines Netzes, z.B. mit einem Hotspot am Handy-Hotspot.

**Module werden nicht transcodiert.** `docker logs urfd | grep -i transcoder` zeigt, ob sich tcd verbunden hat. Auf dem tcd-Rechner zeigt `docker logs tcd` (nativ `journalctl -u tcd`), ob er die AMBE-Sticks gefunden hat; `docker exec tcd tcd --list-devices` (nativ `tcd --list-devices`) listet sie. Läuft tcd auf einem anderen Rechner, muss `ServerAddress` in `tcd.ini` die Adresse des Reflektors sein, und Port 10100/tcp muss von dort erreichbar sein; `Password` in `tcd.ini` muss das `[Transcoder] Password` des Reflektors sein (das Setup-Script zeigt es am Ende der Reflektor-Installation). Ein falsches steht als `refused: wrong password` in `docker logs urfd`.

**Das Setup noch einmal ausführen.** `sudo ./setup.sh` bietet auf einem eingerichteten Rechner ein Update an. Um neu anzufangen, das Update ablehnen und dem Neu-Einrichten zustimmen: Das überschreibt `urfd.ini`, die Dashboard-Konfiguration und `tcd.ini`.

## Von Hand einrichten

Ohne Setup-Script oder ohne Docker: [INSTALL-ADVANCED.de.md](INSTALL-ADVANCED.de.md).

## Alles entfernen

```bash
cd ~/urfd/docker && sudo docker compose down --rmi all -v
sudo rm -rf /opt/urfd
# ein nativer Transcoder:
sudo systemctl disable --now tcd
sudo rm -f /etc/systemd/system/tcd.service /usr/local/bin/tcd /usr/local/bin/tcdmon /usr/local/bin/agc-analyze /usr/local/etc/tcd.ini /usr/local/etc/tcd-build.conf
```
