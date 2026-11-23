# Konfiguration

[English](CONFIGURATION.md) | **Deutsch**

Jede Funktion von urfd-ng mit ihren Einstellungen in der `urfd.ini`. Die kommentierte Vorlage [config/urfd.ini](../config/urfd.ini) führt jeden Schlüssel auf; `inicheck` prüft eine Datei (siehe [README](../README.de.md#inicheck)). Die `tcd.ini` des Transcoders beschreibt [tcd/README.de.md](../tcd/README.de.md).

- [Grundlagen](#grundlagen)
- [XLX-Verzeichnis (CallingHome)](#xlx-verzeichnis-callinghome)
- DMR: [MMDVM-Server](#mmdvm-server), [MMDVMClient](#mmdvmclient), [OpenBridge](#openbridge), [DMR+](#dmr), [dynamische Sprechgruppen](#dynamische-sprechgruppen)
- D-Star: [Client-Anbindungen](#d-star-client-anbindungen-dcs-dextra-dplus), [Slow Data](#d-star-slow-data-für-transcodierte-streams), [G3](#g3)
- [YSF](#ysf), [M17](#m17), [NXDN](#nxdn), [P25](#p25)
- SvxLink: [SvxReflector-Client](#svxreflector-client), [SVXServer](#svxserver); [USRP (AllStar)](#usrp-allstar)
- [Protokolle sperren](#protokolle-sperren), [Reflektor-Kopplung](#reflektor-kopplung), [Echo-Modul](#echo-modul), [Plugins](#plugins)
- [Admin-Schnittstelle](#admin-schnittstelle), [Zugangsdatenbank](#zugangsdatenbank), [WebSocket](#websocket), [Dashboard](#dashboard)
- [Konfiguration per SIGHUP neu laden](#konfiguration-per-sighup-neu-laden)

## Grundlagen

### Namen und Module

```ini
[Names]
Callsign = URF999              # URF and three letters/digits (required)
SysopEmail = me@example.com    # shown in the dashboard's footer
Country = DE
Sponsor = My Club
DashboardUrl = https://my-reflector.example.com   # required
Bootstrap = xrf757.openquad.net  # required with DHT: a nearby reflector to join the DHT

[Modules]
Modules = ADFRSZ               # up to 26 modules, A-Z
DescriptionA = All Modes       # shown in the dashboard
TGD = 262                      # module talkgroups, see below
TGF = 26363
```

**Modul-Sprechgruppen**: `TG<M> = <Nummer>` gibt einem Modul einmal seine Sprechgruppe (jede Nummer auf einem Modul). Die DMR-Verbindungen nehmen sie von dort: `TS1 = <Module>` / `TS2 = <Module>` in einer MMDVMClient-Verbindung oder im MMDVM-Server, `TGs = auto` im MMDVM-Server, bei OpenBridge und im SVXServer, und Plugins (`CConfigure::GetModuleTG`). `TS1 = F` ist dasselbe wie `TG26363 = F,TS1` mit `TGF = 26363`. TG-Zeilen gehen daneben weiter und haben Vorrang (z.B. eine zweite, nur empfangende TG auf einem Modul).

### Transcoder

```ini
[Transcoder]
Port = 10100                   # 0 = no transcoder
BindingAddress = 127.0.0.1     # 0.0.0.0 for a tcd on another host
Modules = auto                 # or the letters of the transcoded modules
# Password =                   # the same as Password in tcd.ini, for a tcd on another host
```

Mit `Modules = auto` wird ein Modul transcodiert, solange ein tcd dafür verbunden ist; die Liste steht damit nur in der `tcd.ini`, und ein dort neues Modul braucht weder eine Änderung noch einen Neustart von urfd. Ein Stream auf einem Modul ohne tcd läuft wie auf einem Modul ohne Transcodierung: Er geht nur an die Protokolle seines eigenen Codecs (D-Star an D-Star; AMBE+2 an DMR, NXDN und YSF; IMBE an P25 und YSF; Codec2 an M17; SVX- und USRP-Audio an SVX und USRP). Interlinks und Plugins bekommen jeden Stream.

Ein tcd auf einem anderen Rechner meldet sich mit `Password` an, so kann der Port zum Internet offen sein; `setup.sh` erzeugt eines. Ein zweiter tcd kann für Module bereitstehen (`StandbyModules` in seiner `tcd.ini`): Er übernimmt sie, solange ihr tcd weg ist, etwa ein Software-tcd in Docker neben urfd für den mit den AMBE-Sticks. Ein tcd, der 20 s nicht antwortet oder ein Senden oder Empfangen 10 s blockiert, wird getrennt, damit ein Bereitschafts-tcd übernehmen kann; siehe [tcd/README.de.md](../tcd/README.de.md).

### Eingeschaltete Protokolle

Jedes Protokoll lässt sich ein- und ausschalten. Ohne `Enable` sind DCS, DExtra, DPlus, DMRPlus, M17, YSF und URF an (NXDN und P25, wenn sie einen `Port` haben); MMDVM, OpenBridge, SvxReflector, SVXServer, Echo, Admin und die Client-Anbindungen sind aus; XLXPeer, G3 und USRP brauchen die Zeile. Ein abgeschaltetes Protokoll braucht trotzdem seinen `Port`.

```ini
[DPlus]
Enable = false
Port = 20001
```

### Externe Adresse

Clients und Verzeichnisse brauchen die Adresse, unter der urfd im Internet erreichbar ist, und die ist hinter NAT nicht die gebundene. `IPv4External` / `IPv6External` setzen sie von Hand. Ohne sie fragt urfd die Dienste in `IPCheckUrl` (standardmäßig icanhazip.com, ifconfig.me, ipify.org und ident.me; die erste gültige Antwort gilt), beim Start und alle `IPCheckInterval` Sekunden (Standard 300, mindestens 30, 0 = nur beim Start). Eine geänderte Adresse, etwa nach dem Wechsel auf eine Ersatzleitung, gilt, wenn eine zweite Prüfung 10 s später sie bestätigt; sie wird protokolliert, geht sofort an das XLX-Verzeichnis und in die DHT, und G3 und die Plugins nennen sie ihren Clients. Eine Prüfung ohne Antwort ändert nichts. Antwortet beim Start kein Dienst (z.B. direkt nach dem Booten noch kein DNS), startet urfd mit einer Warnung und fragt alle 10 s, bis einer antwortet.

```ini
[IP Addresses]
IPv4Binding = 0.0.0.0
# IPv6Binding = ::                # dual-stack
# IPv4External = 203.0.113.10     # fixed: not detected
IPCheckInterval = 60              # check every minute
```

### ID-Datenbanken

Die DMR-ID-, NXDN-ID- und YSF-TX/RX-Datenbanken liefern Rufzeichen, Namen und Frequenzen.

```ini
[DMR ID DB]
Mode = http
FilePath = /usr/local/etc/urfd/dmrid.dat
# comma separated: the first is primary, the rest are tried in order
URL = https://radioid.net/static/user.csv, http://www.pistar.uk/downloads/DMRIds.dat, http://xlxapi.rlx.lu/api/exportdmr.php
```

- **Mehrere Quellen**: der Reihe nach versucht, bis eine antwortet; eine Ausweichquelle wird protokolliert. Eine Ausweichquelle ergänzt die Datenbank, die urfd hat, und lässt den Zwischenspeicher in Ruhe: Ein Spiegel hinkt der ersten Quelle hinterher, so bleiben die IDs, die nur sie hat, bis sie wieder antwortet.
- **Formate**: Felder dürfen durch `;`, `,` oder Tab getrennt sein, so gehen radioid.net-CSV, Pi-Star-`DMRIds.dat` und xlxapi; Zeilen, die nicht mit einer ID beginnen (ein CSV-Kopf), werden übersprungen. xlxapi hat keine Namen und steht deshalb zuletzt.
- **Zwischenspeicher**: Jeder vollständige Download wird als `<FilePath>.cache` aufbewahrt. Beim Start wird zuerst der Zwischenspeicher geladen und durch einen vollständigen Download ersetzt; antwortet keine Quelle, bleibt er, so ist die Datenbank nie leer, nur weil ein Verzeichnis langsam ist oder ausfällt. Mit `Mode = both` enthält `FilePath` eigene zusätzliche Einträge und wird nie überschrieben.
- Für die YSF-TX/RX-Daten ist keine zweite Quelle bekannt, dort ist der Zwischenspeicher der einzige Ersatz; fehlende YSF-Frequenzen betreffen nur die Wires-X-Frequenzangabe.
- NXDN-IDs, die nicht in der NXDN-Datenbank stehen, werden in der DMR-Datenbank gesucht (viele nutzen ihre DMR-ID auch für NXDN).

### Dateien

```ini
[Files]
PidPath = /var/run/xlxd.pid
WhitelistPath = /usr/local/etc/urfd/urfd.whitelist
BlacklistPath = /usr/local/etc/urfd/urfd.blacklist
InterlinkPath = /usr/local/etc/urfd/urfd.interlink
G3TerminalPath = /usr/local/etc/urfd/urfd.terminal
```

Wo urfd seine PID ablegt und seine Listen findet. White-, Blacklist und Interlink werden per SIGHUP neu gelesen, die G3-Terminaldatei, wenn sie sich ändert; die Pfade selbst brauchen einen Neustart.

### Modulzugang je Protokoll

`[Modules] Protocols<M> = <Protokolle>` öffnet Modul M für die genannten Protokolle und schließt es für alle anderen, etwa ein Modul abseits des Amateurfunknetzes, das nur die Protokolle einer geschlossenen Benutzergruppe nutzen. Ein Modul ohne die Zeile ist für jedes Protokoll offen.

```ini
[Modules]
Modules = ADP
DescriptionP = Closed group
ProtocolsP = MMDVM,M17     # module P: these protocols only
HideUnlisted = P           # talkers without an ID database entry stay off the dashboard
```

- **Namen**: wie bei [BlockProtocols](#protokolle-sperren), durch Komma oder Leerzeichen getrennt. `MMDVMClient` meint alle seine Verbindungen, eine einzelne lässt sich hier nicht nennen. Ein unbekannter Name hält urfd an wie ein Fehler in der `urfd.ini`.
- **Verbinden**: Eine DExtra-, DCS-, M17- oder G3-Verbindung zum Modul, ein Wires-X-Connect und ein MMDVM-Modulwechsel per TG werden abgelehnt (`Module P is closed to DCS, link refused`).
- **Senden**: Ein Stream von einem gesperrten Protokoll wird nicht angenommen, egal wie er adressiert war (TG, DG-ID, Anbindung); die Station erscheint nicht in Last heard.
- **Empfangen**: Die Streams des Moduls gehen nur an die offenen Protokolle; Interlinks bekommen nichts, außer `URF` oder `XLXPeer` steht in der Liste.
- **Anbindungen**: Eine Client-Anbindung oder ein Autolink auf einem gesperrten Modul bleibt verbunden, trägt aber nichts; ihr Protokoll in die Liste aufnehmen, um sie dort zu nutzen.
- **Neu laden**: SIGHUP liest die Zeilen neu; eine vorher verbundene Station bleibt verbunden, hört und sendet auf einem jetzt gesperrten Modul aber nichts.

`HideUnlisted = <Module>` hält die Sprecher auf diesen Modulen, die in keiner ID-Datenbank (DMR, NXDN) stehen, vom Dashboard fern: kein Last-Heard-Eintrag, kein „auf Sendung“, keine Position, im WebSocket wie in den Admin-Daten, als wäre das Modul still. Gelistete Sprecher werden gezeigt. Ein Partner, der das Modul offen hat (`URF`, `XLXPeer`), bekommt die Streams und zeigt sie auf seinem eigenen Dashboard, also Interlinks von so einem Modul fernhalten. Die Admin-Seite zeigt weiter, wer spricht.

## XLX-Verzeichnis (CallingHome)

Der Reflektor trägt sich selbst im XLX-Verzeichnis ein, über das ihn Pi-Star, WPSD und andere Hotspot-Software anbieten. Das Dashboard ist dafür nicht nötig.

```ini
[CallingHome]
Enable = true
Country = Germany              # country name as shown in the directory
Comment = My URF Reflector     # max 100 characters
# Hash =                       # letters and digits; blank = made at first start
# OverrideIP =                 # blank = the directory uses the address it sees
# PushDelay = 300              # seconds between updates (60-86400)
# Server = http://xlxapi.rlx.lu/api.php   # the directory
# Timeout = 5                  # seconds before giving up on it (1-60)
```

Gesendet werden der Name `XLXnnn`, Laufzeit, `DashboardUrl` aus `[Names]`, Land, Kommentar, Version und Interlinks. Ohne `Enable = true` wird nichts gesendet. Die Einstellungen werden beim Start gelesen.

Der Hash weist den Reflektor im Verzeichnis aus, und das Verzeichnis bindet `XLXnnn` an den ersten Hash, den es sieht. Das Verzeichnis vergibt keine Hashes: Mit leerem `Hash` erzeugt der Reflektor beim ersten Start einen und speichert ihn als `Hash = ...` in `[CallingHome]` seiner `urfd.ini`, die einzige Zeile, die er dort je schreibt. Kann er die `urfd.ini` nicht schreiben (etwa schreibgeschützt eingebunden), trägt er sich nicht ein und sagt das im Log. Den Hash ab dann behalten.

### Einen schon eingetragenen Reflektor umziehen

Ein Reflektor, der schon eingetragen war, durch ein XLX-Dashboard oder einen anderen urfd, muss seinen Hash behalten; einen neuen für denselben Namen nähme das Verzeichnis nicht an. Ihn vor dem ersten Start mit `Enable = true` setzen.

1. Prüfen, ob der Name im Verzeichnis steht (hier `XLX999`, die Nummer deines `URFnnn`):
   ```bash
   curl -s 'http://xlxapi.rlx.lu/api.php?do=GetReflectorList' | grep -A6 '<name>XLX999</name>'
   # ohne curl:
   wget -qO- 'http://xlxapi.rlx.lu/api.php?do=GetReflectorList' | grep -A6 '<name>XLX999</name>'
   ```
   Nicht eingetragen: nichts zu tun, der Reflektor erzeugt seinen Hash selbst. Ist der eingetragene Reflektor nicht deiner, eine andere Nummer wählen.
2. Eingetragen und deiner: den Hash vom alten Server kopieren. Die Datei ist `$CallingHome['HashFile']` in der `config.inc.php` des alten Dashboards:
   ```bash
   grep '\$Hash' /path/to/callinghome.php
   ```
   Lesbar ist er nur dort, nicht über das Web: Das Dashboard führt die Datei als PHP aus und sendet nichts.
3. In die `urfd.ini` eintragen und starten:
   ```ini
   [CallingHome]
   Enable = true
   Hash = Ab3dE4fG5hIj6K7L
   ```
   `docker logs urfd | grep CallingHome` zeigt dann `registered as XLXnnn`.

## MMDVM-Server

Hotspots und Repeater verbinden sich direkt (HomeBrew-DMR-Protokoll, RPTL→RPTK→RPTC→RPTO), BrandMeister ist nicht nötig. Reines DMR geht ohne Transcoder; für andere Betriebsarten braucht es transcodierte Module. Jeder Zeitschlitz verbindet sich mit seinem eigenen Modul, ein Hotspot kann also auf zwei Modulen zugleich sein.

```ini
[MMDVM]
Enable = true
Port = 62030
# FallbackDmrId = 0                # DMR ID for talkers without one, to the hotspots, DMR+ and P25 (0 or omit = drop)
# RequireAuth = true               # false = accept all connections without login
# DynamicTimeout = 5               # minutes until a slot's dynamic module unlinks without a call of the hotspot on it (0 = never)
# BlockProtocols = SVXServer,USRP  # block protocols bidirectionally
#
# TG mapping: TG<number> = <Module>[,TS<1|2>]
# First TG per module = primary (TX/RX). Additional = secondary (RX only). Default TS2.
# From the module talkgroups ([Modules] TG<M>): TS1 / TS2 = <modules> maps them on that
# timeslot, TGs = auto every other module with a talkgroup (on TS2, or TGs = auto,TS1); TG lines win.
# TS1 = F
# TGs = auto
TG9 = A,TS2                        # common hotspot default
# TG26363 = F,TS1                  # local TG -> module F on TS1
#
# Users: <BASE-DMRID> or <CALLSIGN> = <password>
# No users configured = all logins rejected (unless RequireAuth = false).
# 1234567 = changeme               # 7-digit base DMR ID, covers 123456700-99
# N0CALL = changeme                # callsign (resolved to DMR ID at startup)
# 1234568 =                        # empty password: any password is accepted
```

**Benutzer**: Ein Benutzer meldet sich mit DMR-ID oder Rufzeichen an (beim Start über die DMR-ID-Datenbank aufgelöst). Hotspots nach BrandMeister-Art hängen an die 7-stellige ID zwei Ziffern an (`123456701`); ein Eintrag mit der Basis-ID deckt alle ab. Ein leeres Passwort nimmt jedes Passwort an (für Hotspots mit festem Passwort). `RequireAuth = false` nimmt jeden Hotspot an. Benutzer lassen sich im laufenden Betrieb auf der Admin-Seite anlegen oder kommen aus der [Zugangsdatenbank](#zugangsdatenbank).

**Sprecher ohne DMR-ID**: Ein Sprecher, der in keiner ID-Datenbank steht (AllStar, ein D-Star- oder M17-Rufzeichen), erreicht die Hotspots, DMR+ und P25 nur mit `FallbackDmrId`; sonst wird der Stream für sie verworfen, wie für die DMR-Netze und NXDN.

**Sprechgruppen**: Wie bei MMDVMClient ist die erste TG eines Moduls die primäre (Senden/Empfangen), weitere sind sekundär (nur Empfang). Module auf verschiedene Zeitschlitze legen, damit ein Hotspot beide hört (das Dashboard zeigt z.B. „S TS1 D TS2“). Ein Hotspot, der sich von einem anderen Port neu meldet (NAT), behält seine Module.

**Statische Sprechgruppen**: Ein Hotspot nennt seine Sprechgruppen in seinen Optionen, wie für BrandMeister oder FreeDMR, und bekommt sie bei jeder Anmeldung, ohne zu tasten: `TS1=26363;TS2=262,2625` verbindet die Module von TG 26363 auf TS1 und von TG 262 und 2625 auf TS2 (`TS1_1=…;TS1_2=…`, wie BrandMeister es schreibt, geht auch; andere Schlüssel wie FreeDMRs `DIAL` bleiben unbeachtet). In MMDVMHost ist die Zeile `Options=` in `[DMR Network]`, in DMRGateway `Options=` im Abschnitt des Netzes. Eine TG ohne Modul, ein für MMDVM gesperrtes Modul oder ein doppelt genanntes Modul wird ausgelassen und protokolliert.

Ein Zeitschlitz kann mehrere Module tragen, einen Ruf zur Zeit: Der erste Ruf belegt den Slot, ein Ruf auf einem anderen seiner Module geht solange nicht hinaus, und nach dem letzten Paket bleibt der Slot 5 s bei diesem Modul, damit die Antwort durchkommt (der eigene Ruf des Hotspots hält seinen Slot genauso). Tasten auf einer anderen TG fügt deren Modul als dynamisches des Slots hinzu (das vorige dynamische geht, die statischen bleiben); TG 4000 trennt das dynamische. Eine TG ohne Modul geht an das Modul, mit dem der Slot gerade belegt ist, sonst an das dynamische, sonst an das erste statische.

**Zeitablauf des dynamischen Moduls**: Wie bei BrandMeister trennt sich das dynamische Modul eines Slots nach `DynamicTimeout` Minuten (Standard 5, 0 = nie) ohne einen Ruf des Hotspots darauf. Nur die eigenen Rufe des Hotspots auf diesem Modul starten die Zeit neu, gezählt ab dem Ende des Rufs; eingehende Rufe nicht. Ein Ruf, der beim Ablauf gerade auf den Slot hinausgeht, endet zuerst. Statische Module laufen nie ab. Die Links des WebSockets und `mmdvm_peer_list` der Admin-Schnittstelle geben `ts1_dynamic` / `ts2_dynamic`, `ts1_expires` / `ts2_expires` (Unix-Zeit) und `ts1_left` / `ts2_left` (Sekunden), das Ereignis `client_updated` folgt den Änderungen; die Seite Links des Dashboards zählt neben dem Modul rückwärts.

**TGs der MMDVMClient-Verbindungen**: Ein Gruppenruf auf einer TG, die der Server nicht zuordnet, geht an das Modul, auf dem eine MMDVMClient-Verbindung sie auf der Admin-Seite als primäre TG gebucht hat (z.B. TG91 → A auf der BrandMeister-Verbindung: Ein Hotspot, der auf TG91 tastet, verbindet A als sein dynamisches Modul und hört es auf TG91). Keine sekundäre TG, nicht die TGs der Verbindungen aus der `urfd.ini` (diese in `[MMDVM]` zuordnen), kein für MMDVM gesperrtes Modul und keine TG, die zwei Verbindungen auf verschiedenen Modulen gebucht haben. Die eigenen TGs des Servers haben Vorrang.

**Talker Alias**: Jeder Ruf erreicht die Hotspots mit einem Talker Alias wie von BrandMeister: Rufzeichen und Vorname aus der DMR-ID-Datenbank (`N0CALL Max`), höchstens 13 Zeichen in UTF-16, das Motorola-Geräte brauchen. Einen Alias, den die DMR-Quelle sendet (BrandMeister, das Funkgerät), reicht urfd weiter, sobald er vollständig ist, neu kodiert in UTF-16. Er geht in jedem zweiten Superframe, so kann MMDVMHost in die anderen seinen Voice-LC für spät einsteigende Geräte legen. Die Service-Optionen im Voice-LC (OVCM, Notruf, Privacy) sind immer 0.

**Rufe ohne Sprache**: Manche Geräte tasten nach einem Sprechgruppenwechsel von selbst, mit etwa 3 s Sprachframes, die nur aus Nullen bestehen. Ein Ruf öffnet seinen Stream erst mit dem ersten echten Sprachframe; einer ohne wird verworfen und protokolliert (`call from ... without voice ... ignored`). Der Slot folgt der Sprechgruppe trotzdem, Wechseln durch Tasten geht also.

**Hotspot einrichten**: den Hotspot auf die Adresse des Reflektors, Port 62030, richten. In WPSD/Pi-Star „MMDVM_BRIDGE“, den Ein-Netz-Modus oder ein eigenes Netz von DMRGateway nehmen. DMRGateway gibt einem Netz nur, was seine Regeln dorthin schicken: dem Abschnitt des Reflektors `PassAllTG=1`, `PassAllTG=2`, `PassAllPC=1` und `PassAllPC=2` statt Rewrites geben, so wählt das Funkgerät die Sprechgruppen des Reflektors, wie sie sind. Das eigene Netz von WPSD schreibt Rewrites für 9000001-9999999 (das Funkgerät wählt 9000000 + TG); `WPSD_AutoRewrites` abzuschalten lässt diese Zeilen in `/etc/dmrgateway`, sie müssen von Hand ersetzt werden (Configuration → Expert → DMR GW). Ein leeres `Address=` in einem DMRGateway-Netz lässt DMRGateway beim Start enden, und der Hotspot meldet sich beim Reflektor ab.

**XLX-Master-Modus** (WPSD/Pi-Star „XLX Master“ mit Startmodul) geht auch: DMRGateway verbindet das Modul mit einem Privatruf an 4001-4026 und bekommt das Modul auf TS2/TG9, das es auf Slot und TG des Funkgeräts umsetzt (standardmäßig TG6). DMRGateway meldet sich mit dem festen Passwort `passw0rd` aus `[XLX Network]` in `/etc/dmrgateway` an. Entweder dort das urfd-Passwort des Benutzers eintragen (WPSD/Pi-Star: Configuration → Expert → DMR GW; nach dem Speichern der Hauptkonfigurationsseite prüfen, die `passw0rd` zurückschreiben kann) oder dem Benutzer in `[MMDVM]` ein leeres Passwort geben. Sonst protokolliert der Reflektor `auth FAILED`.

**Admin-Befehle**:
```json
{"cmd": "mmdvm_user_add", "token": "...", "dmrid": 1234567, "password": "secret"}
{"cmd": "mmdvm_user_add", "token": "...", "callsign": "N0CALL", "password": "secret"}
{"cmd": "mmdvm_user_add", "token": "...", "dmrid": 1234568}
{"cmd": "mmdvm_user_remove", "token": "...", "dmrid": 1234567}
{"cmd": "mmdvm_user_list", "token": "..."}
{"cmd": "mmdvm_tg_add", "token": "...", "tg": 26363, "module": "F", "ts": 2, "ttl": 900}
{"cmd": "mmdvm_tg_remove", "token": "...", "tg": 26363}
{"cmd": "mmdvm_tg_list", "token": "..."}
{"cmd": "mmdvm_peer_list", "token": "..."}
```

`mmdvm_peer_list` gibt jeden Hotspot mit seinem `module` (dem ersten) und `ts1` / `ts2`: jedes Modul dieses Zeitschlitzes, die statischen zuerst (fehlt, wenn der Slot keines hat).

## MMDVMClient

urfd verbindet sich als Hotspot mit einem DMR-Master (z.B. BrandMeister) und ordnet Sprechgruppen je Zeitschlitz Modulen zu. Dynamische TGs lassen sich im laufenden Betrieb auf der Admin-Seite hinzufügen.

```ini
[MMDVMClient]
Enable = true
MasterAddress = master.example.com  # MMDVM master server IP or DNS hostname
MasterPort = 62031
# LocalPort = 12345                 # fixed local UDP source port (omit = OS-assigned)
DmrId = 1234567                     # your registered DMR/hotspot ID
Password = changeme                 # MMDVM hotspot security password
Callsign = N0CALL                   # callsign (used for DMR-ID lookup on kerchunk)
Latitude = 0.0
Longitude = 0.0
Location = My City
Description = My URF Reflector
URL = https://example.com
RxFreq = 430412500                  # must differ from TxFreq: BrandMeister refuses RX = TX
TxFreq = 439812500
# FallbackDmrId = 1234567           # DMR ID for talkers without one (0 or omit = drop)
# BlockProtocols = SvxReflector,YSF # block protocols bidirectionally
# Network = brandmeister.network    # default: brandmeister.network for BM masters, else the host
# BrandMeisterApiKey = eyJ0eXAiOiJKV1Qi...   # optional, see below
#
# TG mapping: TG<number> = <Module>[,TS<1|2>]
# First TG per module = primary (TX/RX). Additional = secondary (RX only). Default TS2.
TG91 = A,TS2                        # Worldwide -> module A (primary)
# TG262 = A,TS2                     # Germany -> module A (secondary, RX only)
# TG26363 = B,TS1                   # local -> module B (primary)
```

**Mehrere TGs je Modul**: Die erste TG eines Moduls ist die primäre und trägt, was auf dem Modul gesprochen wird, zum Master hinaus (Senden). Weitere TGs auf demselben Modul sind sekundär und bringen nur Verkehr herein (Empfang). Auf der Admin-Seite hinzugefügte dynamische TGs sind sekundär, außer das Modul hat noch keine primäre.

**FallbackDmrId**: gilt für einen Sprecher, dessen Rufzeichen keine DMR-ID hat. Ohne sie (oder 0) geht der Stream nicht an den Master.

**BrandMeisterApiKey** (optional; BrandMeister SelfCare → Profile → Security → API Keys): Damit gleicht urfd beim Start die statischen TGs ab (entfernt die, die nicht in der Konfiguration stehen, ergänzt fehlende), meldet eine dynamische TG bei BrandMeister als statisch an und entfernt sie dort wieder, wenn sie entfernt wird oder abläuft. Ohne ihn aktiviert urfd eine TG durch eine kurze Aussendung (Kerchunk).

**Mehrere Verbindungen**: bis zu 8 weitere Abschnitte neben `[MMDVMClient]`, 9 Verbindungen insgesamt. Jeder ist ein eigener Hotspot mit zwei Zeitschlitzen und eigener TG-Zuordnung, so können mehr Module zugleich mit BrandMeister sprechen, oder der Reflektor nimmt an anderen Netzen teil (TGIF, FreeDMR, ...). Je Verbindung eine eigene Hotspot-ID nehmen (die DMR-ID mit den Endungen `01`, `02`, ...); ein BrandMeister-Hotspot-Passwort gilt für alle.

```ini
[MMDVMClient]                       # the shared settings of the connections below
Enable = true
MasterAddress = 2622.master.brandmeister.network
MasterPort = 62031
DmrId = 1234567                     # your DMR ID; the connections add their suffix
Password = changeme
Callsign = N0CALL
BrandMeisterApiKey = ...
# no TG here: [MMDVMClient] is no connection of its own then

[MMDVMClient.01]                    # hotspot 123456701
TG262 = D,TS2                       # Germany on module D
TG91 = W,TS1                        # Worldwide on module W

[MMDVMClient.02]                    # hotspot 123456702
TS1 = F                             # the module talkgroup of F ([Modules] TGF)
TS2 = D,R                           # several modules on one timeslot

[MMDVMClient.TGIF]                  # another network: its own master, password and ID
MasterAddress = tgif.network
Password = ...
DmrId = 123456790
TG31665 = T,TS2
```

- **Namen**: `[MMDVMClient.<Name>]`, der Name aus Buchstaben, Ziffern, `-` und `_`. Eine benannte Verbindung nimmt, was sie weglässt, aus `[MMDVMClient]`: Master, Port, Passwort, Rufzeichen, `FallbackDmrId`, `Software`, `Firmware`, die Orts- und Frequenzschlüssel, `Network` mit dem Master und in diesem Netz den `BrandMeisterApiKey` (er gilt für alle Hotspots des Kontos; BrandMeister ignoriert die TGs der Anmeldeoptionen, nur die API macht sie statisch). `LocalPort`, `BlockProtocols` und `Enable` gelten je Verbindung. Eine `DmrId` mit ein oder zwei Ziffern ist eine Endung zur `DmrId` von `[MMDVMClient]`, eine längere die ganze ID; ohne `DmrId` ist ein Name aus Ziffern die Endung (`[MMDVMClient.50]` ist Hotspot `<DmrId>50`). `[MMDVMClient]` mit eigenen TGs ist ebenfalls eine Verbindung. Die nummerierten `[MMDVMClient2]` bis `[MMDVMClient9]` gehen auch; eine Datei nutzt benannte oder nummerierte Abschnitte, nicht beides. Verbindungen hinzufügen, entfernen oder umordnen braucht einen Neustart.
- **Netz**: Jede Verbindung gehört zum Netz in ihrem Schlüssel `Network`. Ohne ihn ist jeder BrandMeister-Master (`*.brandmeister.network`) `brandmeister.network`, jeder andere Master ein eigenes Netz (sein Host). `Network` setzen für zwei Master eines anderen Netzes oder für einen BrandMeister-Master, der per IP-Adresse angegeben ist.
- **Brücken**: Ein Ruf geht nie zurück an die Verbindung, von der er kam, noch an eine andere Verbindung im selben Netz. Zwischen Netzen wird ein Modul, das beide zuordnen, verbunden; `BlockProtocols = MMDVMClient.TGIF` schaltet das für dieses Paar ab.
- **Protokollnamen**: In `BlockProtocols` und den Admin-Befehlen `block`/`unblock` meint `MMDVMClient` jede Verbindung, der Name einer Verbindung (`MMDVMClient.TGIF`, oder `MMDVMClient1` bis `MMDVMClient9` für nummerierte; `MMDVMClient1` ist `[MMDVMClient]`) eine davon.
- **Prüfungen**: `inicheck` lehnt eine TG oder Hotspot-ID ab, die zwei Verbindungen eines Netzes nutzen; die Admin-Seite lehnt so eine dynamische TG ab.
- **Module ohne Transcodierung**: Eine MMDVMClient-TG darf auf einem Modul liegen, das tcd nicht transcodiert. Von dort gehen nur AMBE+2-Streams (DMR, YSF, NXDN) an den Master; D-Star, M17, SVX und die anderen bleiben lokal, sie kämen als Stille im Netz an.
- **Admin**: `tg_add`, `tg_remove`, `reconnect` und `kerchunk` nehmen `"client": 2` oder `"client": "MMDVMClient.50"`; ohne meinen sie `[MMDVMClient]`, bei einer TG die Verbindung, die sie hat, und `reconnect` alle. `tg_list` und `status` führen jede Verbindung mit ihrem Netz auf.

**Echos vom Master**: BrandMeister schickt nach vielen Rufen 0,6 bis 1,2 s später einen zweiten Stream derselben ID mit der letzten Sekunde des Rufs noch einmal. urfd erkennt ihn an seinen Sprachframes und verwirft ihn (`echo of the end of the call from ... dropped`), siehe [docs/AUDIO.de.md](AUDIO.de.md#echo-vom-ende-eines-rufs-vom-master).

## OpenBridge

Verbindet urfd von Server zu Server mit einem DMR-Netz: HBlink3, HBlink4, FreeDMR und BrandMeister (eine Brücke zu BrandMeister braucht ein Ticket und deren Prüfung). Es gibt keine Anmeldung und keinen Zeitschlitz: Jede Sprechgruppe, die die Verbindung zuordnet, läuft gleichzeitig mit den anderen, ein Ruf je Modul, so trägt eine Verbindung so viele Rufe, wie sie Module zuordnet. Beide Seiten richten die Verbindung ein: Die andere Seite braucht urfds Adresse, `LocalPort`, `NetworkId` und die Passphrase. Siehe `[OpenBridge]` in [config/urfd.ini](../config/urfd.ini).

```ini
[OpenBridge]
Enable = true
TargetAddress = obp.example.net
TargetPort = 62035
NetworkId = 1234567
Passphrase = secret
TGs = auto                     # every module with [Modules] TG<M>
TG26363 = F                    # and more TGs
```

- **Dialekte**: `classic` (Standard) sprechen HBlink3, HBlink4 und BrandMeister: die 53 Bytes eines MMDVM-DMRD-Pakets mit gelöschtem Slot-Bit und `NetworkId` als Repeater-ID, gefolgt von einem HMAC-SHA1 mit der Passphrase. `freedmr` ist FreeDMRs Protokollversion 5, die FreeDMR standardmäßig nutzt und mit der es klassische Pakete ablehnt: DMRE-Pakete mit Zeitstempel, Quellserver und -Repeater und Hop-Zähler, mit BLAKE2b gesichert, dazu Keepalives alle 10 s, die Version alle 60 s, Source Quench und Stun. Eine klassische Verbindung wechselt auf `freedmr` (bis zum Neustart), wenn die andere Seite zeigt, dass sie das spricht; `AutoUpgrade = false` hält sie klassisch. Für eine FreeDMR-Gegenstelle trotzdem `Dialect = freedmr` setzen: Bis zu ihrem ersten Keepalive verwirft FreeDMR klassische Pakete, und ein Ruf in dieser Zeit verliert seinen Anfang.
- **IDs**: `NetworkId` steht in jedem Paket, als die `NETWORK_ID`, die die andere Seite für urfd hat. FreeDMR nimmt sie auch als Quellserver von urfds Rufen (`SourceServer` setzt einen anderen) und prüft sie: eine Server-ID seiner Liste (4 oder 5 Ziffern) oder eine DMR-ID, die es kennt (6 oder 7 Ziffern), sonst verwirft es die Rufe; eine auf dich registrierte DMR-ID nehmen. `PeerId` lässt urfd nur Pakete mit dieser Repeater-ID annehmen (HBlinks `NETWORK_ID`, FreeDMRs `SERVER_ID`). `PreserveSourcePeer = true` (nur classic) setzt stattdessen die ID des Hotspots ein, von dem ein Ruf kommt, wie HBlink4s `preserve_source_peer`; BrandMeister und FreeDMR erwarten `NetworkId`. HBlink4 macht das standardmäßig: Mit `PeerId` für eine HBlink4-Gegenstelle dort `preserve_source_peer = false` setzen, sonst verwirft urfd ihre Rufe.
- **Passphrase**: HBlink3 und FreeDMR nutzen nur ihre ersten 20 Zeichen, HBlink4 alle: höchstens 20 nehmen. urfd warnt bei einer längeren.
- **Sprechgruppen**: `TG<Nummer> = <Modul>`, `TGs = auto` für jedes Modul mit einer Sprechgruppe in `[Modules]` oder `Modules = <Module>` für einige davon; TG-Zeilen haben Vorrang. Eine Sprechgruppe auf einem Modul ohne Transcodierung trägt nur DMR, YSF und NXDN. Sprechgruppen, die die Verbindung nicht zuordnet, werden in beide Richtungen verworfen, Privatrufe auch. FreeDMR verwirft TGs bis 79 und einige Bereiche darüber von sich aus.
- **Netz**: Die Verbindung gehört zu einem Netz wie eine MMDVMClient-Verbindung: der Host ihres Ziels, `brandmeister.network` für einen BrandMeister-Host, oder `Network`, das ein per IP-Adresse angegebenes Ziel braucht. Sie teilt keine TG mit einer Verbindung in diesem Netz, und Rufe gehen nie zwischen ihnen; zwischen Netzen wird ein Modul, das beide zuordnen, verbunden, `BlockProtocols` schaltet das ab.
- **Prüfungen**: Pakete kommen nur von der Adresse des Ziels (jeder Port) und müssen ihren HMAC oder BLAKE2b bestehen. FreeDMR-Pakete, die älter als 5 s sind oder mehr als 20 Hops haben, werden verworfen und gequencht: Die Uhren brauchen NTP. Ein von urfd gesendeter Ruf, der zurückkommt, oder ein beendeter Stream wird verworfen. Ein Ziel, das sich nicht auflösen lässt, wird alle 10 s neu nachgeschlagen.
- **Firewall**: Die andere Seite sendet an `LocalPort` (Standard 62035), für UDP öffnen. Nur IPv4.
- **Admin**: Die Admin-Seite listet die TGs der Verbindung (aus der `urfd.ini`, ein SIGHUP lädt sie neu; keine dynamischen), und `status` hat einen Eintrag `openbridge`: Ziel, Netz, Dialekt, wann die andere Seite zuletzt gehört wurde, ob sie die Verbindung gestunnt hat.

## DMR+

DMR+ (DMO-Modus) für Hotspots und Repeater auf UDP 8880. Module erreicht man über TG 4001-4026 (A-Z), auf der Seite Modules des Dashboards als „DMR+ TG“ gezeigt.

```ini
[DMRPlus]
Port = 8880
```

## Dynamische Sprechgruppen

Eine auf der Admin-Seite hinzugefügte TG (MMDVMClient, MMDVM-Server, SvxReflector, SVXServer) läuft nach ihrem Timeout (`ttl`, auf der Admin-Seite „Timeout in s“, Standard 900 s) ohne Aktivität ab. Jeder Sendebeginn auf dem Modul, aus welchem Protokoll auch immer, startet den Timer mit diesem Timeout neu, so hält Aktivität in jeder Betriebsart alle TGs des Moduls. Eine über die BrandMeister-API hinzugefügte MMDVMClient-TG wird beim Ablauf dort wieder entfernt. Das dynamische Modul eines Hotspots ist etwas anderes, siehe `DynamicTimeout` unter [MMDVM-Server](#mmdvm-server).

- **BrandMeister** führt einen eigenen Abo-Timer. Läuft er dort ab, läuft ohne weitere Aktivität auch die Zuordnung in urfd ab. Der Admin-Befehl **kerchunk** verlängert die BrandMeister-Seite ohne Funkgerät.
- **SvxReflector**: Eine abgelaufene TG wird aus den überwachten TGs genommen; war sie gewählt, wird wieder die Heimat-TG gewählt.

## D-Star-Client-Anbindungen (DCS, DExtra, DPlus)

urfd verbindet sich als Client-Knoten mit anderen D-Star-Reflektoren und ordnet ein entferntes Modul einem lokalen zu. Zuordnungen stehen fest in der `urfd.ini` oder werden im laufenden Betrieb auf der Admin-Seite hinzugefügt.

```ini
[DCSClient]
Enable = true
Callsign = N0CALL                         # must be whitelisted on the target reflector
Map1 = dcs002.xreflector.net,30051,A,S   # DCS002 module A -> local module S
Map2 = dcs009.xreflector.net,30051,B,F   # DCS009 module B -> local module F
# BlockProtocols = MMDVMClient,YSF       # optional

[DExtraClient]
Enable = true
Callsign = N0CALL
Map1 = xrf757.openquad.net,30001,A,A     # XRF757 module A -> local module A

[DPlusClient]
Enable = true
Callsign = N0CALL                         # must be registered at dstargateway.org
Map1 = ref001.dstargateway.org,20001,C,A # REF001 module C -> local module A
```

**Format**: `host,port,entferntes_modul,lokales_modul`. Jede Zuordnung ist eine Verbindung zum Modul des entfernten Reflektors.

**Routing**: Die Anbindung spricht den Reflektor an wie ein mit ihm verbundenes Gateway: RPT1 ist `<Callsign> <lokales Modul>`, RPT2 der Reflektor und das entfernte Modul (`DCS002 A`). Das Rufzeichen des Reflektors kommt aus den Streams, die er schickt, bis dahin aus dem Hostnamen (`dcs002.xreflector.net` → `DCS002`). Ein DPlus-Reflektor schickt die Streams aller seiner Module an jeden Client: Nur die des zugeordneten Moduls erreichen das lokale Modul.

**Verbindung halten** (wie ircDDBGateway): Die DCS-Anbindung beantwortet jeden Poll des Reflektors sofort und nennt den Reflektor in ihren Link- und Unlink-Paketen. Eine Verbindung gilt nach 60 s ohne Poll als verloren (DCS, DExtra; DPlus 30 s); die Anbindung versucht es dann nach 5 s erneut, dann nach 10, 20, 40 und alle 60 s. Vor jedem Versuch wird der Host neu nachgeschlagen; scheitert DNS, wird die letzte Adresse genommen und einmal `cannot resolve <host>` protokolliert.

**Rufzeichen**: DPlus-Reflektoren verlangen eine Registrierung bei [dstargateway.org](https://regist.dstargateway.org/). DExtra- und DCS-Reflektoren sind meist offen.

**Admin-Befehle**:
```json
{"cmd": "dcs_map_add", "token": "...", "host": "dcs002.xreflector.net", "port": 30051, "remote_module": "A", "local_module": "S"}
{"cmd": "dcs_map_remove", "token": "...", "local_module": "S"}
{"cmd": "dcs_map_list", "token": "..."}
{"cmd": "dextra_map_add", "token": "...", "host": "xrf757.openquad.net", "port": 30001, "remote_module": "A", "local_module": "A"}
{"cmd": "dplus_map_add", "token": "...", "host": "ref001.dstargateway.org", "port": 20001, "remote_module": "A", "local_module": "Z"}
{"cmd": "reconnect", "token": "...", "protocol": "dcsclient"}
```

## D-Star Slow Data für transcodierte Streams

Streams aus anderen Betriebsarten (DMR, YSF, SVX, M17, P25, USRP, die Client-Anbindungen) erreichen D-Star-Funkgeräte mit richtigen Slow Data:

- **Header**: Rufzeichen des Anrufers (MY), Rufzeichen des Reflektors (RPT1/RPT2). Die Quell-Betriebsart ist das vierstellige Suffix des Anrufers (`N0CALL / DMR`), das D-Star-Geräte anzeigen und in ihrer Anrufliste behalten: `DMR`, `YSF`, `NXDN`, `P25`, `M17`, `USRP`, `SVX` oder der Typ eines Plugins. Nur D-Star bekommt das Suffix; Dashboard, Logs und die anderen Betriebsarten zeigen das Rufzeichen allein.
- **Textnachricht**: der Vorname des Funkers (wenn das Rufzeichen in der DMR- oder NXDN-ID-Datenbank steht) und die Quelle, z.B. `Max TG26363`.

| Quelle | Slow-Data-Text |
|--------|---------------|
| MMDVMClient | `Max TG26363` |
| SvxReflector | `Max TG317424` |
| YSF (Server) | `Max DG28` |
| NXDN | `Max RAN6` |
| YSFClient | `Max DG15` |
| M17, P25, USRP, DMR+ | `Max` (nur der Name) |

YSFClient zeigt die DG-ID, wenn eine eingestellt ist. Ohne Namen und ohne TG, DG-ID oder RAN bleibt der Text leer. D-Star-Quellen, darunter die D-Star-Client-Anbindungen, werden nicht nach D-Star transcodiert: Ihre eigenen Slow Data gehen unverändert durch.

Der Text hat 20 Zeichen. Ist `<Name> <Quelle> TG<n>` länger (z.B. bei einem Anrufer mit eigenem Suffix, bei dem das Protokoll im Text bleibt), wandert die TG in einen zweiten Text, der sich etwa alle 5 Sekunden mit dem ersten abwechselt; ist er dann noch zu lang, wird der Name gekürzt, damit die Quelle ganz bleibt:

```
Max TG262                                      (all fits)
Maximilian TG263112                            (all fits)
Maximilian DMR    <-> DMR TG263112             (own suffix: the TG takes turns)
```

Header und Textnachricht wechseln sich jeden Superframe ab (~420 ms):

```
[Header: N0CALL > URF999 S] → [Max TG262] → [Header] → [Max TG262] → ...
```

**Positionen**: DPRS-Positionen in den Slow Data von D-Star-Geräten (GPS-A, gegen seine CRC geprüft, oder NMEA `RMC`/`GGA`) erscheinen als Locator in Last heard. Das Dashboard bekommt die Mitte des Locator-Feldes (etwa 4,6 × 2,3 km), nicht die genaue Position, die andere D-Star-Stationen auf dem Modul mit den Slow Data weiter bekommen.

## G3

Icom Terminal- und Access-Point-Modus (UDP 12345, 12346, 40000), `[G3] Enable = true`. `urfd.terminal` richtet ihn ein und wird neu geladen, wenn sie sich ändert:

- `address <öffentliche-ip>`: optional; ohne sie bekommt ein Terminal im Internet hinter NAT die externe IPv4 von urfd
- `modules <Buchstaben>`: die Module, die Terminals nutzen dürfen
- `alias <Rufzeichen>`: ein weiteres Reflektor-Rufzeichen, das von Terminals angenommen wird (z.B. sowohl `URFxxx` als auch `XLXxxx`); wiederholbar

## YSF

URF ist ein YSF-Master mit Wires-X-Räumen, einem je Modul. Hotspot-Software (Pi-Star/WPSD) findet URF-Reflektoren über das XLX-Verzeichnis; eine Registrierung bei ysfreflector.de ist nicht nötig.

```ini
[YSF]
Port = 42000
AutoLinkModule = A             # module YSF clients land on at connect
EnableDGID = false             # module switching via DG-ID 10-35 (10 = A, 11 = B, ... 35 = Z)
DefaultTxFreq = 446500000
DefaultRxFreq = 446500000
RegistrationID = 12345
RegistrationName = DE URF999   # max 13 characters
RegistrationDescription = URF Reflector  # max 16 characters
```

- **DG-ID**: Mit `EnableDGID = true` wechseln Benutzer das Modul per DG-ID, und der Reflektor sendet jedes Modul mit seiner DG-ID, so hören Geräte, die darauf filtern, das Modul.
- **DN und VW**: DN und V/D-Modus 1 werden wie DMRs AMBE+2 angenommen. VW (Voice Wide, IMBE Full Rate) ist der Codec von P25: Der Transcoder macht daraus die anderen Betriebsarten wie aus P25, P25-Hörer bekommen genau diese Frames, YSF-Hörer auf einem transcodierten Modul DN. Auf einem Modul ohne Transcoder geht ein IMBE-Stream (VW oder P25) als VW an YSF. VW-Sprache erreicht die anderen Betriebsarten etwa 85 ms später als DN. Die YSF-Client-Anbindung trägt kein VW.
- **Positionen**: Die GPS-Position, die Yaesu-Geräte im Datenkanal senden, erscheint als Locator in Last heard, wie D-Star- und M17-Positionen.

### YSF-Client-Anbindung

urfd verbindet sich als Client-Knoten mit anderen YSF-Reflektoren, jede Verbindung auf einem lokalen Modul, wahlweise mit einer DG-ID für Reflektoren, die Räume danach wählen.

```ini
[YSFClient]
Enable = true
Callsign = N0CALL
Map1 = 203.0.113.20,42099,A             # YSF reflector -> local module A
Map2 = 198.51.100.30,42000,F,15          # YSF reflector -> local module F, DG-ID 15
# BlockProtocols = MMDVMClient            # optional
```

**Format**: `host,port,lokales_modul[,dgid]`; DG-ID 0 = keine. Ist sie gesetzt, steht sie im FICH-Header jedes ausgehenden Sprachpakets. Ausgehende Pakete tragen das Rufzeichen des Reflektors (z.B. `URF999`) im Radio-Feld, das YSF-Dashboards als „Radio“ zeigen.

```json
{"cmd": "ysf_map_add", "token": "...", "host": "203.0.113.20", "port": 42099, "local_module": "A", "dgid": 0}
{"cmd": "ysf_map_remove", "token": "...", "local_module": "A"}
{"cmd": "ysf_map_list", "token": "..."}
```

## M17

```ini
[M17]
Port = 17000
# Callsign = M17-XXX        # required for mrefd interlinks, see below
```

- **Modulwahl**: Ein Client wählt sein Modul beim Verbinden: Das `CONN`- (oder `LSTN`-)Paket trägt den Modulbuchstaben. Zum Wechseln mit dem anderen Buchstaben neu verbinden, z.B. `URF999 B` als Reflektor in Pi-Star/WPSD, die Modulwahl in DroidStar oder mvoice, oder `URF999 B` als Ziel an einem M17-Funkgerät. Eine neue Verbindung ersetzt die alte desselben Rufzeichens und derselben IP. Wie bei mrefd geht eine Aussendung an das Modul, auf dem der Client verbunden ist, egal welches Ziel sie hat (`URF999 B`, `M17-XXX B` oder @ALL); Streams an die Clients sind an @ALL adressiert.
- **Echo**: `PARROT` oder `ECHO` als Ziel erreicht das [Echo-Modul](#echo-modul) von jedem verbundenen Modul aus; ein Paket im Packet Mode an sie kommt direkt an den Absender zurück.
- **Nur hören**: `LSTN`-Clients werden auf ihrem Modul angemeldet; so können Überwachungsdienste wie dvref.com den Reflektor prüfen. Ihre Sprache und Daten werden verworfen.
- **Sprache und Daten**: Sprachframes im alten wie im V#3-TYPE-Layout werden angenommen, verschlüsselte Streams verworfen. Packet Mode (`M17P`, z.B. SMS) wird gegen beide CRCs geprüft und unverändert an die anderen M17-Clients des Moduls weitergegeben; der Absender erscheint in Last heard.
- **META**: Text, GNSS und ECD erreichen die anderen M17-Clients mit ihrem TYPE und der CAN der Quelle. Eine GNSS-Position erscheint als Locator in Last heard, mit Link zu OpenStreetMap.
- **Rufzeichen**: Wie bei mrefd müssen ein `CONN`, ein `LSTN` (großzügiger) und die Quelle von Sprache und Packet-Daten wie ein Rufzeichen aussehen, alles andere wird verworfen. Ein Client, der nach dem Verbinden auf die Blacklist kommt, bekommt ein `DISC` und wird entfernt.
- **mrefd-Kopplung**: siehe [Reflektor-Kopplung](#reflektor-kopplung).

## NXDN

Module werden über die NXDN-RAN (Radio Access Number) oder über die Ziel-ID gewählt.

```ini
[NXDN]
Port = 41400
AutoLinkModule = S             # module for RAN 0 and RAN 1 (comment out to disable)
ReflectorID = 26363
# FallbackNxdnId = 8970        # NXDN ID for unknown callers (0 or omit = drop)
ModuleDstIdBase = 4000         # dstId 4001-4026 = A-Z (omit to disable)
```

**Per RAN**: RAN 1-26 ist Modul A-Z, wobei RAN 1 und die eigene RAN des AutoLinkModule getauscht sind: RAN 1 erreicht immer das AutoLinkModule, und Modul A bekommt dafür dessen RAN. RAN 0 geht ebenfalls an das AutoLinkModule. Mit `AutoLinkModule = S`:

| RAN | Modul | RAN | Modul | RAN | Modul |
|-----|-------|-----|-------|-----|-------|
| 0   | S     | 10  | J     | 19  | **A** |
| 1   | **S** | 11  | K     | 20  | T     |
| 2   | B     | ... | ...   | 26  | Z     |

Den Tausch gibt es, weil die meisten Clients nur RAN 1 senden können: DroidStar hat sie fest eingebaut, und MMDVMHost ersetzt die RAN des Funkgeräts durch die `[NXDN] RAN` des Hotspots (Standard 1) und verwirft HF mit einer anderen. Ein Modulwechsel per RAN vom Funkgerät aus geht deshalb nur bei direkter Verbindung; über einen Hotspot ist die RAN eine Einstellung des Hotspots.

**Per Ziel-ID** (hat Vorrang vor der RAN): Mit gesetztem `ModuleDstIdBase` ist die dstId `base+1` bis `base+26` Modul A-Z, jede andere dstId fällt auf die RAN zurück. Sie wird im Connect-Poll und in jedem Frame gelesen. Mit `ModuleDstIdBase = 4000` decken sich die NXDN-Ziel-IDs mit DMR+ (4001 = A, 4002 = B, …). Für Clients, die keine RAN setzen können, ist das die einzige Modulwahl; DroidStar-Nutzer legen je Modul einen eigenen Host an, die Nummer ist die dstId:

```
NXDN 4001 my.reflector.org 41400     -> Module A
NXDN 4003 my.reflector.org 41400     -> Module C
NXDN 26363 my.reflector.org 41400    -> AutoLinkModule
```

Ausgehende Frames tragen die Ziel-ID, mit der jeder Client gepollt hat: `NXDNGateway` verwirft jeden Frame, dessen dstId von der TG abweicht, mit der der Hotspot verbunden ist.

**IDs**: Eingehende NXDN-IDs werden in der NXDN-ID-Datenbank gesucht, dann in der DMR-Datenbank. Unbekannte IDs werden verworfen, außer `FallbackNxdnId` ist gesetzt. D-Star-Geräte sehen `RAN<n>` im Slow-Data-Text.

**Dashboard**: Die Spalte NXDN RAN erscheint, wenn NXDN eingeschaltet ist, eine Spalte `NXDN DST-ID` daneben, sobald `ModuleDstIdBase` gesetzt ist.

## P25

P25 für P25Gateway auf UDP 41000. `ReflectorID` ist die TG, mit der sich P25Gateway verbindet (Pflicht), `AutoLinkModule` das Modul, auf dem es landet. Sprecher ohne DMR-ID erreichen P25 nur mit `[MMDVM] FallbackDmrId`.

```ini
[P25]
Port = 41000
AutoLinkModule = A
ReflectorID = 12345
```

## SvxReflector-Client

urfd verbindet sich mit SvxLink-SvxReflector-Servern (z.B. FM-Funknetz) und überbrückt FM-Audio in beide Richtungen: TCP für die Signalisierung, UDP für Opus-Audio. Braucht transcodierte Module. Dynamische TGs lassen sich auf der Admin-Seite hinzufügen.

```ini
[SvxReflector]
Enable = true
Host = svxreflector.example.com    # DNS hostname or IP
Port = 5300                        # TCP and UDP port (same)
Callsign = N0CALL-HS               # node callsign as registered on the server
Password = changeme
# BlockProtocols = MMDVMClient,USRP # block protocols bidirectionally
# RxGain = 0                        # incoming audio gain in dB (-40 to +40)
# TxGain = 0                        # outgoing audio gain in dB (-40 to +40)
# Software = urfd                   # software name in the node info, for servers that require a specific one
#
# TG mapping: TG<number> = <Module>; several TGs per module allowed
TG26363 = S
```

**Eine TG zur Zeit**: Ein SvxReflector-Knoten hat eine gewählte TG; er bekommt nur deren Audio, und sein Audio geht dorthin. urfd wählt die erste statische TG (die niedrigste Nummer, die Heimat-TG) und überwacht alle zugeordneten TGs (wie SvxLinks `MONITOR_TGS`). Ein Sprecher auf einer überwachten TG wählt diese TG, wenn 3 s kein Verkehr war; Audio von einem Modul, dessen TG nicht gewählt ist, wählt sie, außer auf der gewählten spricht gerade jemand, dann wird diese Aussendung nicht gesendet. Nach 30 s ohne Verkehr kehrt urfd zur Heimat-TG zurück.

**RxGain / TxGain**: feste Verstärkung je Verbindung. RxGain wirkt nach dem Opus-Dekodieren vor dem Transcoder, TxGain nach dem Transcoder vor dem Opus-Kodieren; Samples über Vollaussteuerung werden abgeschnitten. Die AGC von tcd für analoge Quellen stellt den Pegel nach RxGain ein (sie hat eine eigene Schwelle bei -40 dBFS, siehe [tcd/README.de.md](../tcd/README.de.md)), also RxGain nahe 0 lassen; ein stark negatives RxGain drückt leise FM-Sprache unter die Schwelle.

**Software**: der Name, den urfd in seiner Knoteninfo meldet (`sw`), standardmäßig `urfd`. Nur in Absprache mit dem Betreiber des Reflektors setzen.

Fällt die Verbindung, wird wartendes Audio verworfen statt verspätet gesendet, und hinausgehendes Audio endet mit einem Flush, damit der Server den Sprecher nicht festhält.

## SVXServer

urfd ist ein SvxReflector-Server für SvxLink-Knoten (Protokoll 2, HMAC-SHA1-Anmeldung, Opus-Audio). Braucht transcodierte Module. Benutzer und TGs lassen sich im laufenden Betrieb auf der Admin-Seite verwalten.

```ini
[SVXServer]
Enable = true
Port = 5300                         # TCP and UDP port (same)
# BlockProtocols = MMDVMClient,USRP # block protocols bidirectionally
# RxGain = 0                        # incoming audio gain in dB (-40 to +40)
# TxGain = 0                        # outgoing audio gain in dB (-40 to +40)
#
# TG mapping: TG<number> = <Module>; several TGs per module allowed
# TGs = auto                        # every module with a talkgroup ([Modules] TG<M>); TG lines win
# TG26363 = A
#
# Users: <Callsign> = <Password>
# N0CALL-HS = changeme
```

**Benutzer**: fest in der `urfd.ini`, auf der Admin-Seite oder aus der [Zugangsdatenbank](#zugangsdatenbank). Jeder Knoten meldet sich mit Rufzeichen und Passwort an (HMAC-SHA1).

**TG-Wahl wie bei SvxReflector**: Ein Knoten hat eine gewählte TG (`MsgSelectTG`) und die TGs, die er überwacht (`MsgTgMonitor`). Eine TG hat einen Sprecher zur Zeit. Die anderen Knoten auf der TG bekommen die Opus-Pakete des Sprechers, wie sie kamen (ohne Transcodierung), auch auf TGs, die keinem Modul zugeordnet sind; zugeordnete TGs gehen außerdem an ihr Modul und kommen von dort. Knoten müssen Protokoll 2.0 sprechen, was SvxLink mit `AUTH_KEY` tut, aktuelle Versionen eingeschlossen. Ein Knoten, der nur für Zertifikate eingerichtet ist, nutzt Protokoll 3 (TLS), das urfd nicht unterstützt: Er wird gebeten, herunterzuschalten, und urfd protokolliert, dass es nicht geht.

**Einen SvxLink-Knoten einrichten**, in `[ReflectorLogic]` (Beispiel für TG26363 auf einem urfd-Modul):
```ini
HOSTS = urf.example.org:5300    # instead of DNS_DOMAIN
CALLSIGN = N0CALL-HS            # the user in [SVXServer]
AUTH_KEY = changeme             # its password
DEFAULT_TG = 26363              # selected on local traffic
MONITOR_TGS = 26363             # heard while idle, and the module urfd links the node on
```
- Mit gesetztem `DEFAULT_TG` schaltet SvxLink die erste Aussendung stumm, nachdem es auf TG 0 zurückgefallen ist (`TG_SELECT_TIMEOUT`, standardmäßig 30 s): `MUTE_FIRST_TX_LOC` ist standardmäßig an und verwirft diese Aussendung, um Testtasten und DTMF-Befehle zu unterdrücken. Die zweite geht durch; `MUTE_FIRST_TX_LOC = 0` sendet auch die erste.
- Pegel: Der Empfänger des Knotens darf nicht übersteuern; `RxGain` auf 0 lassen und die AGC von tcd den Pegel einstellen lassen. Ein Beispiel mit Zahlen steht in [docs/AUDIO.de.md](AUDIO.de.md#fm-pegel-von-svxlink-und-allstar).
- Das FM-Rauschen unter der Sprache klingt nach dem AMBE-Encoder auf DMR roboterhaft; tcd entfernt das meiste davon vor dem Kodieren (`AGCAnalogNoiseSuppress`, siehe [tcd/README.de.md](../tcd/README.de.md)).
- Der Knoten erscheint in den Links auf dem Modul seiner gewählten TG, oder solange er keine hat (TG 0) auf dem seiner ersten zugeordneten überwachten TG, mit der Adresse, die urfd sieht: Ein Knoten im eigenen LAN des Reflektors zeigt seine interne IP.

**Admin-Befehle**:
```json
{"cmd": "svxs_user_add", "token": "...", "callsign": "N0CALL-HS", "password": "secret"}
{"cmd": "svxs_user_remove", "token": "...", "callsign": "N0CALL-HS"}
{"cmd": "svxs_user_list", "token": "..."}
{"cmd": "svxs_peer_list", "token": "..."}
```

## USRP (AllStar)

Verbindet einen AllStar-Knoten (USRP-Kanal von app_rpt) mit einem Modul. Braucht ein transcodiertes Modul.

```ini
[USRP]
Enable = true
Callsign = ALLSTAR
IPAddress = 1.2.3.4            # AllStar node IP
RxPort = 34000
TxPort = 32000
Module = A                     # must be a transcoded module
# FilePath = /usr/local/etc/urfd/clients.txt   # optional: listen-only clients
# RxGain = 0                   # incoming audio gain in dB (-40 to +40)
# TxGain = 0                   # outgoing audio gain in dB (-40 to +40)
```

**RxGain / TxGain**: wie bei SVX, je Verbindung; Samples über Vollaussteuerung werden abgeschnitten. tcd schickt AllStar-Audio durch seine AGC für analoge Quellen, die Werte bleiben also normalerweise 0.

**Sprecher**: Ein TEXT-Paket mit einem SET_INFO-TLV vor der Sprache, wie DVSwitch Analog_Bridge es sendet, nennt den Sprecher (Rufzeichen und DMR-ID); urfd sendet dasselbe Paket vor jedem Stream, den es an den Knoten spielt. Ohne läuft der Stream unter `Callsign`, mit dessen DMR-ID aus der Datenbank; sonst bekommen DMR-Hotspots, DMR+ und P25 ihn nur mit `[MMDVM] FallbackDmrId`. Das Unkey beendet den Stream.

## Protokolle sperren

`BlockProtocols` hält Audio der genannten Protokolle von einer Verbindung fern, in beide Richtungen. Es wirkt bei MMDVM, MMDVMClient, OpenBridge, SvxReflector, SVXServer, DCSClient, DExtraClient, DPlusClient und YSFClient. Eine Client-Anbindung schickt ihr eigenes Audio nie zurück (z.B. schickt MMDVMClient BrandMeisters Audio nie zurück an BrandMeister).

Protokollnamen (durch Komma getrennt): `MMDVM`, `MMDVMClient` (jede Verbindung) oder der Name einer Verbindung (`MMDVMClient.TGIF`, `MMDVMClient1` bis `MMDVMClient9`), `OpenBridge`, `SvxReflector`, `SVX` oder `SVXServer`, `DExtra`, `DExtraClient`, `DPlus`, `DPlusClient`, `DCS`, `DCSClient`, `DMRPlus`, `YSF`, `YSFClient`, `M17`, `NXDN`, `P25`, `USRP`, `URF`, `XLXPeer`, `G3` und der Name eines geladenen [Plugins](#plugins). Die Admin-Seite sperrt und entsperrt Paare im laufenden Betrieb (nicht über einen Neustart behalten; „Reset to Config Default“ stellt die INI-Einstellungen wieder her).

## Reflektor-Kopplung

urfd koppelt sich mit URF-, XLX-, DCS- und M17-Reflektoren (mrefd), die in `urfd.interlink` stehen. Nur die dort genannten Reflektoren dürfen koppeln; beide Seiten müssen einander eintragen. Hostnamen werden beim Laden aufgelöst.

```
# urfd.interlink
# Format: <Callsign> <Address> <Modules> [Port] [Protocol]
URF270 urf270.example.com EF
XLX269 xlx269.example.com A
DCS002 dcs002.xreflector.net S             # native DCS protocol (default)
DCS002 dcs002.xreflector.net S XLX         # XLX protocol instead
M17-QQQ m17-qqq.example.com AB             # mrefd >= 1.0 (port 17000)
M17-QQQL m17-qqq.example.com AB            # legacy mrefd < 1.0
M17-QQQ AB                                 # mrefd, address from the Ham-DHT
```

| Art | Präfix | Standard-Port | Protokoll | Voraussetzung |
|-----|--------|---------------|-----------|---------------|
| URF-Partner | `URF` | 10017 | URF nativ | - |
| XLX-Partner | `XLX` | 10002 | XLX | `[XLXPeer] Enable = true` |
| DCS-Partner | `DCS` | 30051 | DCS nativ | - |
| DCS-Partner (XLX) | `DCS` + `XLX` | 10002 | XLX | `[XLXPeer] Enable = true` |
| M17-Partner (mrefd) | `M17-` | 17000 | M17-Interlink | `[M17] Callsign = M17-XXX` |
| M17-Partner (altes mrefd < 1.0) | `M17-…L` | 17000 | M17-Interlink | `[M17] Callsign = M17-XXX` |

Das Protokollfeld gibt es nur für DCS-Einträge. Mit OpenDHT lassen sich URF-Partner auch über die DHT finden (ohne Adresse).

```ini
# urfd.ini
[XLXPeer]
Enable = true               # required for XLX peers and DCS peers with XLX protocol
Port = 10002
# Callsign = XLX999         # optional: callsign for XLX peering

[DCS]
Port = 30051
# Callsign = DCS999         # optional: callsign for native DCS peering

[M17]
# Callsign = M17-XXX        # required for mrefd peers; unique, it is published in the Ham-DHT

[URF]
# InterlinkVersion = 4      # 4 (default) or 3 for legacy mode
```

**URF-Interlink-Version**: urfd meldet 4 und ergänzt den Stream-Header für Partner, die ebenfalls 4 melden, um META-Typ und CAN von M17, so bleibt GNSS über gekoppelte Reflektoren GNSS; Partner, die 3 melden (andere urfd-Forks), bekommen den alten Header. `InterlinkVersion = 3` meldet 3 und sendet die Erweiterung nie.

**mrefd**: M17-Einträge brauchen dieselben Module auf beiden Seiten und das `[M17] Callsign` dieses Reflektors in der `mrefd.interlink` des mrefd. Ein Eintrag nur mit den Modulen (`M17-XXX AB`) nimmt Adresse, Port und Alt-Status aus dem Ham-DHT-Dokument des mrefd; urfd veröffentlicht sein eigenes dort in mrefds Format, so kann ein mrefd diesen Reflektor genauso eintragen. Einen Bezeichner wählen, den kein anderer M17-Reflektor nutzt. Verbindung, Keepalive, Sprache und Packet-Daten folgen mrefd: Daten an ein mrefd >= 1.0 tragen das Modul als zusätzliches Byte, ein altes wird als `M17-XXX <Modul>` adressiert und bekommt keine Packet-Daten. Ein Stream von einem mrefd wird nicht an ein anderes weitergegeben. Gekoppelte mrefds erscheinen als M17-Partner.

## Echo-Modul

Ein Papagei: Was auf dem Modul gesprochen wird, wird nach 3 Sekunden Stille wiedergegeben.

```ini
[Modules]
Modules = AFSZ

[Echo]
Enable = true
Module = Z
```

M17-Clients erreichen es von jedem Modul aus, indem sie an `PARROT` oder `ECHO` senden, siehe [M17](#m17).

## Plugins

Ein Protokoll kann auch aus einer Shared Object kommen, die urfd beim Start lädt, für eines, das nicht zu diesem Repository gehört. `[Plugins]` nennt die Dateien, je eine `Load`-Zeile:

```ini
[Plugins]
Load = /usr/local/lib/urfd/myproto.so
```

Ein Plugin ist C++, gebaut gegen diesen Quellbaum (siehe [reflector/Plugin.h](../reflector/Plugin.h)): Sein Protokoll ist ein `CProtocol` wie die eingebauten und nutzt die Klassen und Globals von urfd, die das Programm `urfd` exportiert. Es passt nur zu dem urfd, für den es gebaut wurde: urfd vergleicht Version, Build-Optionen (`DHT`, `debug`) und die Größe seiner Hauptklassen und lehnt ein aus anderen Quellen gebautes Plugin ab, also die Plugins bei jedem urfd-Update neu bauen.

- **Einstellungen**: Ein Plugin namens `Foo` liest den Abschnitt `[Foo]` der `urfd.ini`, bei jedem Start und Neuladen. `inicheck` vermerkt Plugin-Abschnitte nur.
- **Routing**: `Foo` ist ein Protokollname in jedem `BlockProtocols`, `Protocols<M>` und den Admin-Befehlen `block`/`unblock`; seine Clients gehen durch den Gatekeeper wie die anderen.
- **Admin-Socket**: Seine eigenen Befehle gehen per Name oder als `{"cmd": "plugin", "name": "Foo", "command": "...", ...}`; `status` listet die Plugins mit dem, was sie hinzufügen, `tg_list` ihre Module.
- **Dashboard**: Jedes Plugin beschreibt seine Abschnitte in der Antwort auf `status`, und die Admin-Seite zeichnet sie, siehe [docs/PLUGINS.md](PLUGINS.md).
- **Logins**: Es kann seine Logins unter einem eigenen Dienstnamen in der [Zugangsdatenbank](#zugangsdatenbank) halten.
- **Bauen**: Den Quelltext des Plugins nach `plugins/<Name>/` legen (mit einem `Makefile`, das `URFD=` nimmt), z.B. als `git clone` seines Repositorys; git ignoriert ihn dort. Das Docker-Image baut es mit urfd nach `/usr/local/lib/urfd/`; nativ tun `make plugins` und `sudo make plugins-install` in `reflector/` dasselbe. `setup.sh update` holt jedes Plugin-Verzeichnis, das ein git-Klon ist (auch in `tcd/plugins/`), vor dem Bauen neu. Siehe [plugins/README.md](../plugins/README.md).

## Admin-Schnittstelle

Verwaltung im laufenden Betrieb über die Admin-Seite des Dashboards, die sich über den [WebSocket](#websocket) anmeldet, und über einen JSON-Socket für Scripte. Sie läuft in einem eigenen Thread; jede Socket-Verbindung bekommt einen eigenen Thread mit 5 s Leerlauf-Timeout, so blockiert ein hängender Client nichts.

```ini
[Admin]
Enable = true
Port = 10101
Password = yoursecretpassword
# BindingAddress = 127.0.0.1      # default: loopback only
```

Die Admin-Seite (π in der Fußzeile des Dashboards oder `?show=admin`, sichtbar mit `[Admin] Enable = true` und `[Dashboard] Admin = true`; ohne Letzteres ist nur der Socket offen) meldet sich mit dem `[Admin] Password` an. Die Anmeldung ist ein Token für diesen Browser-Tab, gültig bis eine Stunde nach seinem letzten Befehl; ein Neuladen behält ihn. Ein falsches Passwort pausiert Anmeldungen von dieser Adresse (2 s, verdoppelt bis 5 min). Das Dashboard bewahrt keine Kopie des Passworts auf.

**Was sie kann:**
- **Sprechgruppen**: TGs im laufenden Betrieb mit Timeout hinzufügen und entfernen, auch sekundäre (nur Empfang); mit BrandMeister-API-Schlüssel werden sie bei BrandMeister als statisch angemeldet, ohne aktiviert sie ein **Kerchunk**
- **Protokollsperren**: Protokollpaare im laufenden Betrieb sperren und entsperren (beim Neustart zurückgesetzt)
- **D-Star- und YSF-Verbindungen**: DCS-, DExtra-, DPlus- und YSF-Reflektorverbindungen hinzufügen und entfernen
- **MMDVM-Server und SVXServer**: Benutzer, TG-Zuordnungen und verbundene Knoten (DMR-ID, Frequenzen, Standort, Software)
- **Neu verbinden**: MMDVMClient, SvxReflector und die Client-Anbindungen
- **Transcoder**: Verbindung, Codec, Paketzähler und Laufzeit je Modul
- **Log**: die letzten 50 Zeilen, alle 10 s aktualisiert (der Socket gibt bis zu 200); **Last heard leeren**

**Socket-Protokoll**: zeilenweises JSON über TCP. Zuerst anmelden, dann Befehle mit dem zurückgegebenen Token schicken:
```json
{"cmd": "auth", "password": "..."}
{"cmd": "tg_add", "token": "...", "protocol": "mmdvm", "tg": 26207, "module": "S", "ts": 1, "ttl": 900}
{"cmd": "tg_remove", "token": "...", "protocol": "mmdvm", "tg": 26207}
{"cmd": "tg_list", "token": "..."}
{"cmd": "kerchunk", "token": "...", "tg": 26207}
{"cmd": "tc_stats", "token": "..."}
{"cmd": "reconnect", "token": "...", "protocol": "mmdvm"}
{"cmd": "log", "token": "...", "lines": 50}
{"cmd": "status", "token": "..."}
{"cmd": "block", "token": "...", "a": "SVX", "b": "MMDVM"}
{"cmd": "unblock", "token": "...", "a": "SVX", "b": "MMDVM"}
{"cmd": "block_reset", "token": "..."}
{"cmd": "clear_users", "token": "..."}
```

Die Befehle der D-Star- und YSF-Anbindungen, des MMDVM-Servers und des SVXServers stehen in ihren Abschnitten oben; die Zugangsdatenbank hat `credentials_status` und `credentials_flush`.

## Zugangsdatenbank

Die Logins von `[MMDVM]` und `[SVXServer]` (und von [Plugins](#plugins), die sie nutzen) können auch aus einer Datenbank kommen, damit ein anderes Programm sie verwalten kann: SQLite, MySQL/MariaDB oder PostgreSQL. Benutzer in der `urfd.ini` gelten weiter und haben Vorrang; nur ein Benutzer, der dort nicht steht, wird nachgeschlagen, und die Antwort bleibt in einem Zwischenspeicher (`CacheTTL`, Standard 300 s; unbekannte Benutzer `NegativeTTL`, 60 s). Fällt die Datenbank aus, funktionieren zwischengespeicherte Logins weiter. Die Admin-Seite legt neue Benutzer in der Datenbank an. Passwörter stehen im Klartext, weil die Protokolle sie für ihre Challenge brauchen. Standardmäßig aus:

```ini
[Credentials]
Backend = sqlite           # ini (no database), sqlite, mysql or postgresql
Path = /usr/local/etc/urfd/credentials.db
# Host = localhost         # mysql, postgresql: Host, Port, Database, User, Password
# Table = urfd_credentials
# CacheTTL = 300
```

Tabelle, Zwischenspeicher, Bauen und die Admin-Befehle: [docs/CREDENTIALS.md](CREDENTIALS.md) (englisch).

## WebSocket

Alles, was das Dashboard zeigt, kommt von hier: Clients, Peers und sprechende Stationen (mit dem Namen des Funkers aus der ID-Datenbank), sobald sie sich ändern, die Last-Heard-Liste mit Länge und Klangbericht jedes Durchgangs, die Brücken jedes Moduls (TG-Zuordnungen, Autolinks, Anbindungen) und ihre Änderungen, und der ganze Zustand, wenn sich ein Client verbindet; IP-Adressen nur maskiert, wie `[Dashboard]` es erlaubt. Die Admin-Seite schickt ihre Befehle über dieselbe Verbindung. Er lauscht auf Loopback (`127.0.0.1:10102`), und das nginx des Dashboards reicht `/ws` durch. Immer eingebaut (braucht `libwebsockets-dev`); `[WebSocket]` ist ohne `Enable`-Zeile an (`Enable = false` schaltet es aus, das Dashboard bleibt dann leer); Änderungen brauchen einen Neustart. urfd schreibt keine XML- oder JSON-Statusdatei. Nachrichten, Anfragen und Einrichtung: [docs/WEBSOCKET.md](WEBSOCKET.md) (englisch).

## Dashboard

```ini
[Dashboard]
# Motd = Scheduled maintenance on Sunday 12:00-14:00 UTC   # a banner on every page
# LastHeard = 40               # rows of the last heard list
# Filter = true                # module keys and callsign filter on Last heard
# Links = 100                  # rows on the Links page
# Peers = 100                  # rows on the Peers page
# LinksIP = HideIP             # (default) or ShowLast1ByteOfIP ... ShowLast3ByteOfIP, ShowFullIP
# PeersIP = HideIP             # IPv6: the last 1, 2 or 3 groups
# IPMask = *                   # the character for a hidden byte
# Admin = false                # the admin page, with [Admin] Enable
```

Die Fußzeile nennt `[Names] SysopEmail` (gegen Bots verschleiert). Das Dashboard hat keine eigene JSON-API (`/json/*` antwortet 410); seine Daten stehen in den Nachrichten des WebSockets. Alte Links (`index.php?show=...`) führen zu denselben Seiten. Die Reflektorliste kommt aus dem XLX-Verzeichnis über das nginx des Dashboards, das 5 Minuten lang eine Kopie hält und eine ältere ausliefert, solange das Verzeichnis ausfällt; der QuadNet-Feed geht ebenfalls über nginx. Ein offen gelassener Tab lädt sich nach einem Update selbst neu.

## Konfiguration per SIGHUP neu laden

urfd liest einen großen Teil seiner Konfiguration neu, ohne Clients zu trennen:

```bash
sudo ./setup.sh reload

# von Hand, Docker: das Signal muss urfd erreichen, nicht supervisord (PID 1 startet bei SIGHUP alles neu)
docker exec urfd pkill -HUP -x urfd

# ohne Docker
sudo pkill -HUP -x urfd
```

**Per SIGHUP neu geladen:**
- TG-Zuordnungen (MMDVMClient, OpenBridge, SvxReflector, SVXServer); dynamische TGs bleiben, die MMDVMClient-Verbindungen verbinden sich neu
- `[MMDVM] DynamicTimeout`
- Whitelist, Blacklist und Interlink-Datei
- Transcodierte Module (`[Transcoder] Modules`, auch von und zu `auto`); eine tcd-Verbindung für ein nicht mehr genanntes Modul wird geschlossen
- Modulzugang je Protokoll und verborgene Sprecher (`[Modules] Protocols<M>`, `HideUnlisted`)
- Die Abschnitte der Plugins und in der Zugangsdatenbank geänderte Logins (ihr Zwischenspeicher wird geleert; `[Credentials]` selbst braucht einen Neustart)
- `[Dashboard]` und die Metadaten des Reflektors (Sponsor, Land, URLs) für Dashboards, die sich danach verbinden; das XLX-Verzeichnis behält `DashboardUrl` bis zum Neustart

**Braucht einen Neustart:**
- Rufzeichen, Datenbank-Einstellungen (Mode, URL, RefreshMin)
- `[MMDVM]`-Sprechgruppen, -Benutzer, `FallbackDmrId` und `RequireAuth`; `[SVXServer]`-Benutzer (Benutzer lassen sich im laufenden Betrieb auf der Admin-Seite ändern)
- `[Files]`
- Module hinzufügen oder entfernen
- Protokolle ein- oder ausschalten, Ports
- IP-Bindung
- Einstellungen der Admin-Schnittstelle
- Echo-Modul
- Port, Bindungsadresse und Passwort des Transcoders
