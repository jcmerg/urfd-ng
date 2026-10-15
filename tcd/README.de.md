# tcd — Hybrider Transcoder für digitale Sprache

[English](README.md) | **Deutsch**

**Version 0.5.1** — der Transcoder von [urfd-ng](../README.de.md), im Ordner `tcd/` desselben Repositorys. Basiert auf [n7tae/tcd](https://github.com/n7tae/tcd).

tcd wandelt Sprache zwischen den digitalen Betriebsarten eines URF-Reflektors um, damit ein D-Star-Nutzer einen DMR-Nutzer hört und umgekehrt. Für D-Star (AMBE) und DMR/YSF (AMBE2+) nutzt er DVSI-AMBE-Hardware, für M17 (Codec2), P25/NXDN (IMBE) und optional DMR/YSF (MD380) Software-Vocoder. Der Reflektor gibt das Audio seiner transcodierten Module über TCP an tcd und bekommt alle anderen Betriebsarten zurück.

## Inhalt

- [Installation](#installation)
- [Hardware](#hardware)
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
sudo bash tcd/build.sh               # two or more AMBE devices
sudo bash tcd/build.sh --with-md380  # a single DV3000, DMR/YSF in software
```

`build.sh` installiert die Build-Werkzeuge und die Vocoder-Bibliotheken ([imbe_vocoder](https://github.com/jcmerg/imbe_vocoder) und mit `--with-md380` [md380_vocoder](https://github.com/jcmerg/md380_vocoder): nativ auf armhf, über [dynarmic](https://github.com/jcmerg/md380_vocoder_dynarmic) auf x86_64 und arm64), baut tcd als Eigentümer des Klons und installiert `tcd`, `tcdmon`, `agc-analyze` und die systemd-Unit. Ein laufender tcd wird neu gestartet. Bei der ersten Installation landet die Vorlage unter `/usr/local/etc/tcd.ini`; die bitte anpassen (siehe [Konfiguration](#konfiguration-tcdini)) und mit `sudo systemctl start tcd` starten.

Aktualisieren: im Klon `git pull` und `build.sh` erneut aufrufen, oder `sudo ./setup.sh update`.

### Von Hand

Mit installierten Bibliotheken:

```bash
cd urfd/tcd
make -j$(nproc)             # without MD380
make -j$(nproc) md380=true  # with the MD380 software vocoder
sudo make install
```

tcd baut gegen die urfd-Quellen, die er mit dem Reflektor teilt (`TCPacketDef.h`, `TCSocket.*`, ...), aus `../reflector`; beide Seiten sprechen also immer dasselbe Protokoll. `config/tcd.ini` ist die kommentierte Vorlage, `config/tcd.service` die systemd-Unit.

### Voraussetzungen

- Linux mit systemd: Debian, Ubuntu oder Raspberry Pi OS; x86_64, arm64 oder armhf
- DVSI-AMBE-USB-Geräte (siehe [Hardware](#hardware)); sie laufen über den Kernel-Treiber `ftdi_sio`, ein zusätzlicher Treiber ist nicht nötig
- `libspeexdsp-dev` für die Rauschunterdrückung analoger Eingänge; `build.sh` installiert es
- Geringe Latenz zu den AMBE-Geräten: tcd läuft direkt auf dem System, nicht in Docker. Er kann auf dem Rechner des Reflektors laufen oder auf einem anderen, z.B. einem Raspberry Pi neben der Funktechnik.

## Hardware

Jedes transcodierte Modul braucht einen D-Star-Kanal und einen DMR/YSF-Kanal. Ein DV3000 (ThumbDV, DVstick-30, USB-3000) hat einen Kanal, ein DV3003 (DVstick-33, USB-3003) drei. Fehlt AMBE-Hardware für DMR/YSF, übernimmt der Software-Vocoder MD380 diesen Teil (bauen mit `md380=true` / `--with-md380`).

| Hardware | Build | Module | Hinweis |
|---|---|---|---|
| 1× DV3000 oder 1× DV3003 | MD380 nötig | 1 | D-Star auf dem Gerät, DMR/YSF in Software |
| DV3000 + DV3003 | ohne MD380 | 2 | Mischbetrieb, empfohlen |
| 2× DV3000 | ohne MD380 | 1 | eines für D-Star, eines für DMR/YSF |
| 2× DV3003 | ohne MD380 | bis zu 3 | eines für D-Star, eines für DMR/YSF |

Ein einzelnes Gerät braucht immer den MD380-Build; ohne ihn startet tcd nicht. Bei zwei Geräten ist der MD380 nicht nötig: Seit 0.2.5 bekommt DMR → DMR/YSF mit `DmrGainMode = ambe` dieselbe AGC ohne Neukodieren (siehe [`DmrGainMode`](#pegel-dmrysf--dmrysf-dmrgainmode)). Ohne MD380 lässt sich tcd auf jeder Plattform bauen und betreiben (x86_64, arm64, armhf), ohne ARM-Code zu emulieren; der MD380-Build bringt nur `reencode` als Alternative mit. tcd erkennt die Geräte und wählt den Betrieb selbst.

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

### Einzelnes Gerät mit MD380

Ein Modul, D-Star auf dem DV3000 oder DV3003, DMR/YSF über den MD380. In diesem Betrieb wird DMR/YSF → DMR/YSF nach der AGC **nicht neu kodiert**, weil der MD380 nicht im selben Ablauf dekodieren und neu kodieren kann (gemeinsamer Zustand von Kodierer und Dekodierer). Für diesen Weg die Bitstrom-Verstärkung (`DmrGainMode = ambe`, folgt der AGC; in diesem Betrieb seit 0.2.5) verwenden; alle Wege zwischen verschiedenen Betriebsarten bekommen weiterhin die volle AGC. Neukodieren bei DMR/YSF → DMR/YSF braucht ein zweites AMBE-Gerät.

### Hinweise zum DV3003

- **Companding aus**: Bei jedem DV3003-Kanal wird Companding ausdrücklich abgeschaltet. Der DV3003 startet mit eingeschaltetem Companding (Pull-up an CP_ENABLE), der DV3000 mit ausgeschaltetem; ohne die Abschaltung bekamen die DMR/YSF-Kanäle µ-law-PCM, das als linear gelesen wurde, und das Audio war verzerrt.
- **Seriellen Puffer leeren**: Vor dem Soft-Reset leeren 350 Null-Bytes einen hängengebliebenen seriellen Zustand.
- **Codec pro Kanal**: Ein DV3003 kann gleichzeitig D-Star- und DMR/YSF-Kanäle betreiben (Mischbetrieb).

## AMBE-Geräte auswählen

```bash
tcd --list-devices
```

listet die seriellen USB-Geräte aus `/dev/serial/by-id` mit ihren Seriennummern und markiert die, die nach AMBE-Geräten aussehen.

Ohne `DeviceSerial` in `tcd.ini` verwendet tcd jedes Gerät, das er am Namen als AMBE erkennt. Hängen weitere serielle USB-Geräte am Rechner (GPS, CAT-Kabel, ...), die AMBE-Geräte ausdrücklich eintragen. `DeviceSerial` muss exakt einem davon entsprechen:

- der USB-Seriennummer (`DKB7FXGE`)
- der älteren libftd2xx-Form mit dem Port-Buchstaben von Mehrport-Geräten (`DA1234A`)
- dem Namen unter `/dev/serial/by-id`
- der Gerätedatei (`/dev/ttyUSB0`)

tcd öffnet die Geräte über den Kernel-Treiber `ftdi_sio` und setzt ihren USB-Latenz-Timer auf 1 ms (der Standard des Treibers ist 16 ms, zu langsam für das AMBE-Timing; das Log zeigt den Wert). Geräte, die ein älterer, auf libftd2xx basierender tcd vom `ftdi_sio`-Treiber gelöst hat, werden beim Start wieder gebunden; ein Umstieg braucht also kein erneutes Einstecken.

## Konfiguration (tcd.ini)

`/usr/local/etc/tcd.ini`; die Vorlage in [`config/tcd.ini`](config/tcd.ini) erklärt jeden Schlüssel. Nach Änderungen tcd neu starten (`sudo systemctl restart tcd`), oder die Audio-Einstellungen live im [Web-Dashboard](#web-dashboard) ändern.

### Verbindung und Geräte

| Schlüssel | Vorlage | Bedeutung |
|---|---|---|
| `ServerAddress` | `127.0.0.1` | Adresse des urfd-Reflektors |
| `Port` | `10100` | `[Transcoder] Port` des Reflektors |
| `Modules` | `FS` | transcodierte Module, dieselben Buchstaben wie `[Transcoder] Modules` im Reflektor |
| `DeviceSerial` | — | zu verwendendes AMBE-Gerät, je eine Zeile (siehe [oben](#ambe-geräte-auswählen)) |

### Verstärkung

Alle Werte in dB, -40 bis +40.

| Schlüssel | Vorlage | Wirkt auf |
|---|---|---|
| `OutputGainDStar` | `0` | D-Star-Ausgang, nach der AGC |
| `OutputGainDMR` | `-18` | DMR/YSF-Ausgang, nach der AGC; jeder Weg nach DMR/YSF, auch DMR → DMR/YSF |
| `OutputGainM17` | `0` | M17/Codec2-Ausgang, nach der AGC |
| `OutputGainIMBE` | `0` | P25/NXDN-Ausgang, nach der AGC |
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
| `reencode` | MD380 | vollständiges Dekodieren und Neukodieren über den MD380, mit AGC und `DmrReencodeGain`; eine Alternative zu `ambe`, gleicher Pegel bei gleichem Wert. Ohne MD380 Rückfall auf `ambe`. |

| Schlüssel | Vorlage | Standard im Code |
|---|---|---|
| `DmrGainMode` | `ambe` | `ambe` |
| `AmbeGainDb` | `0` | `0` |
| `DmrReencodeGain` | `0` | — |

DMR-Funkgeräte erwarten einen niedrigeren Pegel als das AGC-Ziel (`AGCTarget`, -16 dBFS). `OutputGainDMR = -18` senkt jeden Weg nach DMR/YSF darauf ab: den DVSI-Encoder für Quellen aus D-Star, M17, P25 und USRP sowie b2 oder das Neukodieren für DMR-Quellen. `AmbeGainDb` und `DmrReencodeGain` verschieben nur DMR → DMR/YSF gegenüber den anderen Wegen und bleiben normalerweise 0. (tcd 0.2.6 legte die -18 in `AmbeGainDb`/`DmrReencodeGain`, D-Star → DMR war dadurch 18 dB zu laut.)

So arbeitet `ambe`: Der Decoder rechnet γ = Δγ[b2] + 0,5·γ<sub>alt</sub>, und der mittlere log2-Pegel eines Frames ist γ, eine Einheit γ sind also 6 dB. Die Δγ-Tabelle ist bei kleinem b2 grob und bei großem fein, deshalb änderte eine feste Verschiebung von b2 (tcd bis 0.2.4) leise Signale um 3–16 dB pro Stufe und laute um etwa 1,2 dB. tcd verfolgt jetzt γ für den Original- und den veränderten Stream und wählt pro Frame das b2, das der gewünschten Verstärkung am nächsten kommt; der Rundungsfehler wird im nächsten Frame ausgeglichen. Gemessen mit dem MD380-Decoder bei -40, -30 und -16 dBFS Eingangspegel kommen -12 bis +6 dB auf 0,8 dB genau an. Die Auflösung pro Frame ist grob, und sehr leise Quellen lassen sich nicht weiter anheben, als b2 = 31 erlaubt.

### AGC

| Schlüssel | Vorlage | Standard im Code | Bedeutung |
|---|---|---|---|
| `AGC` | `true` | `false` | automatische Pegelregelung ein/aus |
| `AGCTarget` | `-16` | `-16` | Zielpegel in dBFS |
| `AGCAttack` | `50` | `50` | ms, um lautes Audio abzusenken |
| `AGCRelease` | `500` | `500` | ms, um leises Audio anzuheben |
| `AGCMaxGainDown` | `24` | `24` | höchste Absenkung in dB |

Gate und Grenze für die Verstärkung gibt es in zwei Profilen: **AGC digital** für D-Star-, DMR/YSF-, M17- und P25/NXDN-Eingang, **AGC analog** für SVX- und USRP-Eingang.

| Schlüssel | Vorlage | Standard im Code | Bedeutung |
|---|---|---|---|
| `AGCDigitalNoiseGate` / `AGCAnalogNoiseGate` | `-55` / `-40` | `-55` / `-40` | Gate-Schwelle in dBFS: Das Gate schließt 60 ms, nachdem der Pegel darunter gefallen ist, und blendet die Verstärkung auf die Untergrenze aus; bis 6 dB darüber wird nicht hochgeregelt. Über den Rauschboden der Quellen legen |
| `AGCDigitalMaxGainUp` / `AGCAnalogMaxGainUp` | `30` / `12` | `30` / `12` | höchste Verstärkung in dB |
| `AGCDigitalGateFloor` / `AGCAnalogGateFloor` | `0` / `0` | `0` / `0` | Verstärkung bei geschlossenem Gate, in dB (0 = höchstens Faktor 1) |
| `AGCAnalogNoiseSuppress` | `-10` | `-10` | nur SVX und USRP: stärkste Absenkung des Rauschens durch den speexdsp-Präprozessor vor der AGC, in dB (-40 bis 0, 0 = aus) |

**Analoge Quellen (SVX, USRP)** liefern FM-Audio, dessen Pausen nicht still sind, sondern der Rauschboden des Empfängers, oft −45 bis −35 dBFS. Mit den Einstellungen für digitale Quellen schließt das Gate darauf nie, und die AGC hob das Rauschen zwischen den Worten um bis zu 30 dB an: Bei einem guten FM-Signal (Rauschen −45 dBFS, Sprache −28 dBFS) fiel der Abstand zwischen Sprache und Rauschen von 17 auf 11 dB. SVX und USRP laufen deshalb durch die AGC analog mit eigenem Gate, eigener Grenze für die Verstärkung und eigener Untergrenze bei geschlossenem Gate; Zielpegel, Attack, Release und `AGCMaxGainDown` sind gemeinsam. Mit den Standardwerten steigt der Abstand im selben Fall auf 25 dB, stärker verrauschte Signale werden nicht mehr verschlechtert (Simulation durch `AGC.h`). 20 Minuten an einem SvxLink-Knoten aufgezeichneter Verkehr des FM-Funknetzes bestätigen das: Über HF fiel der Abstand mit der AGC digital von 34 auf 19 dB und bleibt mit der AGC analog bei 32 dB, bei App- und RoIP-Clients bei 36 dB (vorher 19,7). Das Gate arbeitet auf dem Pegel nach dem `RxGain` von urfd; der sollte daher nahe 0 bleiben und die AGC den Pegel setzen lassen, ein stark negativer `RxGain` drückt leise FM-Sprache unter das Gate. Ein Squelch-Schwanz ist lauter als jedes Gate: `SQL_TAIL_ELIM` im Empfänger des SvxLink-Knotens entfernt ihn an der Quelle.

**Rauschunterdrückung** (`AGCAnalogNoiseSuppress`): Das Gate wirkt nur zwischen den Worten, das FM-Rauschen unter der Sprache bleibt. Die Vocoder bilden Sprache nach, allen voran AMBE, und machen aus diesem Rauschen auf DMR einen blubbernden, roboterhaften Klang. Darum läuft der Eingang von SVX und USRP zuerst durch den speexdsp-Präprozessor (die Rauschunterdrückung vieler VoIP-Telefone), mit den 160 Samples / 8 kHz von urfd, ohne zusätzliche Verzögerung. Er führt eine Rauschschätzung pro Modul, die nächste Aussendung desselben Knotens beginnt also schon sauber, und folgt einem anderen Knoten innerhalb von ein, zwei Sekunden. Seine eigene AGC, VAD und Enthallung sind aus, der Pegel bleibt Sache der AGC. Live im Monitor einstellbar, 0 schaltet sie ab. Die Standardwerte stammen aus QSOs über einen SvxLink-Knoten nach DMR (15.10.2026): Bei -15 dB, dazu dem Gate floor von -12 dB, wurden die Pausen totenstill, der Comfort Noise fehlte, und leise Silbenanfänge und -enden klangen abgeschnitten, weil speexdsp sie für Rauschen hält; -10 dB mit Gate floor 0 lässt ein leises, natürliches Grundrauschen und ganze Silben stehen, und der roboterhafte Klang bleibt weg. Stärkere Werte entfernen mehr Rauschen, schneiden aber Silben an und hinterlassen "musikalische" Artefakte.

### Überwachung und Protokoll

| Schlüssel | Vorlage | Bedeutung |
|---|---|---|
| `Monitor` | `true` | Web-Dashboard und Statistik-Socket ein/aus |
| `MonitorBindAddress` | `127.0.0.1` | Adresse von Web-Dashboard und Statistik-Socket; `0.0.0.0` öffnet sie ins Netz (ohne Passwort, die Seite kann Pegel ändern und speichern, also nur im vertrauenswürdigen LAN) |
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
  AMBE2+ --[decode]--> PCM --[AGC]--> PCM --[OutputGainDMR]--> [DmrReencodeGain] --> MD380 --> AMBE2+

DMR -> DMR/YSF with DmrGainMode = ambe:
  AMBE2+ --[decode]--> PCM --[AGC] (gain only)
  AMBE2+ --[b2: AGC gain + AmbeGainDb + OutputGainDMR]--> AMBE2+
```

**Mit AGC** können die festen Verstärkungen meist alle auf 0 bleiben, bis auf `OutputGainDMR = -18` für den Pegel, den DMR-Funkgeräte erwarten: Sie gleicht unterschiedliche Mikrofone, Funkgeräte und Codecs pro Aussendung aus. Ohne sie braucht jeder Weg seine eigene feste Verstärkung.

Wo die AGC wirkt: Sie misst immer am dekodierten PCM des eingehenden Streams. Zwischen verschiedenen Codecs wird sie auf dieses PCM angewendet, bevor kodiert wird. DMR → DMR/YSF bekommt sie im AMBE2+-Bitstrom über b2 (`DmrGainMode = ambe`) oder durch Neukodieren (`reencode`). D-Star → D-Star, M17 → M17 und P25 → P25 laufen unverändert durch.

Die AGC arbeitet pro Stream (pro Aussendung):
- RMS über ein gleitendes Fenster von 3 Frames (60 ms), folgt also Silben statt einzelnen Lauten
- ein langsamer Mittelwert (etwa 2 s) der Verstärkung, die jeder Sprecher braucht; in Pausen läuft die Verstärkung dorthin zurück, damit die erste Silbe nach einer Pause gleich richtig kommt
- schnelleres Anheben in den ersten 5 Frames nach dem Öffnen des Gates
- ein Noise-Gate bei `AGCDigitalNoiseGate` / `AGCAnalogNoiseGate`: Es schließt 60 ms, nachdem der Pegel darunter gefallen ist, und blendet die Verstärkung dann um 6 dB pro Frame auf höchstens die Untergrenze aus (`AGCDigitalGateFloor` / `AGCAnalogGateFloor`), damit Wortenden erhalten bleiben und Rauschen nicht angehoben wird; bis 6 dB darüber wird nicht hochgeregelt
- die Verstärkung läuft als Rampe Sample für Sample durch jeden Frame, statt alle 20 ms zu springen
- ein Begrenzer knapp unter Vollaussteuerung, der die angewendete Verstärkung sofort kappt, es übersteuert also nie

Die Grenzen sind absichtlich ungleich: Absenken ist unkritisch, Anheben hebt auch das Rauschen. Typisches DMR-Audio liegt um -35 dBFS, für ein Ziel von -16 dBFS braucht es also etwa +20 dB.

## Überwachung

### Web-Dashboard

`http://localhost:8080` auf dem tcd-Rechner, von anderswo über `ssh -L 8080:localhost:8080 <tcd-host>`, oder `http://<tcd-host>:8080` mit `MonitorBindAddress = 0.0.0.0`:

- im Stil des urfd-Dashboards (seit 0.4.0): ein bernsteinfarbenes LCD pro Modul mit Eingangs-Codec, Zielen und AGC-Zustand, helles und dunkles Design; die Schrift Saira ist in die Seite eingebaut, sie lädt also nichts von außen
- LCD-Balkenanzeigen vor und nach der AGC, mit Spitzenwert und markiertem AGC-Ziel
- Signalweg des aktiven Streams mit dem Pegel an jeder Stufe
- AGC- und Verstärkungsregler, die sofort wirken, und ein Knopf, um sie in `tcd.ini` zu speichern
- DVSI-Geräte: Seriennummer, Typ, Rolle, belegte Kanäle mit ihren Modulen, Puffertiefe
- MD380-Status und -Zähler, Verbindung zum Reflektor und Paketzähler

### tcdmon

Monitor im Terminal, praktisch über SSH:

```bash
tcdmon                      # this machine, port 8081
tcdmon 192.0.2.20 8081      # another machine
```

Zeigt Pegelbalken, Signalweg, AGC-Zustand, die DVSI-Kanäle mit ihren Modulen, die MD380-Zähler und die Ausgangsverstärkungen. `q` beendet.

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
| Warnung `USB latency timer is 16 ms` | tcd konnte den Latenz-Timer nicht setzen; er muss als root laufen (die systemd-Unit tut das) |
| `Lost the AMBE device ..., shutting down` | das Gerät wurde abgezogen oder zurückgesetzt; systemd startet tcd nach 5 s neu, und er findet das Gerät wieder |
| der Reflektor zeigt keinen Transcoder | `ServerAddress` und `Port` in `tcd.ini`, auf beiden Seiten dieselben `Modules`, und Port 10100/tcp vom tcd-Rechner aus erreichbar |

## Änderungen gegenüber dem Original

### Hardware und Geräte
- **Serieller Zugriff ohne libftd2xx**: über den Kernel-Treiber `ftdi_sio` und `/dev/serial/by-id` (POSIX-Anbindung von Jason Kendall), RTS/CTS-Flusskontrolle, USB-Latenz-Timer auf 1 ms; von einem älteren tcd gelöste Geräte werden wieder gebunden; ein verlorenes Gerät startet tcd über systemd neu
- **Geräteauswahl**: `--list-devices`, `DeviceSerial` mit exaktem Vergleich
- **Mischbetrieb DV3000 + DV3003**: zwei Module gleichzeitig mit Codec-Einstellung pro Kanal
- **D-Star auf dem DV3003**: Companding aus, seriellen Puffer leeren, Kodierung pro Kanal
- **SVX-Codec-Pfad**: eigener `ECodecType::svx` für SVX-Audio

### Audio
- **Ausgangsverstärkung pro Codec**: nach der AGC, für D-Star, M17/Codec2, DMR/YSF, P25/NXDN und USRP, angewendet auf Kopien (der gemeinsame Puffer wird nie verändert)
- **AMBE2+-Bitstrom-Verstärkung**: `AmbeGainDb` ändert den Parameter b2 im AMBE2+-Frame, mit neu berechnetem Golay(24,12)-FEC für die A-Partition; die B-Partition wird entwürfelt und mit den neuen A-Daten wieder verwürfelt (0.2.3; vorher verlor der Decoder die B-Partition); seit 0.2.5 von der AGC gesteuert und im γ-Bereich gerechnet, die Verstärkung hängt also nicht mehr vom Pegel ab, auch mit nur einem Gerät und dem MD380
- **AGC v3**: gleitendes RMS-Fenster, Verfolgung pro Sprecher, Gate mit Rücklauf, schnelles Anheben nach dem Gate, ungleiche Grenzen, Noise-Gate, Begrenzer, der die angewendete Verstärkung sofort kappt (0.2.3; vorher clippten laute Einsätze nach leiser Sprache), Änderungen live aus dem Dashboard; seit 0.2.3 threadsicher, da die Dekodier-Threads aller Codecs sie nutzen. Seit 0.2.4 hält das Gate 60 ms nach und blendet aus statt umzuschalten (Wortenden werden nicht mehr abgehackt), Rauschen knapp darüber pumpt die Verstärkung nicht mehr hoch, und die Verstärkung läuft als Rampe durch jeden Frame statt alle 20 ms zu springen
- **AGC digital und AGC analog** (0.3.0): SVX- und USRP-Eingang haben ein eigenes Gate, eine eigene Grenze für die Verstärkung und eine Verstärkung bei geschlossenem Gate (`AGCAnalogNoiseGate`, `AGCAnalogMaxGainUp`, `AGCAnalogGateFloor`), das FM-Rauschen zwischen den Worten wird also abgesenkt statt um bis zu 30 dB angehoben; die übrigen Eingänge nutzen dieselben drei Schlüssel als `AGCDigital…` (0.3.1 hat `AGCNoiseGate` und `AGCMaxGainUp` in `AGCDigitalNoiseGate` und `AGCDigitalMaxGainUp` umbenannt, `AGCDigitalGateFloor` ergänzt, und `agc-analyze` nennt den Schlüssel des Profils des Streams); seit 0.3.2 steht die Verstärkung von SVX und USRP pro Anbindung nur noch in der `urfd.ini` (`UsrpGainIn`/`UsrpGainOut` entfallen); ohne Monitor behält tcd die Werte aus der ini (vorher fiel er nach einer Sekunde auf die eingebauten Standardwerte zurück)
- **Rauschunterdrückung für analoge Eingänge** (0.5.0): SVX und USRP laufen vor der AGC analog durch den speexdsp-Präprozessor (`AGCAnalogNoiseSuppress`, Standard -15 dB, live im Monitor; 0.5.1 hat sie auf -10 dB gesenkt und `AGCAnalogGateFloor` von -12 auf 0, die Rauschunterdrückung senkt die Pausen ab), das FM-Rauschen unter der Sprache wird auf DMR also nicht mehr zu roboterhaften AMBE-Artefakten; neue Build-Abhängigkeit `libspeexdsp-dev`
- **DMR neu kodieren** (`md380=true`): `DmrGainMode = reencode` mit AGC und `DmrReencodeGain` für DMR → DMR/YSF
- **MD380 pro Stream getrennt**: Kodierer-Zustand pro Stream gesichert, MD380-Aufrufe nacheinander; seit 0.2.7 startet ein neuer Stream mit dem Zustand direkt nach `md380_init()` statt mit dem des vorherigen Streams; seit 0.2.9 bei jeder MD380-Nutzung, auch als primärer DMR-Codec mit nur einem Gerät
- **Ausgangsverstärkung im Mischbetrieb** (0.2.3): Der D-Star-Kanal des DV3003 nutzt `OutputGainDStar` statt `OutputGainDMR`; M17 mit 1600 bit/s schickt die zweite Hälfte jedes Pakets nicht mehr zweimal durch die AGC; seit 0.2.11 geht ein Modul, das nicht in der Modulliste steht, an keinen DMR-Kanal

### Überwachung und Konfiguration
- **Web-Dashboard**: eingebauter HTTP/WebSocket-Server, seit 0.4.0 im Stil des urfd-Dashboards, mit Signalweg, LCD-Pegelanzeigen, Verstärkungs- und AGC-Reglern, Geräte- und MD380-Status, Speichern in die INI
- **tcdmon**: Monitor im Terminal
- **Statistik-CSV** mit automatischem Aufräumen und dem Werkzeug `agc-analyze`; seit 0.2.9 meldet es Flackern des Gates als Öffnungen unter 100 ms (Rauschen über der Schwelle, das Noise-Gate des Profils anheben), da das Gate seit 0.2.4 vor dem Schließen nachhält
- **Umbenannte Schlüssel**: `DmrGainIn/Out` (war `DmrYsfGainIn/Out`), `UsrpGainIn/Out` (war `UsrpRxGain/TxGain`, seit 0.3.2 entfallen), Verstärkungen erweitert auf ±40 dB

### Zuverlässigkeit
- **Monitor standardmäßig nur lokal**: `MonitorBindAddress` (Standard `127.0.0.1`) für Web-Dashboard und Statistik-Socket, die kein Passwort haben
- **MD380 optional**: Ohne ihn sind zwei AMBE-Geräte nötig, und Neukodieren gibt es nicht; seit 0.2.5 deckt `ambe` DMR → DMR/YSF auch ohne ihn ab
- **Leistung**: blockierendes Lesen mit Timeout statt Dauerabfrage; seit 0.2.12 mit `-O2` gebaut, ein transkodierter Stream (Codec2, IMBE und AGC in Software) braucht etwa 7 % eines Kerns auf einem Raspberry Pi 4 und 3 % auf einem Pi 5, ein Pi 4 reicht für zwei Module ohne MD380 also locker
- **Threadsicherheit**: Sperre um den IMBE-Vocoder, SIGPIPE ignoriert für saubere Neuverbindungen
- **Fehlerbehandlung**: Wiederholung zum Reflektor mit wachsender Pause, Verwerfen bei Überlauf der Warteschlange, Erholung bei Timeouts beim Lesen
- **Wachsende Pause beim Neuverbinden**: 0,5 s bis 5 s, solange der Reflektor nicht erreichbar ist
- **Keine Nagle-Verzögerung**: `TCP_NODELAY` auf der Verbindung zum Reflektor; ein Frame ist nach etwa 23 ms statt 60 ms zurück
- **Build**: Teil des urfd-Repositorys, `build.sh` baut als Eigentümer des Klons

## Copyright

- Copyright (c) 2021-2023 Thomas A. Early N7TAE, Doug McLain AD8DP
- POSIX-Anbindung: Jason Kendall (2026)
- Copyright (c) 2026 Jens-Christian Merg DL4JC (Verbesserungen von urfd-ng)
