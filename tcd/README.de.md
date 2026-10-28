# tcd — Hybrider Transcoder für digitale Sprache

[English](README.md) | **Deutsch**

**Version 0.14.0** — der Transcoder von [urfd-ng](../README.de.md), im Ordner `tcd/` desselben Repositorys. Basiert auf [n7tae/tcd](https://github.com/n7tae/tcd).

tcd wandelt Sprache zwischen den digitalen Betriebsarten eines URF-Reflektors um, damit ein D-Star-Nutzer einen DMR-Nutzer hört und umgekehrt. Für D-Star (AMBE) und DMR/YSF/NXDN (AMBE2+) nutzt er DVSI-AMBE-Hardware, für M17 (Codec2) und P25 (IMBE) Software-Vocoder. [Codec-Plugins](#codec-plugins) können D-Star und DMR/YSF/NXDN in Software ergänzen, seit 0.14.0 auch P25 als Wahl neben dem eingebauten IMBE-Vocoder. Der Reflektor gibt das Audio seiner transcodierten Module über TCP an tcd und bekommt alle anderen Betriebsarten zurück.

## Inhalt

- [Installation](#installation)
- [Hardware](#hardware)
- [Codec-Plugins](#codec-plugins)
- [tcd in Docker](#tcd-in-docker)
- [AMBE-Geräte auswählen](#ambe-geräte-auswählen)
- [Konfiguration (tcd.ini)](#konfiguration-tcdini)
- [Pegel](#pegel)
- [Überwachung](#überwachung)
- [Betrieb und Fehlersuche](#betrieb-und-fehlersuche)
- [Änderungen gegenüber dem Original](#änderungen-gegenüber-dem-original)

## Installation

### Mit dem Setup-Script

Am einfachsten: Das Setup-Script im Wurzelverzeichnis des Repositorys installiert Reflektor und Transcoder zusammen oder den Transcoder allein auf einem zweiten Rechner, stellt ein paar Fragen und schreibt `tcd.ini`. Siehe [docs/INSTALL.de.md](../docs/INSTALL.de.md).

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd
sudo ./setup.sh
```

### Mit build.sh

Nur den Transcoder bauen und installieren:

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd
sudo bash tcd/build.sh
```

`build.sh` installiert die Build-Werkzeuge, `libspeexdsp-dev` und `libwebsockets-dev`, baut tcd als Eigentümer des Klons und installiert `tcd`, `tcdmon`, `agc-analyze` und die systemd-Unit. Ein Codec-Plugin in `tcd/plugins/<name>/` (ein `git clone` seines Repositorys, siehe [Codec-Plugins](#codec-plugins)) wird gegen diesen tcd mitgebaut und nach `/usr/local/lib/tcd/` installiert. Ein laufender tcd wird neu gestartet. Bei der ersten Installation landet die Vorlage unter `/usr/local/etc/tcd.ini`; die bitte anpassen (siehe [Konfiguration](#konfiguration-tcdini)) und mit `sudo systemctl start tcd` starten.

Aktualisieren: `sudo ./setup.sh update` (holt auch die Plugin-Klone in `tcd/plugins/`), oder von Hand `git pull` im Klon und in jedem Plugin-Klon, dann `build.sh` erneut aufrufen.

### Von Hand

Mit installiertem `libspeexdsp-dev`, `libwebsockets-dev` und `libncurses-dev`:

```bash
cd urfd/tcd
make -j$(nproc)
sudo make install
```

Codec2 (`codec2/`) und der P25-IMBE-Vocoder aus OP25 (`imbe/`) gehören zum Quelltext, es braucht also keine weitere Bibliothek und kein weiteres Repository.

tcd baut gegen die urfd-Quellen, die er mit dem Reflektor teilt (`TCPacketDef.h`, `TCSocket.*`, ...), aus `../reflector`; beide Seiten sprechen also immer dasselbe Protokoll. `config/tcd.ini` ist die kommentierte Vorlage, `config/tcd.service` die systemd-Unit.

### macOS

tcd lässt sich auch unter macOS (Apple Silicon oder Intel) bauen und läuft dort nativ mit den AMBE-Geräten am USB; Docker Desktop kann keine USB-Geräte durchreichen. Mit den Xcode Command Line Tools und [Homebrew](https://brew.sh):

```bash
brew install speexdsp libwebsockets
cd urfd/tcd
make -j$(sysctl -n hw.ncpu)
./tcd --list-devices
./tcd /pfad/zu/tcd.ini        # Strg-C beendet ihn
```

- macOS hat kein `/dev/serial/by-id`: tcd findet die seriellen USB-Ports (`/dev/cu.*`) über IOKit und benennt sie wie udev (`usb-FTDI_DVstick-33_DKB7FXGE-if00-port0`), die Erkennung der AMBE-Geräte und `DeviceSerial` funktionieren also wie unter Linux. Es wird Apples eigener FTDI-Treiber verwendet, ein zusätzlicher Treiber ist nicht nötig.
- tcd setzt auch dort den FTDI-Latenz-Timer auf 1 ms. 460800 und 921600 Baud, die das termios von macOS nicht kennt, setzt er über IOKit.
- `make install` und die systemd-Unit sind für Linux; unter macOS tcd von Hand starten (oder mit einem launchd-Job). Ein Codec-Plugin in `plugins/<name>/` baut wie unter Linux.

### Voraussetzungen

- Linux mit systemd: Debian, Ubuntu oder Raspberry Pi OS; x86_64, arm64 oder armhf; oder macOS mit Homebrew (siehe [macOS](#macos))
- DVSI-AMBE-USB-Geräte (siehe [Hardware](#hardware)); sie laufen über den Kernel-Treiber `ftdi_sio`, ein zusätzlicher Treiber ist nicht nötig
- `libspeexdsp-dev` für die Rauschunterdrückung analoger Eingänge und `libwebsockets-dev` für das Web-Dashboard; `build.sh` installiert beide
- Geringe Latenz zu den AMBE-Geräten: tcd setzt ihren USB-Latenz-Timer selbst auf 1 ms, nativ und in Docker (siehe [tcd in Docker](#tcd-in-docker)). Er kann auf dem Rechner des Reflektors laufen oder auf einem anderen, z.B. einem Raspberry Pi neben der Funktechnik.

## Hardware

Jedes transcodierte Modul braucht einen D-Star-Kanal und einen DMR/YSF-Kanal. Ein DV3000 (ThumbDV, DVstick-30, USB-3000) hat einen Kanal, ein DV3003 (DVstick-33, USB-3003) drei. Einen Codec ohne Gerät kann ein [Codec-Plugin](#codec-plugins) übernehmen.

| Hardware | Codec-Plugin | Module | Hinweis |
|---|---|---|---|
| 1× DV3000 oder 1× DV3003 | eines für DMR (oder D-Star) | 1 | D-Star auf dem Gerät, DMR/YSF in Software |
| DV3000 + DV3003 | — | 2 | Mischbetrieb, empfohlen |
| 2× DV3000 | — | 1 | eines für D-Star, eines für DMR/YSF |
| 2× DV3003 | — | bis zu 3 | eines für D-Star, eines für DMR/YSF |
| keine | eines für D-Star und DMR | beliebig | alles in Software |

Ein einzelnes Gerät braucht ein Codec-Plugin für den anderen Codec; ohne startet tcd nicht. Bei zwei Geräten ist kein Plugin nötig: Seit 0.2.5 bekommt DMR → DMR/YSF mit `DmrGainMode = ambe` dieselbe AGC ohne Neukodieren (siehe [`DmrGainMode`](#pegel-dmrysf--dmrysf-dmrgainmode)); ein DMR-Plugin bringt nur `reencode` als Alternative mit. tcd erkennt die Geräte und wählt den Betrieb selbst.

### Mischbetrieb: DV3000 + DV3003

Zwei Module gleichzeitig, die Kanäle werden automatisch belegt:

| Kanal | Verwendung |
|---|---|
| DV3000 ch0 | D-Star, erstes Modul |
| DV3003 ch0 | D-Star, zweites Modul |
| DV3003 ch1 | DMR/YSF, erstes Modul |
| DV3003 ch2 | DMR/YSF, zweites Modul |

```ini
Modules = FS            # first = D-Star on the DV3000, second = D-Star on the DV3003
DeviceSerial = DQ015SBR # DV3000
DeviceSerial = DKB7FXGE # DV3003
```

### Einzelnes Gerät mit Codec-Plugin

Ein Modul, D-Star auf dem DV3000 oder DV3003, DMR/YSF über ein Codec-Plugin. In diesem Betrieb wird DMR/YSF → DMR/YSF nach der AGC **nicht neu kodiert**. Für diesen Weg die Bitstrom-Verstärkung (`DmrGainMode = ambe`, folgt der AGC; in diesem Betrieb seit 0.2.5) verwenden; alle Wege zwischen verschiedenen Betriebsarten bekommen weiterhin die volle AGC. Neukodieren bei DMR/YSF → DMR/YSF braucht ein zweites AMBE-Gerät.

### Hinweise zum DV3003

- **Companding aus**: Bei jedem DV3003-Kanal wird Companding ausdrücklich abgeschaltet. Der DV3003 startet mit eingeschaltetem Companding (Pull-up an CP_ENABLE), der DV3000 mit ausgeschaltetem; ohne die Abschaltung bekamen die DMR/YSF-Kanäle µ-law-PCM, das als linear gelesen wurde, und das Audio war verzerrt.
- **Seriellen Puffer leeren**: Vor dem Soft-Reset leeren 350 Null-Bytes einen hängengebliebenen seriellen Zustand.
- **Codec pro Kanal**: Ein DV3003 kann gleichzeitig D-Star- und DMR/YSF-Kanäle betreiben (Mischbetrieb).

## Codec-Plugins

Ein Codec-Plugin ist eine Shared Library, die D-Star (AMBE) und/oder DMR/YSF (AMBE2+) in Software mitbringt, seit 0.14.0 auch P25 (IMBE) als Wahl neben dem eigenen OP25-Vocoder von tcd. tcd lädt es beim Start:

```ini
CodecPlugin = /usr/local/lib/tcd/foo.so   # so viele Zeilen wie nötig
CodecDStar  = dvsi                        # der Vocoder für neue D-Star-Streams: dvsi oder der Name eines Plugins
CodecDMR    = foo                         # dasselbe für DMR/YSF
CodecP25    = op25                        # P25: op25 (der eigene von tcd, Standard) oder der Name eines Plugins
```

- **Welcher Vocoder**: Ohne `CodecDStar`/`CodecDMR` haben die Geräte Vorrang, ein Plugin übernimmt nur, was kein Gerät macht. Steht dort der Name eines Plugins, läuft dieser Codec von Anfang an in Software, auch wenn ein Gerät ihn könnte; bei nur einem Gerät übernimmt das Gerät dann den anderen Codec. P25 läuft für alle Module über den eigenen OP25-Vocoder von tcd, außer `CodecP25` nennt ein Plugin, das P25 kann; nennt es ein Plugin, das nicht geladen ist oder kein P25 kann, startet tcd nicht.
- **Mehr Module als Kanäle**: Die Geräte übernehmen die ersten Module aus `Modules`, so viele, wie sie Kanäle haben (Mischbetrieb 2, zwei DV3003 3, ...); die übrigen Module gehen immer an ein Plugin. Mit DV3000 + DV3003 und `Modules = FSABC` werden F und S auf den Geräten transcodiert, A, B und C in Software. Ohne Plugin für den Rest startet tcd nicht.
- **Umschalten zur Laufzeit**: Das Web-Dashboard (Einstellungen, „Vocoder“) und `POST /api/codec` mit `{"codec": "dstar", "use": "foo"}` (`dstar`, `dmr` oder `p25`) wählen den Vocoder für die Streams, die ab dann beginnen; eine laufende Aussendung behält ihren Vocoder. Angeboten wird alles, was beim Start geladen wurde: ein Gerät für den Codec (für P25 `op25`) und jedes Plugin, das ihn kann. „Speichern“ schreibt die Wahl in die `tcd.ini`.
- **Optionen**: Zeilen der `tcd.ini`, die tcd nicht kennt, gehen an die Plugins, etwa ein Pfad, den ein Plugin braucht; eine Zeile, die niemand nimmt, wird gemeldet.
- **Schnittstelle**: schlichtes C, [`CodecPlugin.h`](CodecPlugin.h): `open`/`close` eines Zustands pro Stream, `encode`/`decode` von 160 Samples PCM mit 8 kHz in einen und aus einem 9-Byte-Frame (P25: 11 Byte), `start`/`stop` vor dem ersten und nach dem letzten Frame einer Aussendung (seit 0.12.0, `TCD_CODEC_API 2`), damit ein Plugin jede Aussendung frisch beginnt, auch wenn eine Stream-ID wiederkommt. tcd ruft ein Plugin immer nur aus einem Thread zur Zeit auf und schließt den Zustand eines Streams, der 30 s still war. Ein Plugin wird nur gegen diesen Header gebaut und muss nicht bei jedem tcd-Update neu gebaut werden; tcd prüft `TCD_CODEC_API`.
- **Frames**: D-Star als die 9 Datenbytes eines DV-Frames, DMR/YSF als die 72 Bits eines AMBE2+-Frames in Luft-Reihenfolge, MSB zuerst, so wie die DVSI-Geräte sie nutzen; P25 als die 88 IMBE-Parameterbits u0..u7 in 11 Byte, MSB zuerst, wie der OP25-Vocoder von tcd sie hat. Ein Fehler des Plugins ergibt für diesen Frame IMBE-Stille (Kodieren) oder stille Samples (Dekodieren).

Dieses Repository enthält kein Codec-Plugin: Software-Umsetzungen von AMBE und AMBE2+ können Patente berühren (AMBE2+ Halbrate, US 8,359,197, voraussichtlich bis 20.05.2028), ob man eines baut und nutzt, entscheidet der Betreiber. Ein Plugin für D-Star, DMR/YSF und P25 mit [mbelib-neo](https://github.com/arancormonk/mbelib-neo) ist [tcd-mbelib-neo-plugin](https://github.com/jcmerg/tcd-mbelib-neo-plugin); bitte zuerst dessen Patenthinweis lesen.

`tools/` enthält Testwerkzeuge für Plugins (`make` dort): `codectest <plugin.so> dmr|dstar|p25 in.raw out.frames out.raw [option=wert ...]` kodiert und dekodiert 8-kHz-s16le-PCM, gibt die Zeit pro Frame aus und prüft, dass zwei verschachtelte Streams getrennt bleiben; `decode` dekodiert eine Frame-Datei; `dvsitest <Gerät> enc|dec dstar|dmr ein aus` tut dasselbe auf einem DV3003, oder mit `DVSI_TYPE=3000` auf einem DV3000 (tcd gestoppt), um ein Plugin mit der Hardware zu vergleichen; `dvsitest <Gerät> info` gibt Produkt-ID und Version des Chips aus.

## tcd in Docker

tcd läuft genauso gut in einem Container, neben urfd oder auf einem eigenen Rechner: mit AMBE-Sticks oder in Software mit einem [Codec-Plugin](#codec-plugins) für D-Star und DMR. `setup.sh` fragt, ob tcd in Docker oder nativ mit systemd läuft.

Von Hand, mit der Konfiguration in `$URFD_DATA/tcd/tcd.ini` (Standard `/opt/urfd/tcd`):

```bash
cd ~/urfd
# nur für D-Star/DMR in Software: das Codec-Plugin, vorher dessen Patenthinweis lesen
git clone https://github.com/jcmerg/tcd-mbelib-neo-plugin.git tcd/plugins/mbelib-neo
sudo mkdir -p /opt/urfd/tcd && sudo cp tcd/config/tcd.ini /opt/urfd/tcd/tcd.ini
sudo nano /opt/urfd/tcd/tcd.ini
sudo docker/build.sh start
```

`docker/build.sh` baut und startet, wofür der Rechner eine Konfiguration hat: urfd mit `$URFD_DATA/config/urfd.ini`, tcd mit `$URFD_DATA/tcd/tcd.ini`, es taugt also auch für einen Rechner nur mit tcd. Eine `tcd.ini` mit `DeviceSerial` oder ohne `CodecPlugin` heißt AMBE-Sticks: build.sh nimmt `docker/docker-compose.tcd-sticks.yml` dazu, die dem Container das `/dev` des Hosts nur lesend und die USB-Seriell-Geräte (Major 188) freigibt. tcd findet die Sticks wie nativ über `/dev/serial/by-id`, auch wenn ein Stick nach dem Neueinstecken eine andere Gerätedatei hat, und setzt ihren USB-Latenz-Timer selbst auf 1 ms (per Low-Latency-ioctl, ohne `/sys`; tcd zeigt den Wert beim Start im Log). tcd beendet sich, wenn ihm ein Gerät verloren geht, und der Container startet neu. Im Container: `docker exec -it tcd tcdmon`.

In der `tcd.ini`: `ServerAddress = 127.0.0.1` neben urfd (der Container nutzt das Netz des Hosts), sonst die Adresse des Reflektors; `Modules` so viele wie gewünscht; mit Sticks `DeviceSerial` wie nativ, in Software `CodecPlugin = /usr/local/lib/tcd/mbelib_neo_codec.so` und keine `DeviceSerial`; ein `MonitorHttpPort`, der auf dem Host frei ist. In der `urfd.ini`: `[Transcoder] Modules = auto`, dann entscheidet allein die Liste von tcd. Das Image baut jedes `tcd/plugins/<name>/` mit `Makefile` (Git ignoriert das Verzeichnis); die `tcd.ini` ist beschreibbar für „Speichern“ im Monitor. Aktualisieren: `sudo ./setup.sh update`, oder von Hand `git pull` im Klon und in jedem Plugin-Klon (`git -C tcd/plugins/<name> pull`), dann `docker/build.sh`. Die Container tragen ein Label, das watchtower vom Ersetzen abhält, denn sie werden hier gebaut und nicht geladen.

Neben urfd reicht der nginx im urfd-Image `/tcd/` an den Monitor weiter (seit 4.26.0), über den TLS-Proxy davor ist er also als `https://<reflektor>/tcd/` erreichbar; der Proxy reicht `/tcd/` mit denselben Upgrade-Headern wie `/ws`, `X-Real-IP` und `X-Forwarded-Proto` weiter. Der Port ist der `MonitorHttpPort` aus `$URFD_DATA/tcd/tcd.ini`: `docker/build.sh` liest ihn und gibt ihn dem urfd-Container als `TCD_MONITOR_PORT` mit (Standard 8080; bei `docker compose` von Hand in der Umgebung setzen). `MonitorBindAddress = 127.0.0.1` reicht dann; `MonitorPassword` setzen. Über HTTPS ist das Session-Cookie `Secure`.

### Standby: ein zweiter tcd für dieselben Module

`StandbyModules` (seit 0.11.0, urfd 4.26.0) nennt Module, die ein tcd nur übernimmt, solange kein anderer tcd sie hat: Ein Software-tcd in Docker neben urfd steht für die Module des tcd mit den Sticks bereit, übernimmt sie, wenn der weg ist, und gibt sie zurück, sobald er wiederkommt.

```ini
# tcd in Docker neben urfd, mit dem mbelib-neo-Plugin
Modules        = AP      # seine eigenen Module
StandbyModules = FS      # die Module des tcd mit den Sticks, solange der weg ist
```

Der Standby-tcd schickt den Modulbuchstaben klein; der Reflektor nimmt ihn nur für ein Modul ohne tcd an und antwortet sonst „belegt“, und eine normale Verbindung ersetzt eine Standby-Verbindung. Der Standby-tcd fragt alle 3 s still nach, in einem eigenen Thread, damit das Nachfragen seine eigenen Streams nie aufhält (seit 0.12.2). Ein tcd, der beendet wird oder seine Verbindung verliert, übergibt nach Sekunden (eine geschlossene Verbindung bemerkt urfd unter Linux sofort); einen, der ohne Abbau verschwunden ist (Strom, Netz), bemerkt der Reflektor per TCP-Keepalive nach etwa 25 s. Ein Stream, der gerade läuft, während ein Modul den tcd wechselt, wird erst ab dem nächsten Durchgang transcodiert. urfd braucht `[Transcoder] Modules = auto` (oder die Buchstaben dort) und 4.26.0; ein älterer urfd lehnt die Standby-Verbindungen ab, und tcd sagt das einmal.

## AMBE-Geräte auswählen

```bash
tcd --list-devices
```

listet die seriellen USB-Geräte aus `/dev/serial/by-id` (unter macOS aus IOKit, siehe [macOS](#macos)) mit ihren Seriennummern und markiert die, die nach AMBE-Geräten aussehen.

Ohne `DeviceSerial` in `tcd.ini` verwendet tcd jedes Gerät, das er am Namen als AMBE erkennt. Hängen weitere serielle USB-Geräte am Rechner (GPS, CAT-Kabel, ...), die AMBE-Geräte ausdrücklich eintragen. `DeviceSerial` muss exakt einem davon entsprechen:

- der USB-Seriennummer (`DKB7FXGE`)
- der älteren libftd2xx-Form mit dem Port-Buchstaben von Mehrport-Geräten (`DA1234A`)
- dem Namen unter `/dev/serial/by-id`
- der Gerätedatei (`/dev/ttyUSB0`)

tcd öffnet die Geräte über den Kernel-Treiber `ftdi_sio` und setzt ihren USB-Latenz-Timer auf 1 ms (der Standard des Treibers ist 16 ms, zu langsam für das AMBE-Timing; das Log zeigt den Wert). Geräte, die ein älterer, auf libftd2xx basierender tcd vom `ftdi_sio`-Treiber gelöst hat, werden beim Start wieder gebunden; ein Umstieg braucht also kein erneutes Einstecken.

Ob ein Gerät ein DV3000 (ein Kanal) oder ein DV3003 (drei) ist, entnimmt tcd der Produkt-ID, die sein Chip meldet (`AMBE3000…`, `AMBE3003…`), nicht dem USB-Namen, den Nachbauten oft generisch lassen. Die Baudrate richtet sich weiter nach dem Namen: 460800 für die einkanaligen Sticks (ThumbDV, DVstick-30, USB-3000 und die generischen `FT230X_Basic_UART`/`FT232R_USB_UART`), 921600 für die anderen. tcd probiert nie eine andere Rate: Bytes mit falscher Rate können einen AMBE3003 taub machen, bis er ab- und wieder eingesteckt wird.

Ein Gerät, das sich nicht öffnen lässt oder nicht antwortet, wird mit einer Warnung weggelassen, und tcd arbeitet mit den übrigen und den Codec-Plugins weiter, solange sie die Module abdecken (vorher beendete er sich). Ein Gerät, das im Betrieb verloren geht (abgezogen, oder eins, das 3 s lang Pakete bekommt, ohne eins zu beantworten, etwa ein hängender Chip), beendet tcd; sein Dienst startet ihn neu, das Gerät wird weggelassen, und die Codec-Plugins übernehmen seinen Codec, oder ohne eins startet tcd nicht und ein [Standby-tcd](#standby-ein-zweiter-tcd-für-dieselben-module) übernimmt die Module. Jedes Gerät öffnet tcd für sich allein: ein zweiter tcd oder `dvsitest` bekommt `is in use by another process`, statt den Chip unter dem laufenden tcd zurückzusetzen.

## Konfiguration (tcd.ini)

`/usr/local/etc/tcd.ini`; die Vorlage in [`config/tcd.ini`](config/tcd.ini) erklärt jeden Schlüssel. Nach Änderungen tcd neu starten (`sudo systemctl restart tcd`), oder die Audio-Einstellungen live im [Web-Dashboard](#web-dashboard) ändern.

### Verbindung und Geräte

| Schlüssel | Vorlage | Bedeutung |
|---|---|---|
| `ServerAddress` | `127.0.0.1` | IP-Adresse oder Hostname des urfd-Reflektors; ein Hostname wird bei jedem Neuverbinden neu aufgelöst (seit 0.7.4) |
| `Port` | `10100` | `[Transcoder] Port` des Reflektors |
| `Password` | — | `[Transcoder] Password` des Reflektors, nötig, wenn er eins hat (seit 0.8.0) |
| `Modules` | `FS` | transcodierte Module; mit `[Transcoder] Modules = auto` im Reflektor (urfd 4.25.0) beliebige seiner Module, dann entscheidet allein diese Liste; sonst dieselben Buchstaben wie dort |
| `StandbyModules` | — | Module, die dieser tcd nur übernimmt, solange kein anderer tcd sie hat (seit 0.11.0, urfd 4.26.0), siehe [Standby](#standby-ein-zweiter-tcd-für-dieselben-module); nicht zusätzlich in `Modules` |
| `DeviceSerial` | — | zu verwendendes AMBE-Gerät, je eine Zeile (siehe [oben](#ambe-geräte-auswählen)) |

### Verstärkung

Alle Werte in dB, -40 bis +40.

| Schlüssel | Vorlage | Wirkt auf |
|---|---|---|
| `OutputGainDStar` | `0` | D-Star-Ausgang, nach der AGC |
| `OutputGainDMR` | `-14` | DMR/YSF-Ausgang, nach der AGC; jeder Weg nach DMR/YSF, auch DMR → DMR/YSF |
| `OutputGainDMRBitstream` | `-6` | -24 bis 0 dB (0 = alles vor dem Encoder): Bei Quellen aus D-Star, M17, P25, USRP und SVX wird so viel von `OutputGainDMR` nach dem Encoder über b2 im AMBE2+-Frame angewendet, der Rest als PCM davor, der Encoder bekommt also mehr Pegel; live im Monitor |
| `OutputGainM17` | `0` | M17/Codec2-Ausgang, nach der AGC |
| `OutputGainIMBE` | `0` | P25-Ausgang, nach der AGC |
| `OutputGainUSRP` | `0` | USRP- und SVX-Ausgang, nach der AGC (beide bekommen dasselbe PCM von tcd) |
| `DStarGainIn` / `DStarGainOut` | `0` | im DVSI-Chip, D-Star dekodieren / kodieren |
| `DmrGainIn` / `DmrGainOut` | `0` | im DVSI-Chip, DMR dekodieren / kodieren |

Die Verstärkung von SVX und USRP pro Anbindung steht in der `urfd.ini` des Reflektors (`RxGain`/`TxGain` in `[SvxReflector]`, `[SVXServer]` und `[USRP]`); `OutputGainUSRP` hier setzt den Pegel des PCM, das tcd für beide zurückgibt.

### Pegel DMR/YSF → DMR/YSF (`DmrGainMode`)

Audio von DMR nach DMR/YSF läuft normalerweise als AMBE2+ durch, ohne dekodiert zu werden; die AGC kommt daran nicht heran. `DmrGainMode` legt fest, wie sein Pegel angepasst wird:

| Modus | Braucht | Wirkung |
|---|---|---|
| `off` | — | keine Anpassung |
| `ambe` | — | wendet die Verstärkung der AGC plus `AmbeGainDb` (-30 bis +10 dB) und `OutputGainDMR` über den Verstärkungsparameter b2 (delta-gamma) im AMBE2+-Frame an. Kein Neukodieren, kein Qualitätsverlust, repariert nebenbei Golay-FEC-Fehler. Seit 0.2.5 im γ-Bereich und unabhängig vom Pegel (siehe unten). Standard. |
| `reencode` | ein DMR-Codec-Plugin und ein DMR-Gerät | vollständiges Dekodieren und Neukodieren über das Codec-Plugin, mit AGC und `DmrReencodeGain`; eine Alternative zu `ambe`, gleicher Pegel bei gleichem Wert. Ohne DMR-Codec-Plugin Rückfall auf `ambe`. |

| Schlüssel | Vorlage | Standard im Code |
|---|---|---|
| `DmrGainMode` | `ambe` | `ambe` |
| `AmbeGainDb` | `0` | `0` |
| `DmrReencodeGain` | `0` | — |

DMR-Funkgeräte erwarten einen niedrigeren Pegel als das AGC-Ziel (`AGCTarget`, -16 dBFS). `OutputGainDMR = -14` senkt jeden Weg nach DMR/YSF darauf ab, auf etwa -30 dBFS, den Pegel der Sprache bei BrandMeister (gemessen auf TG91: Median -29 dBFS, zwischen den Sprechern -39 bis -23; die -18 vor 0.13.1 ließen DMR/YSF rund 4 dB darunter): den DVSI-Encoder für Quellen aus D-Star, M17, P25 und USRP sowie b2 oder das Neukodieren für DMR-Quellen. `AmbeGainDb` und `DmrReencodeGain` verschieben nur DMR → DMR/YSF gegenüber den anderen Wegen und bleiben normalerweise 0. (tcd 0.2.6 legte die -18 in `AmbeGainDb`/`DmrReencodeGain`, D-Star → DMR war dadurch 18 dB zu laut.)

Mit der PCM-Absenkung bekommt der Encoder den Ton der anderen Codecs mit etwa -34 dBFS, weit unter dem Pegel, für den er gemacht ist. `OutputGainDMRBitstream` verlegt einen Teil davon hinter den Encoder: So viele dB gehen danach über b2 im Frame, pegelunabhängig wie `DmrGainMode = ambe`, der Rest bleibt eine PCM-Absenkung vor dem Encoder. Alles über b2 (0.6.0 hatte nur das, als Schalter) klang über einen SvxLink-Knoten nach DMR (15.10.2026) etwas besser, aber stellenweise mit Artefakten: b2 verschiebt leise Frames in groben Stufen von 3-16 dB, eine große Verschiebung lässt sich auf leisen Silben nicht treffen, und der Fehler wird in den nächsten Frames aufgeholt. Seit 0.7.0 ist es eine Aufteilung, z.B. gibt -9 dem Encoder -25 statt -34 dBFS und b2 nur die halbe Verschiebung. 0 ist die reine PCM-Absenkung; im Monitor lässt es sich während eines QSOs verschieben. Selbst -6 knackte: Zwischen zwei groben b2-Stufen holte die Nachführung den Rundungsfehler im nächsten Frame auf, die Verstärkung leiser Frames sprang also alle 20 ms um 3-6 dB hin und her. Seit 0.7.1 ändert sich die Verstärkung auf diesem Weg um höchstens 1,2 dB pro Frame; in einer Simulation mit sprachähnlichen b2-Folgen bei -4 dB bleibt kein Sprung über 3 dB übrig (vorher 68), und laute Frames treffen das Ziel weiter. Größere Verschiebungen begrenzt das Format selbst, das grobe untere Ende der b2-Tabelle: die Aufteilung klein halten: Mit 0.7.1 klangen -4 und -6 über SvxLink nach DMR gleich und sauber (15.10.2026), URF363 läuft mit -6. DMR → DMR behält die unbegrenzte Nachführung, sie war im Test sauber. Seit 0.7.1 ist die Option nicht mehr experimentell, seit 0.7.2 ist -6 der Standard (vorher 0).

Eine b2-Auswahl mit Vorausschau wurde simuliert und weggelassen (15.10.2026): Eine Strahlsuche über die nächsten Frames mit quadratischen Kosten für Änderungen der Verstärkung beseitigt die Sprünge über 3 dB bei -6 (61 → 0) erst mit 6 Frames Vorausschau, etwa 100 ms, und kommt auf etwa -8 dB mit so vielen Sprüngen wie die einfache Begrenzung bei -6; ohne Vorausschau verfehlt sie das Ziel um etwa 3 dB. Das sind 2 dB mehr Pegel für den Encoder gegen 60-100 ms mehr Verzögerung für jeden Frame des Pakets (tcd schickt ein Paket erst, wenn alle Codecs fertig sind) und mehr Code im Paketpfad, während -4 und -6 schon gleich und sauber klangen: Die meisten gezählten Sprünge liegen an Silbengrenzen und werden überdeckt. Die b2-Tabelle selbst lässt sich nicht verbessern, sie ist Teil von AMBE2+ und in jedem Decoder fest eingebaut.

So arbeitet `ambe`: Der Decoder rechnet γ = Δγ[b2] + 0,5·γ<sub>alt</sub>, und der mittlere log2-Pegel eines Frames ist γ, eine Einheit γ sind also 6 dB. Die Δγ-Tabelle ist bei kleinem b2 grob und bei großem fein, deshalb änderte eine feste Verschiebung von b2 (tcd bis 0.2.4) leise Signale um 3–16 dB pro Stufe und laute um etwa 1,2 dB. tcd verfolgt jetzt γ für den Original- und den veränderten Stream und wählt pro Frame das b2, das der gewünschten Verstärkung am nächsten kommt; der Rundungsfehler wird im nächsten Frame ausgeglichen. Gemessen mit einem AMBE2+-Software-Decoder bei -40, -30 und -16 dBFS Eingangspegel kommen -12 bis +6 dB auf 0,8 dB genau an. Die Auflösung pro Frame ist grob, und sehr leise Quellen lassen sich nicht weiter anheben, als b2 = 31 erlaubt.

### AGC

| Schlüssel | Vorlage | Standard im Code | Bedeutung |
|---|---|---|---|
| `AGC` | `true` | `false` | automatische Pegelregelung ein/aus |
| `AGCTarget` | `-16` | `-16` | Zielpegel in dBFS |
| `AGCAttack` | `50` | `50` | Zeitkonstante in ms für Anstiege des Sprachpegels bis 10 dB (größere Sprünge werden in 2–3 Frames abgefangen) |
| `AGCRelease` | `500` | `500` | Zeitkonstante in ms für das Sinken des Sprachpegels, nur auf deutlich stimmhafter Sprache weit über dem Rauschboden |
| `AGCMaxGainDown` | `24` | `24` | höchste Absenkung in dB |

Gate und Grenze für die Verstärkung gibt es in zwei Profilen: **AGC digital** für D-Star-, DMR/YSF/NXDN-, M17- und P25-Eingang, **AGC analog** für SVX- und USRP-Eingang.

| Schlüssel | Vorlage | Standard im Code | Bedeutung |
|---|---|---|---|
| `AGCDigitalNoiseGate` / `AGCAnalogNoiseGate` | `-55` / `-40` | `-55` / `-40` | Gate-Schwelle in dBFS: Frames darunter oder weniger als 9 dB über dem Rauschboden, den tcd pro Stream misst, gelten nicht als Sprache; 240 ms später schließt das Gate und blendet die Verstärkung auf die Untergrenze aus. Ein Sicherheitsnetz unter dem gemessenen Rauschboden |
| `AGCDigitalMaxGainUp` / `AGCAnalogMaxGainUp` | `30` / `12` | `30` / `12` | höchste Verstärkung in dB |
| `AGCDigitalGateFloor` / `AGCAnalogGateFloor` | `0` / `0` | `0` / `0` | Verstärkung bei geschlossenem Gate, in dB (0 = höchstens Faktor 1) |
| `AGCAnalogNoiseSuppress` | `-10` | `-10` | nur SVX und USRP: stärkste Absenkung des Rauschens durch den speexdsp-Präprozessor vor der AGC, in dB (-40 bis 0, 0 = aus) |

**Analoge Quellen (SVX, USRP)** liefern FM-Audio, dessen Pausen nicht still sind, sondern der Rauschboden des Empfängers, oft −45 bis −35 dBFS. Mit den Einstellungen für digitale Quellen schließt das Gate darauf nie, und die AGC hob das Rauschen zwischen den Worten um bis zu 30 dB an: Bei einem guten FM-Signal (Rauschen −45 dBFS, Sprache −28 dBFS) fiel der Abstand zwischen Sprache und Rauschen von 17 auf 11 dB. SVX und USRP laufen deshalb durch die AGC analog mit eigenem Gate, eigener Grenze für die Verstärkung und eigener Untergrenze bei geschlossenem Gate; Zielpegel, Attack, Release und `AGCMaxGainDown` sind gemeinsam. Mit den Standardwerten steigt der Abstand im selben Fall auf 25 dB, stärker verrauschte Signale werden nicht mehr verschlechtert (Simulation durch `AGC.h`). 20 Minuten an einem SvxLink-Knoten aufgezeichneter Verkehr des FM-Funknetzes bestätigen das: Über HF fiel der Abstand mit der AGC digital von 34 auf 19 dB und bleibt mit der AGC analog bei 32 dB, bei App- und RoIP-Clients bei 36 dB (vorher 19,7). Das Gate arbeitet auf dem Pegel nach dem `RxGain` von urfd; der sollte daher nahe 0 bleiben und die AGC den Pegel setzen lassen, ein stark negativer `RxGain` drückt leise FM-Sprache unter das Gate. Ein Squelch-Schwanz ist lauter als jedes Gate: `SQL_TAIL_ELIM` im Empfänger des SvxLink-Knotens entfernt ihn an der Quelle.

**Rauschunterdrückung** (`AGCAnalogNoiseSuppress`): Das Gate wirkt nur zwischen den Worten, das FM-Rauschen unter der Sprache bleibt. Die Vocoder bilden Sprache nach, allen voran AMBE, und machen aus diesem Rauschen auf DMR einen blubbernden, roboterhaften Klang. Darum läuft der Eingang von SVX und USRP zuerst durch den speexdsp-Präprozessor (die Rauschunterdrückung vieler VoIP-Telefone), mit den 160 Samples / 8 kHz von urfd, ohne zusätzliche Verzögerung. Er führt eine Rauschschätzung pro Modul, die nächste Aussendung desselben Knotens beginnt also schon sauber, und folgt einem anderen Knoten innerhalb von ein, zwei Sekunden. Seine eigene AGC, VAD und Enthallung sind aus, der Pegel bleibt Sache der AGC. Live im Monitor einstellbar, 0 schaltet sie ab. Die Standardwerte stammen aus QSOs über einen SvxLink-Knoten nach DMR (15.10.2026): Bei -15 dB, dazu dem Gate floor von -12 dB, wurden die Pausen totenstill, der Comfort Noise fehlte, und leise Silbenanfänge und -enden klangen abgeschnitten, weil speexdsp sie für Rauschen hält; -10 dB mit Gate floor 0 lässt ein leises, natürliches Grundrauschen und ganze Silben stehen, und der roboterhafte Klang bleibt weg. Stärkere Werte entfernen mehr Rauschen, schneiden aber Silben an und hinterlassen "musikalische" Artefakte.

Warum nicht die Rauschunterdrückung im AMBE-Chip: Die DVSI-Geräte haben eine (ab Werk an, `NS_ENABLE` in `ECMODE`), tcd schaltet sie aber beim Einrichten der Kanäle ab (`ECMODE = 0`). Sie sitzt im Encoder-Kanal und würde auf alles wirken, was dieser Kanal kodiert, also auch auf Ton aus D-Star, M17 und P25, der schon sauber ist, und sie kann eine FM-Quelle nicht von den anderen unterscheiden; außerdem wird sie pro Kanal beim Start gesetzt, nicht pro Stream. speexdsp arbeitet dort, wo der analoge Eingang hereinkommt, nur bei SVX und USRP, mit eigener Einstellung, und gleich für jeden Ziel-Codec (DMR, D-Star, M17, P25).

### Überwachung und Protokoll

| Schlüssel | Vorlage | Bedeutung |
|---|---|---|
| `Monitor` | `true` | Web-Dashboard und Statistik-Socket ein/aus |
| `MonitorBindAddress` | `127.0.0.1` | Adresse von Web-Dashboard und Statistik-Socket; `0.0.0.0` öffnet sie ins Netz (ohne Passwort, die Seite kann Pegel ändern und speichern, also nur im vertrauenswürdigen LAN) |
| `MonitorPassword` | (keins) | die Seite fragt danach und hält eine Sitzung per Cookie 30 Tage (nach 5 Fehlversuchen von einer Adresse 60 s Pause für Anmeldungen von dort); `tcdmon` von anderen Rechnern als 127.0.0.1 ebenfalls (es fragt danach oder nimmt `TCD_MONITOR_PASSWORD`; `tcdmon -p <Passwort> <Host>` geht auch, zeigt das Passwort aber in der Prozessliste). Fehlversuche, die älter als 10 Minuten sind, zählen nicht. Unverschlüsseltes HTTP: von außerhalb des LANs über einen TLS-Proxy oder einen SSH-Tunnel |
| `MonitorHttpPort` | `8080` | Web-Dashboard |
| `MonitorStatsPort` | `8081` | Statistik-Socket (JSON, für `tcdmon`) |
| `StatsLog` | `false` | CSV-Dateien pro Stream zum Einstellen der AGC |
| `StatsLogDir` | `/tmp/tcd-stats` | Ablageort |
| `StatsLogRetain` | `24` | Stunden, bis sie gelöscht werden |

## Pegel

Jede Umwandlung läuft über PCM:

```
D-Star/DMR (DVSI hardware):
  AMBE --[DVSI GainIn]--> PCM --[AGC]--> PCM --[OutputGain]--> [DVSI GainOut] --> target AMBE

M17/P25 (software vocoder):
  codec --[decode]--> PCM --[AGC]--> PCM --[OutputGain]--> target codec

USRP/SVX (PCM):
  PCM --[Rauschunterdrückung]--> PCM --[analoge AGC]--> PCM --[OutputGain]--> target codec   (RxGain/TxGain pro Anbindung in urfd.ini)

DMR -> DMR/YSF with DmrGainMode = reencode:
  AMBE2+ --[decode]--> PCM --[AGC]--> PCM --[OutputGainDMR]--> [DmrReencodeGain] --> Codec-Plugin --> AMBE2+

DMR -> DMR/YSF with DmrGainMode = ambe:
  AMBE2+ --[decode]--> PCM --[AGC] (gain only)
  AMBE2+ --[b2: AGC gain + AmbeGainDb + OutputGainDMR]--> AMBE2+
```

**Mit AGC** können die festen Verstärkungen meist alle auf 0 bleiben, bis auf `OutputGainDMR = -14` für den Pegel, den DMR-Funkgeräte erwarten: Sie gleicht unterschiedliche Mikrofone, Funkgeräte und Codecs pro Aussendung aus. Ohne sie braucht jeder Weg seine eigene feste Verstärkung.

Wo die AGC wirkt: Sie misst immer am dekodierten PCM des eingehenden Streams. Zwischen verschiedenen Codecs wird sie auf dieses PCM angewendet, bevor kodiert wird. DMR → DMR/YSF bekommt sie im AMBE2+-Bitstrom über b2 (`DmrGainMode = ambe`) oder durch Neukodieren (`reencode`). D-Star → D-Star, M17 → M17 und P25 → P25 laufen unverändert durch.

Die AGC arbeitet pro Stream (pro Aussendung):
- ein Hochpass bei 75 Hz (3. Ordnung, 18 dB/Oktave) auf dem Audio, damit Gleichanteil und Brumm weder den Pegel bestimmen noch in den Encoder gelangen
- der Sprachpegel in dB aus der lauteren 10-ms-Hälfte jedes Frames, über das ganze Band: Er folgt der oberen Hüllkurve der Sprache, nahe an den Vokalen
- Rauschboden, Gate und Stimmerkennung laufen auf einer bei 300 Hz hochpassgefilterten Kopie, dem Band, das die Funkgeräte übertragen; ein CTCSS-Ton oder Brumm darunter zählt weder als Rauschen noch als Stimme
- ein Anstieg um mehr als 10 dB wird um die halbe Differenz pro Frame abgefangen, kleinere Anstiege mit `AGCAttack`; der Pegel sinkt (die Verstärkung steigt) nur auf deutlich stimmhaften Frames (Autokorrelation im Tonhöhenbereich 80–400 Hz) weit über dem Rauschboden, und um höchstens 10 dB pro `AGCRelease`. Atem, Rauschen, Zischlaute und Pausen halten die Verstärkung auf dem Wert des Sprechers, sie pumpt also nicht
- die ersten stimmhaften Frames eines Streams setzen seinen Pegel schnell, damit die ersten Worte eines leisen Sprechers nicht zu leise kommen
- ein Rauschboden pro Stream: Er folgt dem Pegel auf Frames ohne Sprache, und das Minimum der letzten 2,4 s hebt ihn an, wenn das Rauschen gestiegen ist. Die Anhebung wird mit (Sprachpegel − Rauschboden) / 20 dB skaliert, höchstens 1: Saubere Quellen behalten die volle Verstärkung, schwache, verrauschte Signale werden nicht in ihr Rauschen gehoben
- ein Noise-Gate: Frames unter `AGCDigitalNoiseGate` / `AGCAnalogNoiseGate` oder weniger als 9 dB über dem Rauschboden sind keine Sprache; 240 ms später, das überbrückt die Lücken zwischen den Worten, blendet die Verstärkung um 6 dB pro Frame auf höchstens die Untergrenze aus (`AGCDigitalGateFloor` / `AGCAnalogGateFloor`), damit Wortenden erhalten bleiben und Rauschen nicht angehoben wird
- die Verstärkung läuft als Rampe Sample für Sample durch jeden Frame, statt alle 20 ms zu springen
- ein Begrenzer knapp unter Vollaussteuerung, der die angewendete Verstärkung sofort kappt, es übersteuert also nie

Die Grenzen sind absichtlich ungleich: Absenken ist unkritisch, Anheben hebt auch das Rauschen. Typisches DMR-Audio liegt um -35 dBFS, für ein Ziel von -16 dBFS braucht es also etwa +20 dB.

## Überwachung

### Web-Dashboard

`http://localhost:8080` auf dem tcd-Rechner, von anderswo über `ssh -L 8080:localhost:8080 <tcd-host>`, oder `http://<tcd-host>:8080` mit `MonitorBindAddress = 0.0.0.0`:

- im Stil des urfd-Dashboards (seit 0.4.0): ein bernsteinfarbenes LCD pro Modul mit Eingangs-Codec, Zielen und AGC-Zustand, helles und dunkles Design; die Schrift Saira ist in die Seite eingebaut, sie lädt also nichts von außen
- LCD-Balkenanzeigen vor und nach der AGC, mit Spitzenwert und markiertem AGC-Ziel
- bei vielen Modulen eine kompakte Ansicht (ab fünf Modulen oder per „Compact“): eine Taste pro Modul mit Codec und Pegel, volle Karten nur für Module auf Sendung und für die, die per Klick offen gehalten werden; tcd merkt sich die Ansicht für alle Browser (`MonitorView`, schreibt „Speichern“), die angehefteten Module merkt sich der Browser
- Signalweg des aktiven Streams mit dem Pegel an jeder Stufe
- AGC- und Verstärkungsregler, die sofort wirken, und ein Knopf, um sie in `tcd.ini` zu speichern
- DVSI-Geräte: Seriennummer, Typ, Rolle, belegte Kanäle mit ihren Modulen, Puffertiefe
- Codec-Plugins: ihr Codec, Zähler und Streams, und welcher Vocoder neue D-Star-, DMR- und P25-Streams bekommt (umschaltbar, siehe [Codec-Plugins](#codec-plugins))
- Verbindung zum Reflektor und Paketzähler; ein Standby-Modul zeigt „standby“, solange ein anderer tcd es hat, und „taken over“, solange dieser tcd es hat
- hinter einem Reverse-Proxy unter einem Pfad (seit 0.11.0): Die Seite nutzt relative Pfade, `https://<host>/tcd/` funktioniert also (siehe [tcd in Docker](#tcd-in-docker)); hinter einem Proxy auf demselben Rechner zählt die Pause nach Fehlanmeldungen je Adresse aus dem letzten `X-Forwarded-For`-Eintrag, und das Session-Cookie ist `Secure`, wenn der Proxy `X-Forwarded-Proto: https` schickt; tcdmon zeigt Standby-Module ebenfalls

### tcdmon

Monitor im Terminal, praktisch über SSH:

```bash
tcdmon                      # this machine, port 8081
tcdmon 192.0.2.20 8081      # another machine
```

Zeigt Pegelbalken, Signalweg, AGC-Zustand, die DVSI-Kanäle mit ihren Modulen, die Codec-Plugins und die Ausgangsverstärkungen. `q` beendet.

### Statistik-CSV und agc-analyze

Mit `StatsLog = true` schreibt tcd pro Stream eine CSV-Datei nach `StatsLogDir` (z.B. `F_20260402_003512_43521.csv`, Spalten `ms,module,codec,rms_in,peak_in,rms_out,peak_out,agc_gain,gate`). `agc-analyze` wertet sie aus:

```bash
agc-analyze               # details, default directory /tmp/tcd-stats
agc-analyze -s            # one line per stream
agc-analyze -s -c dmr     # by codec: dstar, dmr, p25, usrp, svx
agc-analyze -s -m F       # by module
agc-analyze file.csv      # one file
```

Es zeigt pro Stream Dauer, Sprach- und Gate-Frames, Ein- und Ausgangspegel, den Bereich der AGC-Verstärkung und den Anteil an Übersteuerung.

## Betrieb und Fehlersuche

```bash
sudo systemctl status tcd
sudo systemctl restart tcd
journalctl -u tcd -f
```

| Meldung / Problem | Prüfen |
|---|---|
| `Could not find an AMBE device!` | `tcd --list-devices`; ist `DeviceSerial` gesetzt, muss es exakt passen (siehe [oben](#ambe-geräte-auswählen)) |
| `No USB serial devices found` | kein Gerät eingesteckt, oder der Treiber `ftdi_sio` fehlt (`lsmod \| grep ftdi_sio`) |
| Warnung `USB latency timer is 16 ms` | tcd konnte den Latenz-Timer nicht setzen; nativ muss er als root laufen (die systemd-Unit tut das); in Docker hat der Kernel-Treiber `ftdi_sio` die Low-Latency-Anforderung ignoriert, dann auf dem Host `/sys/bus/usb-serial/devices/<tty>/latency_timer` auf 1 setzen |
| `... does not answer at ... baud and is left out` | das Gerät wird weggelassen, tcd läuft mit den übrigen; ab- und wieder einstecken, dann tcd neu starten. Ein DV3003 mit generischem USB-Namen bekommt 460800 Baud und kann nicht antworten |
| `... is in use by another process` | ein anderer tcd oder `dvsitest` hält das Gerät: ihn beenden (`pgrep -a tcd`) |
| `Lost the AMBE device ..., shutting down` | das Gerät wurde abgezogen oder zurückgesetzt; systemd startet tcd nach 5 s neu, und er findet das Gerät wieder |
| der Reflektor zeigt keinen Transcoder | `ServerAddress` und `Port` in `tcd.ini`, auf beiden Seiten dieselben `Modules`, und Port 10100/tcp vom tcd-Rechner aus erreichbar |

## Änderungen gegenüber dem Original

### Hardware und Geräte
- **Serieller Zugriff ohne libftd2xx**: über den Kernel-Treiber `ftdi_sio` und `/dev/serial/by-id` (POSIX-Anbindung von Jason Kendall), RTS/CTS-Flusskontrolle, USB-Latenz-Timer auf 1 ms; von einem älteren tcd gelöste Geräte werden wieder gebunden; ein verlorenes Gerät startet tcd über systemd neu
- **Geräteauswahl**: `--list-devices`, `DeviceSerial` mit exaktem Vergleich
- **Gerätetyp vom Chip**: DV3000 oder DV3003 nach der Produkt-ID, ein Gerät ohne Antwort wird weggelassen, statt tcd zu beenden, ein zweiter Reset-Versuch, die Null-Bytes zum Leeren gehen hinaus, bevor die Eingabe verworfen wird, exklusives Öffnen
- **macOS**: baut mit clang und läuft nativ mit den AMBE-Geräten, siehe [macOS](#macos)
- **Mischbetrieb DV3000 + DV3003**: zwei Module gleichzeitig mit Codec-Einstellung pro Kanal
- **D-Star auf dem DV3003**: Companding aus, seriellen Puffer leeren, Kodierung pro Kanal
- **SVX-Codec-Pfad**: eigener `ECodecType::svx` für SVX-Audio

### Audio
- **Ausgangsverstärkung pro Codec**: nach der AGC, für D-Star, M17/Codec2, DMR/YSF/NXDN, P25 und USRP, angewendet auf Kopien (der gemeinsame Puffer wird nie verändert)
- **AMBE2+-Bitstrom-Verstärkung**: `AmbeGainDb` ändert den Parameter b2 im AMBE2+-Frame, mit neu berechnetem Golay(24,12)-FEC für die A-Partition; die B-Partition wird entwürfelt und mit den neuen A-Daten wieder verwürfelt (0.2.3; vorher verlor der Decoder die B-Partition); seit 0.2.5 von der AGC gesteuert und im γ-Bereich gerechnet, die Verstärkung hängt also nicht mehr vom Pegel ab, auch mit nur einem Gerät und einem Codec-Plugin
- **AGC v3**: gleitendes RMS-Fenster, Verfolgung pro Sprecher, Gate mit Rücklauf, schnelles Anheben nach dem Gate, ungleiche Grenzen, Noise-Gate, Begrenzer, der die angewendete Verstärkung sofort kappt (0.2.3; vorher clippten laute Einsätze nach leiser Sprache), Änderungen live aus dem Dashboard; seit 0.2.3 threadsicher, da die Dekodier-Threads aller Codecs sie nutzen. Seit 0.2.4 hält das Gate 60 ms nach und blendet aus statt umzuschalten (Wortenden werden nicht mehr abgehackt), Rauschen knapp darüber pumpt die Verstärkung nicht mehr hoch, und die Verstärkung läuft als Rampe durch jeden Frame statt alle 20 ms zu springen
- **AGC digital und AGC analog** (0.3.0): SVX- und USRP-Eingang haben ein eigenes Gate, eine eigene Grenze für die Verstärkung und eine Verstärkung bei geschlossenem Gate (`AGCAnalogNoiseGate`, `AGCAnalogMaxGainUp`, `AGCAnalogGateFloor`), das FM-Rauschen zwischen den Worten wird also abgesenkt statt um bis zu 30 dB angehoben; die übrigen Eingänge nutzen dieselben drei Schlüssel als `AGCDigital…` (0.3.1 hat `AGCNoiseGate` und `AGCMaxGainUp` in `AGCDigitalNoiseGate` und `AGCDigitalMaxGainUp` umbenannt, `AGCDigitalGateFloor` ergänzt, und `agc-analyze` nennt den Schlüssel des Profils des Streams); seit 0.3.2 steht die Verstärkung von SVX und USRP pro Anbindung nur noch in der `urfd.ini` (`UsrpGainIn`/`UsrpGainOut` entfallen); ohne Monitor behält tcd die Werte aus der ini (vorher fiel er nach einer Sekunde auf die eingebauten Standardwerte zurück)
- **OutputGainDMRBitstream = -6 als Standard** (0.7.2): sauber bei -4 und -6 über SvxLink nach DMR, Ton aus D-Star, M17, P25, USRP und SVX kommt also standardmäßig 6 dB lauter beim AMBE2+-Encoder an; eine ini mit dem Schlüssel behält ihren Wert
- **Kein Knacken mit OutputGainDMRBitstream** (0.7.1): Die b2-Verstärkung kodierten Tons ändert sich um höchstens 1,2 dB pro Frame, leise Frames springen also nicht mehr zwischen zwei groben b2-Stufen hin und her
- **Version im Monitor** (0.7.0): Die Webseite zeigt die laufende tcd-Version in der Kopfleiste, wie das urfd-Dashboard, und tcdmon in seiner Kopfzeile; das Stats-JSON führt sie als `version`, alles aus einer Konstante in `Version.h`; `OutputGainDMRBitstream` wurde eine Aufteilung in dB
- **DMR-Pegel im Bitstrom für andere Codecs** (0.6.0, bis 0.7.1 experimentell): `OutputGainDMRBitstream` wendet `OutputGainDMR` bei Ton aus D-Star, M17, P25, USRP und SVX über b2 nach dem Encoder an statt davor; 0.6.0 hatte einen Schalter für alles, das hinterließ Artefakte auf leisen Silben, seit 0.7.0 ist es die Zahl der dB in b2 (-24 bis 0), live im Monitor; Standard -6 seit 0.7.2 (vorher 0)
- **Rauschunterdrückung für analoge Eingänge** (0.5.0): SVX und USRP laufen vor der AGC analog durch den speexdsp-Präprozessor (`AGCAnalogNoiseSuppress`, Standard -15 dB, live im Monitor; 0.5.1 hat sie auf -10 dB gesenkt und `AGCAnalogGateFloor` von -12 auf 0, die Rauschunterdrückung senkt die Pausen ab), das FM-Rauschen unter der Sprache wird auf DMR also nicht mehr zu roboterhaften AMBE-Artefakten; neue Build-Abhängigkeit `libspeexdsp-dev`
- **P25 nach M17**: ein Hochpass bei 300 Hz (4. Ordnung) vor dem Codec2-Encoder für P25-Ton; die zusätzliche Energie unter 120 Hz aus dem IMBE-Decoder brachte die Grundfrequenzschätzung von Codec2 durcheinander (PESQ etwa 1,6, jetzt 2,5)
- **DMR neu kodieren** (mit einem DMR-Codec-Plugin): `DmrGainMode = reencode` mit AGC und `DmrReencodeGain` für DMR → DMR/YSF
- **Codec-Plugins** (0.10.0): Software-Vocoder für D-Star und DMR/YSF aus Shared Libraries mit einer schlichten C-Schnittstelle, ein Vocoder-Zustand pro Stream; ersetzen die eingebaute MD380-Unterstützung. Der Vocoder für neue Streams lässt sich zur Laufzeit zwischen den Geräten und den Plugins umschalten; seit 0.12.0 (`TCD_CODEC_API 2`) erfährt ein Plugin Anfang und Ende einer Aussendung, Plugins für API 1 müssen neu gebaut werden
- **Ausgangsverstärkung im Mischbetrieb** (0.2.3): Der D-Star-Kanal des DV3003 nutzt `OutputGainDStar` statt `OutputGainDMR`; M17 mit 1600 bit/s schickt die zweite Hälfte jedes Pakets nicht mehr zweimal durch die AGC; seit 0.2.11 geht ein Modul, das nicht in der Modulliste steht, an keinen DMR-Kanal

### Überwachung und Konfiguration
- **Web-Dashboard**: eingebauter HTTP/WebSocket-Server, seit 0.4.0 im Stil des urfd-Dashboards, mit Signalweg, LCD-Pegelanzeigen, Verstärkungs- und AGC-Reglern, Geräte- und Codec-Plugin-Status, Vocoder-Wahl, Speichern in die INI
- **tcdmon**: Monitor im Terminal
- **Letzter Stream im Monitor** (0.7.3): ein freies Modul zeigt Codec und Alter seines letzten Streams (Sekunden, dann Minuten und Stunden), statt 2 s nach jedem Stream wieder auf "No stream since tcd started" zu fallen; das Stats-JSON liefert ihn als `last_codec`
- **Statistik-CSV** mit automatischem Aufräumen und dem Werkzeug `agc-analyze`; seit 0.2.9 meldet es Flackern des Gates als Öffnungen unter 100 ms (Rauschen über der Schwelle, das Noise-Gate des Profils anheben), da das Gate seit 0.2.4 vor dem Schließen nachhält
- **Umbenannte Schlüssel**: `DmrGainIn/Out` (war `DmrYsfGainIn/Out`), `UsrpGainIn/Out` (war `UsrpRxGain/TxGain`, seit 0.3.2 entfallen), Verstärkungen erweitert auf ±40 dB

### Zuverlässigkeit
- **Monitor standardmäßig nur lokal**: `MonitorBindAddress` (Standard `127.0.0.1`) für Web-Dashboard und Statistik-Socket; seit 0.10.7 fragen mit `MonitorPassword` die Seite und ein `tcdmon` von außen nach einem Passwort
- **Keine Grauzone im Kern** (0.10.0): Die MD380-Firmware ist aus tcd verschwunden; ohne Codec-Plugin sind zwei AMBE-Geräte nötig, und `ambe` deckt DMR → DMR/YSF ab
- **Leistung**: blockierendes Lesen mit Timeout statt Dauerabfrage; seit 0.2.12 mit `-O2` gebaut, ein transkodierter Stream (Codec2, IMBE und AGC in Software) braucht etwa 7 % eines Kerns auf einem Raspberry Pi 4 und 3 % auf einem Pi 5, ein Pi 4 reicht für zwei Module also locker
- **Threadsicherheit**: ein IMBE-Encoder und -Decoder pro Modul (ein gemeinsamer Zustand verstümmelte P25, wenn zwei Module gleichzeitig transcodierten), SIGPIPE ignoriert für saubere Neuverbindungen
- **Fehlerbehandlung**: Wiederholung zum Reflektor mit wachsender Pause, Verwerfen bei Überlauf der Warteschlange, Erholung bei Timeouts beim Lesen
- **Wachsende Pause beim Neuverbinden**: 0,5 s bis 5 s, solange der Reflektor nicht erreichbar ist
- **Standby-Module** (0.11.0, urfd 4.26.0): Mit `StandbyModules` übernimmt ein zweiter tcd Module, solange ihr tcd weg ist, siehe [Standby](#standby-ein-zweiter-tcd-für-dieselben-module)
- **Sprachgesteuerte AGC** (0.9.0): Der Pegel wird in dB auf der oberen Hüllkurve der Sprache verfolgt und nur auf deutlich stimmhaften Frames weit über einem pro Stream gemessenen Rauschboden angehoben; bei verrauschten Signalen fällt die Anhebung kleiner aus, ein Hochpass bei 75 Hz säubert das Audio, und die Analyse ignoriert alles unter 300 Hz (CTCSS, Brumm). Die Verstärkung pumpt nicht mehr bei Atem, Rauschen oder in Pausen, das Gate hält 240 ms über die Lücken zwischen den Worten. Gleiche Schlüssel und Standardwerte in `tcd.ini`. Gemessen an 18 FM-Funknetz-Aussendungen von HF-Stationen (23.10.2026) durch den analogen Pfad (Rauschunterdrückung und AGC analog) gegen 0.8.0: Die Stationen liegen gleich nah beieinander (Streuung 1,2 dB) bei etwa gleichem Pegel (−16,4 gegen −15,6 dBFS), die Verstärkung bewegt sich von Frame zu Frame nur ein Drittel so stark (0,20 gegen 0,55 dB), und die Sprache steht 2,5 dB weiter über dem Rauschen in den Pausen
- **Passwort für die Verbindung zum Reflektor** (0.8.0, urfd 4.16.0): mit `Password` in `tcd.ini` und `[Transcoder] Password` in `urfd.ini` schickt der Reflektor nach dem Modul-Byte eine Zufallszahl, und tcd antwortet mit HMAC-SHA256 darüber; erst dann zählt die Verbindung. Sie bekommt ein Modul nur, wenn es keine lebende Verbindung (oder eine Standby-Verbindung) hat: ein zweiter tcd für dieselben Module wird abgewiesen, statt dass die beiden sich das Modul immer wieder gegenseitig abnehmen, und ein verschwundener tcd bekommt sein Modul zurück, sobald der Reflektor die alte Verbindung als tot erkannt hat (sofort, wenn sie geschlossen wurde, sonst nach etwa 25 s TCP-Keepalive). Ohne Passwort konnte jeder, der Port 10100 erreichte, ein Modul übernehmen. Die Verbindung bleibt unverschlüsselt. Ohne Passwort auf beiden Seiten ändert sich nichts; steht es nur auf einer Seite, wird die Verbindung mit einer Meldung dazu abgewiesen
- **Hostname als ServerAddress** (0.7.4): `ServerAddress` nimmt neben einer IP-Adresse auch einen Hostnamen; vorher hielt die Konfig-Prüfung tcd bei allem außer einer IP-Adresse an
- **Keine Nagle-Verzögerung**: `TCP_NODELAY` auf der Verbindung zum Reflektor; ein Frame ist nach etwa 23 ms statt 60 ms zurück
- **Web-Dashboard auf libwebsockets** (0.10.0): Der HTTP- und WebSocket-Server ist libwebsockets (MIT, wie in urfd) statt des mitgelieferten mongoose, dessen Lizenz GPL-2.0-only nicht zur GPLv3 von tcd passt; gleiche Seite, gleiche `/api/...` und `/ws`
- **Build**: Teil des urfd-Repositorys, `build.sh` baut als Eigentümer des Klons; seit 0.10.0 gehört der IMBE-Vocoder zum Quelltext (`imbe/`), es wird kein weiteres Repository geholt

## Copyright

- Copyright (c) 2021-2023 Thomas A. Early N7TAE, Doug McLain AD8DP
- POSIX-Anbindung: Jason Kendall (2026)
- Copyright (c) 2026 Jens-Christian Merg DL4JC (Verbesserungen von urfd-ng)
