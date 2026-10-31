# urfd-ng - Universal Digital Voice Reflector und Transcoder

[English](README.md) | **Deutsch**

Basiert auf [urfd](https://github.com/n7tae/urfd) von Thomas Early N7TAE, mit erweiterten Funktionen, dem hybriden Transcoder [tcd](tcd/) und einem Setup-Script, das beides installiert.

**urfd 4.32.0** | tcd 0.15.0 | Dashboard v2.14.2

**Neu hier? Mit dem Setup-Script installieren, siehe [docs/INSTALL.de.md](docs/INSTALL.de.md):**

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd && cd urfd && sudo ./setup.sh
```

## Neu in urfd-ng

### Setup-Script und Transcoder
`setup.sh` installiert nach ein paar Fragen den Reflektor (in Docker, mit Dashboard) und den Transcoder (mit systemd), auf einem Rechner oder auf zwei verteilt, und aktualisiert beide auch. Der Transcoder liegt in [`tcd/`](tcd/), Reflektor und Transcoder stammen also immer aus derselben Version. tcd spricht die AMBE-Sticks über den Kernel-Treiber `ftdi_sio` mit einem USB-Latenz-Timer von 1 ms an und braucht damit den libftd2xx-Treiber nicht mehr, den ftdichip.com nicht mehr per Script herunterladen lässt. Ein tcd auf einem anderen Rechner meldet sich seit 4.16.0 mit einem gemeinsamen Passwort an (`[Transcoder] Password` in `urfd.ini`, `Password` in `tcd.ini`), so kann der Transcoder-Port zum Internet offen sein; das Setup-Script erzeugt es. Eine Verbindung, die beim Anmelden hängen bleibt, hält den Status-Thread des Reflektors nicht mehr auf. Seit tcd 0.10 bringen [Codec-Plugins](tcd/README.de.md#codec-plugins) D-Star und DMR/YSF in Software mit, tcd arbeitet damit auch mit einem AMBE-Stick, mit mehr Modulen, als die Sticks Kanäle haben, oder ganz ohne Stick. Seit 4.25.0 überlässt `[Transcoder] Modules = auto` die Wahl tcd: Ein Modul wird transcodiert, solange tcd dafür verbunden ist; die Liste steht damit nur noch in der `tcd.ini`, und ein dort neues Modul braucht weder eine Änderung noch einen Neustart von urfd. Seit 4.26.0 kann ein zweiter tcd für Module bereitstehen (`StandbyModules`): Er übernimmt sie, solange ihr tcd weg ist, etwa ein Software-tcd in Docker neben urfd für den mit den Sticks. Siehe [docs/INSTALL.de.md](docs/INSTALL.de.md) und [tcd/README.de.md](tcd/README.de.md).

### Gleicher Pegel in allen Betriebsarten
Die AGC von tcd bringt jeden Ruf auf denselben Pegel, egal von welchem Funkgerät oder Vocoder er kommt, und zwar auch bei DMR → DMR/YSF, wo die AMBE2+-Frames ohne Vocoder durchgereicht werden: Die Verstärkung der AGC geht in den Verstärkungsparameter des Frames (b2) statt in ein Dekodieren und Neukodieren, der Klang bleibt also so sauber wie das Original. SvxLink und AllStar (SVX, USRP) laufen durch eine eigene AGC analog: FM-Audio ist zwischen den Worten nicht still, sondern trägt das Rauschen des Empfängers, das die AGC für digitale Quellen um bis zu 30 dB anhob; bei 20 Minuten Verkehr aus dem FM-Funknetz blieb der Abstand zwischen Sprache und Rauschen damit bei 32 statt 19 dB. Zwei DVSI-Geräte, z.B. DV3000 + DV3003 für zwei Module, brauchen kein Codec-Plugin; ein Raspberry Pi 4 reicht locker. Siehe [tcd/README.de.md](tcd/README.de.md#pegel-dmrysf--dmrysf-dmrgainmode).

### XLX-Verzeichnis (CallingHome)
Der Reflektor trägt sich selbst im XLX-Verzeichnis ein, über das ihn Pi-Star, WPSD und andere Hotspot-Software anbieten. Das war bisher Aufgabe des Dashboards; jetzt geht es auch ohne Dashboard, und das Dashboard stellt die Anfrage nicht mehr.

```ini
[CallingHome]
Enable = true
Country = Germany              # country name as shown in the directory
Comment = My URF Reflector     # max 100 characters
# Hash =                       # letters and digits; blank = made at first start
# OverrideIP =                 # blank = the directory uses the address it sees
# PushDelay = 300              # seconds between updates
```

Gesendet wird dasselbe wie vorher vom Dashboard (Name `XLXnnn`, Laufzeit, `DashboardUrl` aus `[Names]`, Land, Kommentar, Version, Kopplungen). Ohne `Enable = true` wird nichts gesendet. Die Einstellungen werden beim Start gelesen.

Der Hash identifiziert den Reflektor im Verzeichnis, und das Verzeichnis bindet `XLXnnn` an den ersten Hash, den es sieht. Das Verzeichnis vergibt keine Hashes: Ist `Hash` leer, erzeugt der Reflektor beim ersten Start einen und speichert ihn als `Hash = ...` in `[CallingHome]` seiner `urfd.ini`, die einzige Zeile, die er dort je schreibt. Eine Hash-Datei gibt es nicht. Kann er `urfd.ini` nicht schreiben (etwa weil sie schreibgeschützt eingebunden ist), trägt er sich nicht ein und schreibt das ins Log. Danach den Hash nicht mehr ändern.

#### Einen schon eingetragenen Reflektor umziehen

Ein Reflektor, den bisher ein XLX-Dashboard oder ein älteres urfd eingetragen hat, muss seinen Hash behalten, einen neuen würde das Verzeichnis für denselben Namen nicht annehmen. Setze ihn vor dem ersten Start mit `Enable = true`.

1. Prüfen, ob dein Name im Verzeichnis steht (hier `XLX363`, die Nummer deines `URFnnn`):
   ```bash
   curl -s 'http://xlxapi.rlx.lu/api.php?do=GetReflectorList' | grep -A6 '<name>XLX363</name>'
   # ohne curl:
   wget -qO- 'http://xlxapi.rlx.lu/api.php?do=GetReflectorList' | grep -A6 '<name>XLX363</name>'
   ```
   Nicht eingetragen: nichts zu tun, der Reflektor erzeugt seinen Hash selbst. Gehört der eingetragene Reflektor nicht dir, nimm eine andere Nummer.
2. Eingetragen und deiner: den Hash vom alten Server kopieren. Die Datei steht als `$CallingHome['HashFile']` in der `config.inc.php` des alten Dashboards:
   ```bash
   grep '\$Hash' /pfad/zur/callinghome.php
   ```
   Lesen lässt er sich nur dort, nicht übers Web: Das Dashboard führt die Datei als PHP aus und liefert nichts.
3. In `urfd.ini` eintragen und starten:
   ```ini
   [CallingHome]
   Enable = true
   Hash = Ab3dE4fG5hIj6K7L
   ```
   `docker logs urfd | grep CallingHome` zeigt danach `registered as XLXnnn`.

### MMDVMClient-Anbindung
Verbindet sich über das MMDVM-Protokoll mit einem beliebigen DMR-Master-Server (z.B. BrandMeister). Bildet Talkgroups auf Reflektor-Module ab, mit Unterstützung pro Timeslot. Mehrere TGs können sich ein Modul teilen: Die erste ist die primäre (TX/RX), weitere TGs sind sekundär (nur eingehend, RX). Dynamische TGs lassen sich zur Laufzeit über die Admin-Oberfläche hinzufügen.

```ini
[MMDVMClient]
Enable = true
MasterAddress = master.example.com  # MMDVM master server IP or DNS hostname
MasterPort = 62031
# LocalPort = 12345                 # Fixed local UDP source port (omit = OS-assigned)
DmrId = 1234567                     # Your registered DMR/hotspot ID
Password = changeme                 # MMDVM hotspot security password
Callsign = YOURCALL                 # Callsign (used for DMR-ID lookup on kerchunk)
Latitude = 0.0
Longitude = 0.0
Location = My City
Description = My URF Reflector
URL = https://example.com
RxFreq = 430412500             # anders als TxFreq: der Hotspot ist duplex (zwei Zeitschlitze), BrandMeister lehnt RX = TX ab
TxFreq = 439812500
# FallbackDmrId = 1234567           # DMR ID for talkers without one (0 or omit = drop)
# BlockProtocols = SvxReflector,YSF # Block protocols bidirectionally
# Network = brandmeister.network    # Standard: brandmeister.network bei BM-Mastern, sonst der Host (siehe „Mehrere Verbindungen“)
#
# BrandMeister API (optional): sync static TGs, add/remove via API instead of kerchunk
# Get your API key from BrandMeister SelfCare -> Profile -> Security -> API Keys
# BrandMeisterApiKey = eyJ0eXAiOiJKV1Qi...
#
# TG mapping: TG<number> = <Module>[,TS<1|2>]
# First TG per module = primary (TX/RX). Additional = secondary (RX only).
# Default timeslot is TS2.
TG91 = A,TS2                        # Worldwide -> Module A (primary)
# TG262 = A,TS2                     # Germany -> Module A (secondary, RX only)
# TG26363 = B,TS1                   # Local -> Module B (primary)
```

**Mehrere TGs pro Modul**: Die erste für ein Modul eingetragene TG ist die primäre und wird für ausgehenden Verkehr (TX) verwendet. Weitere TGs auf demselben Modul sind sekundär und empfangen nur eingehenden Verkehr (RX). Über die Admin-Oberfläche hinzugefügte dynamische TGs sind immer sekundär, außer das Modul hat noch keine primäre.

**FallbackDmrId**: Lässt sich ein Rufzeichen keiner DMR-ID zuordnen, wird stattdessen diese ID verwendet. Ist sie nicht gesetzt oder 0, wird der Stream verworfen.

**BrandMeisterApiKey** (optional): Schaltet die Anbindung an die BrandMeister REST API v2 ein. Den Schlüssel gibt es unter BrandMeister SelfCare -> Profile -> Security -> API Keys. Auswirkungen:
- **Abgleich beim Start**: Holt die statischen TGs von BM, entfernt verwaiste (nicht in der Konfiguration) und ergänzt fehlende
- **Dynamische TG hinzufügen**: Wird per API bei BM als statisch eingetragen (kein Kerchunk nötig)
- **Dynamische TG entfernen**: Wird per API bei BM gelöscht
- Ohne API-Schlüssel gilt wie bisher das Kerchunk-Verfahren

**Mehrere Verbindungen** (4.21.0, mit Namen seit 4.27.0): weitere Sektionen neben `[MMDVMClient]`, höchstens 9 Verbindungen. Jede ist ein eigener Hotspot mit zwei Zeitschlitzen und eigener TG-Liste. So können mehr Module gleichzeitig mit BrandMeister sprechen (ein Ruf je Zeitschlitz und Verbindung), oder der Reflektor hängt sich an weitere Netze (TGIF, FreeDMR, ...). Jede Verbindung braucht eine eigene Hotspot-ID (z. B. die DMR-ID mit den Endungen `01`, `02`, ...); ein BrandMeister-Hotspot-Passwort gilt für alle. Was eine weitere Sektion weglässt (`Latitude`, `Longitude`, `Location`, `Description`, `URL`, `RxFreq`, `TxFreq`), kommt aus `[MMDVMClient]`: BrandMeister trennt einen Hotspot, der die nackten Standardwerte schickt. Eine Verbindung im Netz von `[MMDVMClient]` ohne eigenen `BrandMeisterApiKey` nutzt dessen Schlüssel (er gilt für alle Hotspots des Kontos): BrandMeister übernimmt die TGs aus den Login-Optionen nicht, erst die API macht sie auf dem neuen Hotspot statisch.

```ini
[MMDVMClient]                       # die gemeinsamen Einstellungen der Verbindungen darunter
Enable = true
MasterAddress = 2622.master.brandmeister.network
MasterPort = 62031
DmrId = 1234567                     # deine DMR-ID; die Verbindungen hängen ihre Endung an
Password = changeme
Callsign = N0CALL
BrandMeisterApiKey = ...
# keine TG hier: dann ist [MMDVMClient] keine eigene Verbindung

[MMDVMClient.01]                    # Hotspot 123456701
TG262 = D,TS2                       # Deutschland auf Modul D
TG91 = W,TS1                        # Weltweit auf Modul W

[MMDVMClient.02]                    # Hotspot 123456702
TG26363 = F,TS1

[MMDVMClient.TGIF]                  # ein anderes Netz: eigener Master, eigenes Passwort, eigene ID
MasterAddress = tgif.network
Password = ...
DmrId = 123456790
TG31665 = T,TS2
```

**Modul-Talkgroups** (4.28.0): `[Modules]` kann jedem Modul einmal seine Talkgroup geben, `TG<M> = <nummer>` (jede Nummer auf einem Modul). Eine Verbindung bindet sie dann mit `TS1 = <module>` und `TS2 = <module>` statt der TG-Nummer ein: `TS1 = R` ist `TG2625 = R,TS1` bei `TGR = 2625`. Die TG-Zeilen gehen daneben weiter (z. B. für eine zweite, nur empfangene TG auf einem Modul). Plugins können die Talkgroups ebenfalls nehmen (`CConfigure::GetModuleTG`).

```ini
[Modules]
Modules = ADFR
TGD = 262
TGF = 26363
TGR = 2625

[MMDVMClient.50]
TS1 = F
TS2 = D,R                           # mehrere Module auf einem Zeitschlitz
```

**Verbindungen mit Namen** (4.27.0): `[MMDVMClient.<name>]`, der Name aus Buchstaben, Ziffern, `-` und `_`. Eine benannte Verbindung übernimmt, was sie weglässt, aus `[MMDVMClient]`: Master, Port, Passwort, Rufzeichen, `FallbackDmrId`, `Software`, `Firmware`, die Standort-Schlüssel, `Network` zusammen mit dem Master und in diesem Netz den `BrandMeisterApiKey`. Ihre Sektion enthält also nur ihr Eigenes: ihre Hotspot-ID und ihre TGs (`LocalPort`, `BlockProtocols` und `Enable` gelten je Verbindung). Eine `DmrId` mit ein oder zwei Ziffern ist eine Endung an die `DmrId` aus `[MMDVMClient]`, eine längere die ganze ID; ohne `DmrId` ist ein Name aus Ziffern die Endung (`[MMDVMClient.50]` ist Hotspot `<DmrId>50`). `[MMDVMClient]` mit eigenen TGs ist selbst auch eine Verbindung, ohne sie liefert es nur die gemeinsamen Einstellungen. Der Name steht im Log, auf der Admin-Seite (TG-Tabelle, Reconnect-Knöpfe, Protokoll-Steuerung), in `BlockProtocols` (`BlockProtocols = MMDVMClient.TGIF`) und bei den Admin-Befehlen (`"client": "MMDVMClient.TGIF"`). Die nummerierten `[MMDVMClient2]` bis `[MMDVMClient9]` gehen weiter, mit allen ihren Schlüsseln; eine Datei nutzt benannte oder nummerierte Sektionen, nicht beides. Verbindungen hinzufügen, entfernen oder umsortieren braucht einen Neustart.

- **Netz.** Jede Verbindung, auch `[MMDVMClient]`, gehört zum Netz aus ihrem Schlüssel `Network`. Ohne ihn ist jeder BrandMeister-Master (`*.brandmeister.network`) `brandmeister.network`, jeder andere Master ein eigenes Netz (sein Hostname oder seine Adresse). `Network` braucht es für zwei Master eines anderen Netzes oder für einen BrandMeister-Master, der per IP-Adresse eingetragen ist.
- **Brücken.** Ein Ruf geht nie an die Verbindung zurück, von der er kam, und nie an eine andere Verbindung im selben Netz (das Netz hat ihn schon). Zwischen verschiedenen Netzen wird ein Modul, das beide gemappt haben, gebrückt; `BlockProtocols = MMDVMClient3` schaltet das für dieses Paar ab.
- **Namen.** In allen `BlockProtocols` und bei den Admin-Befehlen `block`/`unblock` heißt `MMDVMClient` alle Verbindungen, der Name einer Verbindung (`MMDVMClient.TGIF`, bei nummerierten `MMDVMClient1` bis `MMDVMClient9`; `MMDVMClient1` ist `[MMDVMClient]`) eine davon. Bei mehreren Verbindungen listet der Admin-Status sie unter diesen Namen.
- **Prüfungen.** `inicheck` lehnt eine TG oder Hotspot-ID ab, die zwei Verbindungen desselben Netzes nutzen; die Admin-Seite lehnt so eine dynamische TG ab.
- **Module ohne Transcoding.** Eine MMDVMClient-TG darf auf einem Modul liegen, das tcd nicht transkodiert. Von dort gehen nur AMBE+2-Aussendungen (DMR, YSF, NXDN) an den Master, erkannt am ersten Frame; D-Star, M17, SVX und die übrigen bleiben lokal, im Netz kämen sie als Stille an.
- **Admin.** `tg_add`, `tg_remove`, `reconnect` und `kerchunk` nehmen `"client": 2` oder `"client": "MMDVMClient.50"` (oder das Protokoll `mmdvmclient2`); ohne das ist `[MMDVMClient]` gemeint, bei einer TG die Verbindung, die sie hat, und bei `reconnect` alle. `tg_list` und `status` listen jede Verbindung mit ihrem Netz. Das Dashboard (v2.13.0) bietet beim Hinzufügen einer TG jede Verbindung an und hat für jede einen Reconnect-Button.

### SvxReflector-Client
Verbindet sich mit SvxLink-SvxReflector-Servern (z.B. FM-Funknetz) und überträgt FM-Audio in beide Richtungen. Signalisierung über TCP, OPUS-kodiertes Audio über UDP. Benötigt transcodierte Module. Dynamische TGs lassen sich zur Laufzeit über die Admin-Oberfläche hinzufügen.

```ini
[SvxReflector]
Enable = true
Host = svxreflector.example.com    # DNS hostname or IP
Port = 5300                         # TCP and UDP port (same)
Callsign = YOURCALL-HS             # Node callsign (as registered on server)
Password = changeme                 # Authentication password
# BlockProtocols = MMDVMClient,USRP # Block protocols bidirectionally
# RxGain = 0                        # Incoming audio gain in dB (-40 to +40, default 0)
# TxGain = 0                        # Outgoing audio gain in dB (-40 to +40, default 0)
# Software = urfd                   # software name in the node info; for servers that require a specific one
#
# TG mapping: TG<number> = <Module>
# Modules must be transcoded. Multiple TGs per module supported.
# TG26363 = S
```

**BlockProtocols**: Unterstützt bei MMDVMClient, SvxReflector, SVXServer, DCSClient, DExtraClient, DPlusClient und YSFClient. Audio aus den aufgeführten Quell-Protokollen wird nicht über diese Anbindung weitergeleitet. Jedes Client-Protokoll verhindert außerdem immer, dass Audio zu ihm selbst zurückläuft (DCSClient schickt z.B. nie Audio, das von DCSClient kam, wieder hinaus). Verfügbare Protokollnamen (seit 4.20.0 eine Tabelle für alle): `MMDVM`, `MMDVMClient` (alle Verbindungen) oder `MMDVMClient1` bis `MMDVMClient9` (eine), `SvxReflector`, `SVX` oder `SVXServer`, `DExtra`, `DExtraClient`, `DPlus`, `DPlusClient`, `DCS`, `DCSClient`, `DMRPlus`, `YSF`, `YSFClient`, `M17`, `NXDN`, `P25`, `USRP`, `URF`, `XLXPeer`, `G3` und der Name eines geladenen [Plugins](#plugins). Durch Komma getrennt.

**Eine TG zur Zeit**: Ein Knoten am SvxReflector hat genau eine ausgewählte TG; er bekommt nur deren Audio, und sein Audio geht dorthin. urfd wählt die erste feste TG (die niedrigste Nummer, die Heimat-TG) und überwacht alle zugeordneten TGs (`MsgTgMonitor`, wie `MONITOR_TGS` bei SvxLink). Ein Sprecher auf einer überwachten TG wählt diese TG, wenn urfd seit 3 s keinen Verkehr hatte; Audio eines urfd-Moduls, dessen TG nicht gewählt ist, wählt sie, außer auf der gewählten spricht gerade jemand, dann geht der Durchgang dieses Moduls nicht hinaus. Nach 30 s ohne Verkehr kehrt urfd zur Heimat-TG zurück. Mit nur einer zugeordneten TG ändert sich nichts.

**RxGain / TxGain**: Feste Verstärkung für SVX-Audio, pro Anbindung wie `[USRP] RxGain/TxGain`. RxGain wirkt nach dem OPUS-Dekodieren vor dem Transcoder, TxGain nach dem Transcoder vor dem OPUS-Kodieren; Werte über Vollaussteuerung werden begrenzt. Die AGC analog von tcd (eigenes Gate bei −40 dBFS, siehe [tcd/README.de.md](tcd/README.de.md#agc)) läuft auf SVX-Audio nach RxGain; RxGain daher nahe 0 lassen und den Pegel der AGC überlassen, ein stark negativer RxGain drückt leise FM-Sprache unter das Gate. Standard ist für beide 0 dB.

**Software**: Der Software-Name, den urfd nach der Anmeldung in seiner Node-Info meldet (`sw`), standardmäßig `urfd`. Manche Reflektoren erwarten einen bestimmten; nur in Absprache mit dem Betreiber des Reflektors ändern. Der Wert wird als JSON-maskierter Text gesendet.

### SVXServer
Nimmt eingehende Verbindungen von SvxLink-Knoten an. Arbeitet als SvxReflector-Server (Protokoll V2, HMAC-SHA1-Authentifizierung, OPUS-Audio). Benötigt transcodierte Module. Nutzer melden sich mit Rufzeichen und Passwort an. Jeder Knoten wählt per `MsgSelectTG` eine TG, zugeordnete TGs führen zu einem Modul, und Knoten auf derselben TG hören sich gegenseitig. Nutzer und TGs lassen sich zur Laufzeit über die Admin-Oberfläche verwalten.

```ini
[SVXServer]
Enable = true
Port = 5300                         # TCP and UDP port (same)
# BlockProtocols = MMDVMClient,USRP # Block protocols bidirectionally
# RxGain = 0                        # Incoming audio gain in dB (-40 to +40)
# TxGain = 0                        # Outgoing audio gain in dB (-40 to +40)
#
# TG mapping: TG<number> = <Module>
# Modules must be transcoded. Multiple TGs per module supported.
# TGs = auto                       # every module with a talkgroup ([Modules] TG<M>); TG lines win
# TG26363 = A
#
# User authentication: <Callsign> = <Password>
# Users can also be added/removed at runtime via the Admin interface.
# N0CALL-HS = changeme
```

**Nutzerverwaltung**: Nutzer lassen sich zur Laufzeit über die Admin-Oberfläche hinzufügen und entfernen oder fest in `urfd.ini` eintragen. Jeder Nutzer meldet sich mit Rufzeichen und Passwort per HMAC-SHA1 an.

**TG-Zuordnung**: Die Module müssen transcodiert sein. Mehrere TGs pro Modul sind möglich. Dynamische TGs lassen sich über die Admin-Oberfläche mit einstellbarer Laufzeit (TTL) hinzufügen.

**TG-Auswahl wie beim SvxReflector**: Ein Knoten hat genau eine ausgewählte TG, `MsgSelectTG` ersetzt sie; `MsgTgMonitor` legt fest, von welchen TGs er Sprecherbeginn und -ende bekommt. Eine TG hat jeweils einen Sprecher. Die anderen Knoten auf der TG bekommen dessen OPUS-Pakete unverändert (ohne Transcoding), auch auf TGs ohne Modul; zugeordnete TGs gehen außerdem zu ihrem urfd-Modul und kommen von dort. Die Knoten müssen Protokoll 2.0 sprechen. SvxLink mit `AUTH_KEY` tut das, auch aktuelle Versionen (getestet mit SvxLink 1.9.99.36 / 25.05, 15.10.2026); ein Knoten, der nur für Zertifikate eingerichtet ist, nutzt Protokoll 3 (TLS), das urfd nicht unterstützt: Er wird um einen Downgrade gebeten und loggt, dass er das nicht kann.

**SvxLink-Knoten einrichten**, in `[ReflectorLogic]` (Beispiel für TG26363 auf einem urfd-Modul):
```ini
HOSTS = urf.example.org:5300    # statt DNS_DOMAIN
CALLSIGN = N0CALL-HS            # der Nutzer in [SVXServer]
AUTH_KEY = changeme             # sein Passwort
DEFAULT_TG = 26363              # wird bei lokalem Verkehr gewählt
MONITOR_TGS = 26363             # im Leerlauf gehört, und das Modul, auf dem urfd den Knoten verlinkt
```
- Mit gesetzter `DEFAULT_TG` schaltet SvxLink die erste Aussendung stumm, nachdem es auf TG 0 zurückgefallen ist (`TG_SELECT_TIMEOUT`, standardmäßig 30 s): `MUTE_FIRST_TX_LOC` ist standardmäßig an und behält diese Aussendung zurück (es wird gar kein Audio gesendet), um Testdurchgänge und DTMF-Befehle vom Netz fernzuhalten. Die zweite Aussendung geht durch; `MUTE_FIRST_TX_LOC = 0` sendet auch die erste.
- Pegel: Der Empfänger des Knotens darf nicht übersteuern. Ein SHARI-Knoten (CM108) mit `Mic Capture Volume` 23 von 35 übersteuerte jede Aussendung (Sprache bei -6 dBFS, Spitzen bei Vollaussteuerung), das klingt nach dem AMBE-Encoder kratzig; mit 15 kam die Sprache mit -16 dBFS ohne Übersteuerung (`amixer -c Device cset numid=8 15`, danach `alsactl store`). `RxGain` in der urfd.ini bei 0 lassen und den Pegel der AGC analog im tcd überlassen.
- Das FM-Rauschen unter der Sprache klingt nach dem AMBE-Encoder auf DMR roboterhaft; tcd entfernt seit 0.5.0 den größten Teil davon vor dem Kodieren (`AGCAnalogNoiseSuppress`, siehe [tcd/README.de.md](tcd/README.de.md#agc)).
- Der Knoten erscheint in den Links auf dem Modul seiner gewählten TG, oder ohne gewählte TG (TG 0) auf dem seiner ersten beobachteten TG mit Modul, mit der Adresse, die urfd sieht: Ein Knoten im eigenen LAN des Reflektors zeigt seine interne IP.

**Admin-Befehle**:
```json
{"cmd": "svxs_user_add", "token": "...", "callsign": "N0CALL-HS", "password": "secret"}
{"cmd": "svxs_user_remove", "token": "...", "callsign": "N0CALL-HS"}
{"cmd": "svxs_user_list", "token": "..."}
{"cmd": "svxs_peer_list", "token": "..."}
```

### Plugins
Ein Protokoll kann auch aus einer Shared Library kommen, die urfd beim Start lädt, etwa eines, das nicht in diesem Repository liegt. `[Plugins]` nennt die Dateien, jede `Load`-Zeile eine:

```ini
[Plugins]
Load = /usr/local/lib/urfd/myproto.so
```

Ein Plugin ist C++, gebaut gegen diesen Quellbaum (siehe [reflector/Plugin.h](reflector/Plugin.h)): Sein Protokoll ist ein `CProtocol` wie die eingebauten und nutzt die Klassen und Globals von urfd, die das `urfd`-Programm exportiert (es wird mit `-rdynamic` gelinkt). Es passt nur zu dem urfd, für das es gebaut wurde: urfd vergleicht Version, Build-Optionen (`DHT`, `debug`) und die Größe seiner wichtigsten Klassen und lehnt ein Plugin aus anderen Quellen ab. Plugins also bei jedem urfd-Update neu bauen.

- **Einstellungen**: Ein Plugin namens `Foo` liest den Abschnitt `[Foo]` der `urfd.ini`; urfd reicht ihm die Zeilen bei jedem Start und Reload. `inicheck` kennt Plugin-Abschnitte nicht und vermerkt sie nur.
- **Routing**: `Foo` ist ein Protokollname in jedem `BlockProtocols` und in den Admin-Befehlen `block`/`unblock`; seine Clients gehen durch den Gatekeeper wie die Knoten der anderen Protokolle.
- **Admin-Socket**: Seine eigenen Befehle gehen direkt mit Namen oder als `{"cmd": "plugin", "name": "Foo", "command": "...", ...}`; `status` listet die Plugins (`plugins`) mit dem, was sie ergänzen, `tg_list` ihre Module.
- **Dashboard**: Die Admin-Seite kennt kein Plugin. Jedes Plugin beschreibt seine Abschnitte in der Antwort auf `status` (Tabellen, die einer seiner Befehle füllt, ein Formular und ein Entfernen-Knopf, die andere aufrufen), und die Seite zeichnet sie, siehe [docs/PLUGINS.md](docs/PLUGINS.md). Die Protokollliste zeigt die Beschreibung des Plugins.
- **Logins**: Es kann seine Logins unter einem eigenen Dienstnamen in der [Zugangsdatenbank](#zugangsdaten-aus-einer-datenbank-4200) halten.
- **Bauen**: Den Quelltext des Plugins nach `plugins/<name>/` legen (mit einem `Makefile`, das `URFD=` versteht), z. B. als `git clone` seines Repositorys; git ignoriert ihn dort. Das Docker-Image baut es mit urfd nach `/usr/local/lib/urfd/`; nativ tun `make plugins` und `sudo make plugins-install` in `reflector/` dasselbe. `setup.sh update` holt jedes Plugin-Verzeichnis, das ein git-Clone ist (auch in `tcd/plugins/`), vor dem Bauen auf den neuen Stand.

### USRP (AllStar)
Verbindet einen AllStar-Knoten (USRP-Kanal von app_rpt) mit einem Modul. Benötigt ein transcodiertes Modul.

```ini
[USRP]
Enable = true
Callsign = ALLSTAR
IPAddress = 1.2.3.4            # AllStar node IP
RxPort = 34000
TxPort = 32000
Module = A                     # Must be a transcoded module
# RxGain = 0                   # Incoming audio gain in dB (-40 to +40)
# TxGain = 0                   # Outgoing audio gain in dB (-40 to +40)
```

**RxGain / TxGain**: wie bei SVX pro Anbindung in der `urfd.ini`; Werte über Vollaussteuerung werden begrenzt. tcd schickt AllStar-Audio durch seine AGC analog, die beiden bleiben daher normalerweise 0.

**Sprecher**: Ein TEXT-Paket mit SET_INFO-TLV vor dem Ton, wie es DVSwitch Analog_Bridge schickt, nennt den Sprecher (Rufzeichen und DMR-ID); urfd schickt dasselbe Paket vor jedem Stream, den es zum Knoten spielt. Ohne eines läuft der Stream unter `Callsign`, mit der DMR-ID dieses Rufzeichens aus der DMR-Datenbank, sonst bekommen DMR-Hotspots, DMR+ und P25 ihn mit `[MMDVM] FallbackDmrId`, ohne sie gar nicht. Das Unkey beendet den Stream.

### MMDVM-Server
Nimmt Verbindungen von MMDVM-Hotspots und -Repeatern direkt an (ohne BrandMeister). Implementiert das HomeBrew-DMR-Protokoll (Handshake RPTL→RPTK→RPTC→RPTO). Für reinen DMR-zu-DMR-Betrieb ohne Transcoder nutzbar, transcodierte Module sind nur für die Verbindung zwischen Betriebsarten nötig. Ein Sprecher ohne DMR-ID (in keiner ID-Datenbank registriert: AllStar, ein D-Star- oder M17-Rufzeichen) geht nur mit `FallbackDmrId` an die Hotspots, an DMR+ und an P25; ohne sie wird der Stream für sie verworfen, wie für die DMR-Netze (MMDVMClient) und für NXDN (`FallbackNxdnId`). Die TG-Zuordnung funktioniert wie beim MMDVMClient, mit Timeslots. **Dual-Slot**: Jeder Timeslot (TS1/TS2) verbindet sich unabhängig mit seinem eigenen Modul, ein Hotspot kann also gleichzeitig auf zwei Modulen sein.

```ini
[MMDVM]
Enable = true
Port = 62030
# FallbackDmrId = 0                # DMR ID for talkers without one, to the hotspots, DMR+ and P25 (0 or omit = drop)
# RequireAuth = false              # Set to false to accept all connections (legacy)
# BlockProtocols = SVXServer,USRP  # Block protocols bidirectionally
#
# TG mapping: TG<number> = <Module>[,TS<1|2>]
# First TG per module = primary (TX/RX). Additional = secondary (RX only).
# Default timeslot is TS2. Dual-slot: map different modules to different TSes.
# From the module talkgroups ([Modules] TG<M>): TS1 / TS2 = <modules> maps
# them on that timeslot, TGs = auto every other module with a talkgroup
# (on TS2, or TGs = auto,TS1); TG lines win.
# TS1 = F
# TGs = auto
TG9 = A,TS2                        # Common hotspot default
# TG26363 = F,TS1                  # Local TG -> Module F on TS1
#
# User authentication: <BASE-DMRID> or <CALLSIGN> = <password>
# No users configured = all logins rejected.
# One entry covers all 9-digit extensions (e.g. 1234567 covers 123456700-99).
# Password may be empty to allow any password (useful for compat hotspots).
# 1234567 = changeme               # 7-digit base DMR ID
# N0CALL = changeme                 # Callsign (resolved to DMR ID at startup)
# 2632545 =                        # Open access (no password required)
```

**Erweiterte DMR-IDs**: Hotspots im BrandMeister-Stil hängen an die 7-stellige DMR-ID eine 2-stellige Endung an (z.B. `123456701` = Basis `1234567` + Endung `01`). urfd entfernt die Endung selbst, ein Nutzer-Eintrag mit der Basis-ID gilt für alle Endungen.

**Nutzer-Anmeldung**: Nutzer melden sich mit DMR-ID (7-9 Ziffern) oder Rufzeichen an. Rufzeichen werden beim Start über die DMR-ID-Datenbank in DMR-IDs übersetzt. Sind keine Nutzer eingetragen, werden alle Anmeldungen abgelehnt. Einzelne Nutzer können mit leerem Passwort eingetragen werden, dann ist jedes Passwort gültig (nützlich für einfache Hotspots). Mit `RequireAuth = false` entfällt die Anmeldung ganz und alle Verbindungen werden angenommen (früheres Verhalten). Nutzer lassen sich zur Laufzeit über die Admin-Oberfläche verwalten.

**TG-Zuordnung**: Wie beim MMDVMClient: Die erste TG pro Modul ist primär (TX+RX), weitere TGs sind sekundär (nur RX). Dynamische TGs lassen sich über die Admin-Oberfläche mit einstellbarer Laufzeit hinzufügen.

**Dual-Slot**: Jeder Timeslot verbindet sich unabhängig. Sendet ein Hotspot auf TS1 mit TG 4019 (Modul S) und auf TS2 mit TG 4004 (Modul D), empfängt er beide Module gleichzeitig, Modul S auf dem Timeslot, der für dessen TG eingetragen ist, Modul D auf dem anderen. Dafür müssen TGs verschiedener Module auf verschiedenen Timeslots liegen. Das Dashboard zeigt beide verbundenen Module (z.B. "S (TS1), D (TS2)").

**Talker Alias**: Jeder Ruf kommt bei den Hotspots mit Talker Alias an, wie von BrandMeister: Rufzeichen und Vorname aus der DMR-ID-Datenbank (z.B. `N0CALL Max`), höchstens 13 Zeichen in UTF-16 wie bei BrandMeister, denn Motorola-Funkgeräte ignorieren einen Alias in ISO 8 Bit. Schickt die DMR-Quelle einen eigenen Alias (BrandMeister oder ein Funkgerät mit eigenem), wird sein Text weitergegeben, sobald er vollständig angekommen ist, neu kodiert in UTF-16 (Quellen schicken auch 7 Bit, ISO 8 Bit oder UTF-8) und wieder ab dem Header, damit sich keine Blöcke der beiden mischen; bis dahin, und bei D-Star, YSF, M17 und den anderen Betriebsarten, ist es unserer. Er steckt in der Embedded Signalling, ein LC in jedem zweiten Superframe; der Hotspot zeigt ihn an und gibt ihn ans Funkgerät weiter. Die Superframes dazwischen füllt MMDVMHost mit dem Voice-LC (Quelle, TG, seine eigene OVCM-Einstellung), ein Funkgerät, das spät in einen Ruf einsteigt, bekommt es also trotzdem. Die Service Options im Voice-LC (OVCM, Emergency, Privacy) sind immer 0: die Flags einer einzelnen Quelle werden nicht an alle verlinkten Hotspots weitergegeben.

**Hotspot-Einstellungen**: Den Hotspot direkt auf die IP des Reflektors, Port 62030, einstellen. In WPSD/Pi-Star "MMDVM_BRIDGE" oder den Einzel-Netzwerk-Modus verwenden. Der Multi-Netzwerk-Modus von DMRGateway leitet standardmäßig zu BrandMeister und muss abgeschaltet werden.

**XLX-Master-Modus** (WPSD/Pi-Star „XLX Master“ mit Startmodul) funktioniert ebenfalls: DMRGateway verbindet das Modul per Private Call an 4001-4026 und bekommt das Modul auf TS2/TG9, das es auf Slot und TG des Funkgeräts umsetzt (standardmäßig TG6). DMRGateway meldet sich dabei mit dem festen Passwort `passw0rd` aus `[XLX Network]` in `/etc/dmrgateway` an, das xlxd nie prüft. Entweder dort das urfd-Passwort des Nutzers eintragen (WPSD/Pi-Star: Configuration → Expert → DMR GW; nach dem Speichern der normalen Konfigurationsseite prüfen, die `passw0rd` zurückschreiben kann) oder dem Nutzer in `[MMDVM]` ein leeres Passwort geben, das jedes Passwort annimmt. Sonst meldet der Reflektor `auth FAILED`.

**Admin-Befehle**:
```json
{"cmd": "mmdvm_user_add", "token": "...", "dmrid": 1234567, "password": "secret"}
{"cmd": "mmdvm_user_add", "token": "...", "callsign": "N0CALL", "password": "secret"}
{"cmd": "mmdvm_user_add", "token": "...", "dmrid": 2632545}
{"cmd": "mmdvm_user_remove", "token": "...", "dmrid": 1234567}
{"cmd": "mmdvm_user_list", "token": "..."}
{"cmd": "mmdvm_tg_add", "token": "...", "tg": 26363, "module": "F", "ts": 2, "ttl": 900}
{"cmd": "mmdvm_tg_remove", "token": "...", "tg": 26363}
{"cmd": "mmdvm_tg_list", "token": "..."}
{"cmd": "mmdvm_peer_list", "token": "..."}
```

### D-Star-Client-Anbindungen (DCS, DExtra, DPlus)
Verbindet sich als Client-Knoten mit fremden D-Star-Reflektoren und ordnet deren Module lokalen Reflektor-Modulen zu. Alle drei D-Star-Linkprotokolle werden mit ihrem jeweiligen Paketformat unterstützt. Zuordnungen lassen sich fest in `urfd.ini` eintragen oder zur Laufzeit über die Admin-Oberfläche hinzufügen.

```ini
[DCSClient]
Enable = true
Callsign = N0CALL                         # Must be whitelisted on target reflector
Map1 = dcs002.xreflector.net,30051,A,S   # DCS002 module A -> local module S
Map2 = dcs009.xreflector.net,30051,B,F   # DCS009 module B -> local module F
# BlockProtocols = MMDVMClient,YSF       # Optional: block audio from these protocols

[DExtraClient]
Enable = true
Callsign = N0CALL
Map1 = xrf757.openquad.net,30001,A,A     # XRF757 module A -> local module A

[DPlusClient]
Enable = true
Callsign = N0CALL                         # Must be registered at dstargateway.org
Map1 = ref001.dstargateway.org,20001,C,A # REF001 module C -> local module A
```

**Format der Zuordnung**: `host,port,remote_module,local_module`. Jede Zuordnung baut eine Verbindung zum angegebenen Modul des fremden Reflektors auf und leitet das Audio auf das lokale Modul.

**Routing**: Die Anbindung spricht den Reflektor an wie ein mit ihm verbundenes Gateway: RPT1 ist `<Callsign> <lokales Modul>`, RPT2 der Reflektor mit dem fremden Modul (`DCS002 A`), und der Reflektor leitet nach diesem Modul. Das Rufzeichen des Reflektors kommt aus den Streams, die er schickt, bis dahin aus dem ersten Teil des Hostnamens (`dcs002.xreflector.net` -> `DCS002`); bei einer IP-Adresse und noch ohne Stream steht das eigene Rufzeichen der Anbindung dort, was urfd und xlxd annehmen (sie werten nur das Modul aus). Ein DPlus-Reflektor schickt die Streams aller Module an jeden Client: nur die des zugeordneten fremden Moduls kommen auf das lokale Modul.

**Rufzeichen**: DPlus-Reflektoren verlangen eine Registrierung bei [dstargateway.org](https://regist.dstargateway.org/). DExtra- und DCS-Reflektoren sind meist offen.

**Dynamische Zuordnungen**: Über den Abschnitt "D-Star Client Mappings" im Admin-Dashboard oder über die Socket-API:
```json
{"cmd": "dcs_map_add", "token": "...", "host": "dcs002.xreflector.net", "port": 30051, "remote_module": "A", "local_module": "S"}
{"cmd": "dcs_map_remove", "token": "...", "local_module": "S"}
{"cmd": "dcs_map_list", "token": "..."}
{"cmd": "dextra_map_add", "token": "...", "host": "xrf757.openquad.net", "port": 30001, "remote_module": "A", "local_module": "A"}
{"cmd": "dplus_map_add", "token": "...", "host": "ref001.dstargateway.org", "port": 20001, "remote_module": "A", "local_module": "Z"}
{"cmd": "reconnect", "token": "...", "protocol": "dcsclient"}
```

### YSF-Client-Anbindung
Verbindet sich als Client-Knoten mit fremden YSF-Reflektoren. Optional mit DG-ID zur Raumwahl auf Reflektoren, die DG-IDs auswerten. Jede Verbindung wird einem lokalen Reflektor-Modul zugeordnet.

```ini
[YSFClient]
Enable = true
Callsign = N0CALL
Map1 = 90.187.72.177,42099,A             # YSF reflector -> local module A
Map2 = 85.215.138.68,42000,F,15          # YSF reflector -> local module F, DG-ID 15
# BlockProtocols = MMDVMClient            # Optional
```

**Format der Zuordnung**: `host,port,local_module[,dgid]`. Die DG-ID ist optional (0 = aus). Ist sie gesetzt, steht sie im FICH-Header aller ausgehenden Sprachpakete.

**YSF-Feld "Radio"**: Ausgehende Sprachpakete tragen das Reflektor-Rufzeichen (z.B. `URF363`) im Radio-/Beschreibungsfeld (CSD1, Bytes 0-9). Auf YSF-Dashboards wie ysf-deutschland.de erscheint es in der Spalte "Radio".

**Dynamische Zuordnungen**:
```json
{"cmd": "ysf_map_add", "token": "...", "host": "90.187.72.177", "port": 42099, "local_module": "A", "dgid": 0}
{"cmd": "ysf_map_remove", "token": "...", "local_module": "A"}
{"cmd": "ysf_map_list", "token": "..."}
```

### NXDN-Modulwahl
Reflektor-Module werden über die NXDN-RAN (Radio Access Number) oder über die Ziel-ID gewählt.

```ini
[NXDN]
Enable = true
Port = 41400
AutoLinkModule = S             # Module for RAN 0 and RAN 1 (comment out to disable)
ReflectorID = 26363
# FallbackNxdnId = 8970        # NXDN ID for unknown callers (0 or omit = drop)
ModuleDstIdBase = 4000         # dstId 4001-4026 = A-Z (omit to disable)
```

**RAN zu Modul**: RAN 1-26 entspricht Modul A-Z, wobei RAN 1 und die RAN des AutoLinkModule getauscht sind. RAN 1 erreicht immer das AutoLinkModule, Modul A bekommt dafür dessen RAN. RAN 0 führt ebenfalls zum AutoLinkModule. Mit `AutoLinkModule = S`:

| RAN | Modul | RAN | Modul | RAN | Modul |
|-----|-------|-----|-------|-----|-------|
| 0   | S     | 10  | J     | 19  | **A** |
| 1   | **S** | 11  | K     | 20  | T     |
| 2   | B     | ... | ...   | 26  | Z     |

Der Tausch ist nötig, weil RAN 1 die einzige RAN ist, die die meisten Clients erzeugen können. DroidStar setzt fest `set_sacch_ran(0x01)`, und MMDVMHost überschreibt die RAN des Funkgeräts vor dem Weiterleiten mit der im Hotspot eingestellten `[NXDN] RAN` (Standard 1) und verwirft HF mit abweichender RAN. Ein Wechsel der RAN pro Aussendung vom Funkgerät aus funktioniert deshalb nur bei direkter Verbindung; über einen Hotspot ist die RAN eine Einstellung des Hotspots.

**Ziel-ID zu Modul** (hat Vorrang vor der RAN): Wird mit `ModuleDstIdBase` eingeschaltet. Ziel-ID `base+1` bis `base+26` entspricht Modul A-Z, jede andere Ziel-ID fällt auf die RAN zurück. Ausgewertet wird beim Verbindungs-Poll und in jedem Frame, das Modul stimmt also ab dem ersten Paket. Mit `ModuleDstIdBase = 4000` passen die NXDN-Ziel-IDs zur DMR+-TG-Zuordnung (4001 = A, 4002 = B, …).

Für Clients, die keine RAN setzen können, ist das die einzige Möglichkeit der Modulwahl. DroidStar-Nutzer legen unter Settings pro Modul einen eigenen Host an, die Nummer ist die Ziel-ID:

```
NXDN 4001 my.reflector.org 41400     -> Module A
NXDN 4003 my.reflector.org 41400     -> Module C
NXDN 26363 my.reflector.org 41400    -> AutoLinkModule
```

Ausgehende Frames tragen die Ziel-ID, mit der der jeweilige Client gepollt hat, pro unterschiedlicher Ziel-ID auf dem Modul getrennt kodiert. Das ist nötig, nicht kosmetisch: `NXDNGateway` verwirft jedes Frame des Reflektors, dessen Ziel-ID nicht zur TG passt, mit der der Hotspot verbunden ist. Ein Client auf der reinen Reflektor-ID und einer auf einer Modul-Ziel-ID brauchen also jeweils eigene Frames.

**NXDN-ID-Auflösung**: Eingehende NXDN-IDs werden über die NXDN-ID-Datenbank in Rufzeichen übersetzt. Fehlt eine ID dort, wird die DMR-ID-Datenbank verwendet (viele Funkamateure nutzen ihre DMR-ID auch als NXDN-ID). Das ermöglicht die Anzeige des Rufzeichens über Betriebsarten hinweg und die Namenssuche für D-Star-Slow-Data. Unbekannte IDs werden verworfen, außer FallbackNxdnId ist gesetzt.

**D-Star-Slow-Data**: NXDN-Quellen erscheinen im D-Star-Textfeld als `NXDN RAN<n>`. Betreibernamen werden in der DMR- und der NXDN-Datenbank gesucht.

**Dashboard**: Die Spalte NXDN RAN erscheint in der Modulübersicht, wenn NXDN eingeschaltet ist, daneben die Spalte `NXDN DST-ID`, sobald `ModuleDstIdBase` gesetzt ist. Die Spalten DMR+ und YSF DG-ID werden ausgeblendet, wenn das jeweilige Protokoll aus ist.

### Laufzeit dynamischer Talkgroups

Dynamische TGs von MMDVM und SVX laufen nach 15 Minuten ohne Aktivität ab. Der Timer wird bei jedem Sendebeginn (DvHeader) auf dem Modul erneuert, egal aus welchem Protokoll der Verkehr kommt. Aktivität aus beliebigen Betriebsarten hält also alle Timer einheitlich am Leben.

**Hinweis zu BrandMeister**: BM führt einen eigenen Timer für das TG-Abonnement. Läuft das BM-Abonnement ab, läuft ohne weitere Aktivität auch die lokale Zuordnung in urfd ab. Mit dem Admin-Befehl **kerchunk** lässt sich das Abonnement bei BM ohne Funkgerät verlängern.

**Hinweis zu SVX**: Läuft eine dynamische SVX-TG ab, nimmt urfd sie aus den überwachten TGs; war sie gewählt, wird wieder die Heimat-TG gewählt (oder keine, wenn keine TG übrig ist).

### Admin-Oberfläche
Verwaltung zur Laufzeit über einen JSON-basierten TCP-Socket, bedienbar über das Web-Panel im Dashboard. Läuft in einem eigenen Thread und blockiert die Audioverarbeitung nicht. Jede Client-Verbindung läuft in einem eigenen Thread mit 5 Sekunden Leerlauf-Timeout, langsame oder hängende Clients können den Admin-Socket also nicht blockieren.

```ini
[Admin]
Enable = true
Port = 10101
Password = yoursecretpassword
# BindingAddress = 127.0.0.1      # Default: loopback only
```

Dashboard-Konfiguration (`config.inc.php`):
```php
$Admin['Enable']   = true;
$Admin['Host']     = '127.0.0.1';
$Admin['Port']     = 10101;
# Anmeldung auf der Admin-Seite mit dem [Admin]-Passwort aus urfd.ini
```

**Funktionen:**
- **Dynamische TG-Verwaltung**: Talkgroups zur Laufzeit mit einstellbarer Laufzeit hinzufügen und entfernen. Mit BM-API-Schlüssel per REST-API als statisch eingetragen (kein Kerchunk nötig), ohne per Kerchunk bei BM aktiviert. Bei SVX werden die überwachten TGs angepasst.
- **Protokoll-Sperren zur Laufzeit**: Beliebige Protokoll-Paare in beide Richtungen sperren und freigeben. Wird nicht gespeichert und gilt nach einem Neustart nicht mehr. Die Schaltfläche "Reset to Config Default" stellt die Einstellungen aus der INI wieder her.
- **Mehrere TGs pro Modul**: Sekundäre TGs (nur RX) dynamisch zu Modulen hinzufügen, die schon eine primäre TG haben.
- **Kerchunk auf Knopfdruck**: Einen Kerchunk für eine bestimmte TG an BrandMeister schicken, ohne Funkgerät. Nur MMDVM. Mit BM-API-Schlüssel nicht nötig.
- **D-Star-Client-Zuordnungen**: Verbindungen zu DCS-, DExtra- und DPlus-Reflektoren mit Modul-zu-Modul-Zuordnung dynamisch hinzufügen und entfernen. Protokollauswahl DCS/DExtra/DPlus mit passendem Standard-Port.
- **YSF-Client-Zuordnungen**: Verbindungen zu YSF-Reflektoren mit optionaler DG-ID hinzufügen und entfernen.
- **MMDVM-Server-Verwaltung**: Nutzer (per DMR-ID oder Rufzeichen) und TG-Zuordnungen hinzufügen, entfernen und auflisten. Verbundene Hotspots mit DMR-ID, Frequenzen, Standort und Software-Angaben anzeigen.
- **Protokoll neu verbinden**: Neuverbindung von MMDVM, SVX, DCSClient, DExtraClient, DPlusClient oder YSFClient erzwingen.
- **Transcoder-Statistik**: Verbindungsstatus, aktiver Codec, Paketzähler und Laufzeit pro transcodiertem Modul.
- **Live-Log**: Die letzten 50 Log-Zeilen mit Zeitstempel, alle 10 Sekunden aktualisiert (der Socket liefert bis zu 200).
- **Last Heard leeren**: Leert die Liste der zuletzt gehörten Stationen im Dashboard.
- **Versteckter Zugang**: Über das Pi-Symbol unten rechts oder direkt mit `?show=admin`.

**Socket-Protokoll**: Zeilenweises JSON über TCP. Zuerst anmelden, dann Befehle mit dem zurückgegebenen Token schicken:
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
{"cmd": "dcs_map_add", "token": "...", "host": "dcs002.xreflector.net", "port": 30051, "remote_module": "A", "local_module": "S"}
{"cmd": "dcs_map_remove", "token": "...", "local_module": "S"}
{"cmd": "dcs_map_list", "token": "..."}
{"cmd": "ysf_map_add", "token": "...", "host": "90.187.72.177", "port": 42099, "local_module": "A", "dgid": 15}
{"cmd": "ysf_map_list", "token": "..."}
{"cmd": "svxs_user_add", "token": "...", "callsign": "N0CALL-HS", "password": "secret"}
{"cmd": "svxs_user_remove", "token": "...", "callsign": "N0CALL-HS"}
{"cmd": "svxs_user_list", "token": "..."}
{"cmd": "svxs_peer_list", "token": "..."}
{"cmd": "mmdvm_user_add", "token": "...", "dmrid": 1234567, "password": "secret"}
{"cmd": "mmdvm_user_add", "token": "...", "callsign": "N0CALL", "password": "secret"}
{"cmd": "mmdvm_user_add", "token": "...", "dmrid": 2632545}
{"cmd": "mmdvm_user_remove", "token": "...", "dmrid": 1234567}
{"cmd": "mmdvm_user_list", "token": "..."}
{"cmd": "mmdvm_tg_add", "token": "...", "tg": 26363, "module": "F", "ts": 2, "ttl": 900}
{"cmd": "mmdvm_tg_remove", "token": "...", "tg": 26363}
{"cmd": "mmdvm_tg_list", "token": "..."}
{"cmd": "mmdvm_peer_list", "token": "..."}
{"cmd": "clear_users", "token": "..."}
```

### Zugangsdaten aus einer Datenbank (4.20.0)

Die Logins von `[MMDVM]` und `[SVXServer]` (und von [Plugins](#plugins), die sie nutzen) können auch aus einer Datenbank kommen, damit ein anderes Programm sie verwalten kann: SQLite, MySQL/MariaDB oder PostgreSQL. Nutzer in der `urfd.ini` bleiben gültig und haben Vorrang; nur wer dort fehlt, wird nachgeschlagen, und die Antwort bleibt in einem LRU-Cache (`CacheTTL`, Standard 300 s; unbekannte Nutzer `NegativeTTL`, 60 s). Ist die Datenbank weg, funktionieren Logins aus dem Cache weiter. Die Admin-Seite legt neue Nutzer in der Datenbank an. Die Passwörter stehen im Klartext darin, weil die Protokolle sie für ihre Challenge brauchen. Standardmäßig aus:

```ini
[Credentials]
Backend = sqlite           # ini (keine Datenbank), sqlite, mysql oder postgresql
Path = /usr/local/etc/urfd/credentials.db
# Host = localhost         # mysql, postgresql: Host, Port, Database, User, Password
# Table = urfd_credentials
# CacheTTL = 300
```

Tabelle, Cache, Bauen und die Admin-Befehle `credentials_status` und `credentials_flush`: [docs/CREDENTIALS.md](docs/CREDENTIALS.md) (englisch). Vorgeschlagen von Jason Kendall (coolacid), Issue #11.

### WebSocket (4.13.0, Bridges und Namen 4.14.0)

Live-Ereignisse für das Dashboard: Clients, Peers und sprechende Stationen (mit dem Namen des Funkers aus der ID-Datenbank), sobald sie sich ändern, die Bridges jedes Moduls (TG-Mappings, Autolinks, Connectoren zu anderen Reflektoren) und ihre Änderungen, und beim Verbinden der komplette Stand, ohne IP-Adressen. Nur lesend und ohne Passwort, daher lauscht es auf Loopback (`127.0.0.1:10102`), und das nginx des Dashboards reicht `/ws` durch. Immer eingebaut (braucht `libwebsockets-dev`), aus, bis `[WebSocket] Enable = true` gesetzt ist, was `setup.sh` bei der Installation und bei `setup.sh update` tut; Änderungen brauchen einen Neustart. Basiert auf dem Notifier von Jason Kendall (coolacid), dessen Live-Dashboard das Docker-Image unter `/live/` ausliefert, neben diesem Dashboard. Nachrichten und Einrichtung: [docs/WEBSOCKET.md](docs/WEBSOCKET.md) (englisch).

### Konfiguration per SIGHUP neu laden

Der Reflektor kann seine Konfiguration im laufenden Betrieb neu laden, ohne Verbindungen zu trennen. Dazu `SIGHUP` an den urfd-Prozess schicken.

**Wird bei SIGHUP neu geladen:**
- TG-Zuordnungen (MMDVMClient, SvxReflector und SVXServer), dynamische TGs bleiben erhalten
- Whitelist, Blacklist und Interlink-Datei
- Transcodierte Module (`[Transcoder] Modules`, auch von und zu `auto`, seit 4.25.1); die tcd-Verbindung eines Moduls, das nicht mehr in der Liste steht, wird getrennt
- Reflektor-Angaben (Rufzeichen, Sponsor, Land, URLs)
- Parameter für die Datenbank-Aktualisierung

**Braucht einen vollständigen Neustart:**
- Änderungen der Modulliste (Module hinzufügen oder entfernen)
- Protokolle ein- oder ausschalten, Ports ändern
- Änderungen der IP-Bindung
- Einstellungen der Admin-Oberfläche
- Zuordnung des Echo-Moduls
- Port, Bindungsadresse und Passwort des Transcoders

```bash
sudo ./setup.sh reload

# von Hand, Docker: das Signal muss urfd erreichen, nicht supervisord (PID 1 startet bei SIGHUP alles neu)
docker exec urfd pkill -HUP -x urfd

# ohne Docker
sudo pkill -HUP -x urfd
```

### Reflektor-Kopplung
Kopplung mit URF-, XLX-, DCS- und M17-Reflektoren (mrefd). DNS-Namen werden unterstützt. Eingetragen wird die Kopplung in `urfd.interlink`:

```
# urfd.interlink
# Format: <Callsign> <Address> <Modules> [Port] [Protocol]
URF270 urf270.example.com EF
XLX269 xlx269.example.com A
DCS002 dcs002.xreflector.net S             # native DCS protocol (default)
DCS002 dcs002.xreflector.net S XLX         # XLX protocol instead
M17-QQQ m17-qqq.example.com AB             # mrefd >= 1.0 (port 17000)
M17-QQQL m17-qqq.example.com AB            # legacy mrefd < 1.0
M17-QQQ AB                                 # mrefd, Adresse aus dem Ham-DHT
```

DCS-Einträge verwenden standardmäßig das native DCS-Protokoll (Port 30051). Mit angehängtem `XLX` wird stattdessen über XLX gekoppelt (Port 10002). Das Protokollfeld gibt es nur bei DCS-Einträgen.

XLX- und DCS-Kopplungen können mit einem eigenen Rufzeichen auftreten (z.B. als XLX363 statt URF363):

```ini
# urfd.ini
[XLXPeer]
Enable = true               # required for XLX peers and DCS peers with XLX protocol
Port = 10002
# Callsign = XLX363         # optional: callsign for XLX peering

[DCS]
Port = 30051
# Callsign = DCS363         # optional: callsign for native DCS peering

[M17]
# Callsign = M17-XXX        # required for mrefd peers: mrefd only accepts M17-XXX designators; must be unique, it is published in the Ham-DHT

[URF]
# InterlinkVersion = 4      # 4 (default) or 3 for legacy mode, see URF interlink 4 below
```

M17-Einträge brauchen auf beiden Seiten dieselben Module, und die `[M17] Callsign` dieses Reflektors muss in der `mrefd.interlink` des mrefd stehen. Wie bei mrefd holt sich ein Eintrag nur mit den Modulen (`M17-XXX AB`) Adresse, Port und Legacy-Status aus dem Ham-DHT-Dokument des mrefd. urfd veröffentlicht dort seine `[M17] Callsign` im Format von mrefd, damit ein mrefd diesen Reflektor ebenso eintragen kann; die Kennung darf also kein anderer M17-Reflektor benutzen.

### Echo-Modul
Eingebaute Echo-/Papagei-Funktion. Kann jedem Modul zugeordnet werden: Das Audio wird aufgenommen und nach 3 Sekunden Stille wiedergegeben.

```ini
[Modules]
Modules = AFSZ

[Echo]
Enable = true
Module = Z
```

Mit `Enable = false` wird es abgeschaltet, ohne die Konfiguration zu entfernen.

M17-Clients erreichen das Echo-Modul von jedem verbundenen Modul aus, indem sie an `PARROT` (wie bei mrefd) oder `ECHO` senden. Die Wiedergabe ihrer eigenen Aufnahme bekommen sie dann auch auf ihrem verbundenen Modul. Ein Paket im Paketmodus (`M17P`) an `PARROT` oder `ECHO` geht wie bei mrefd direkt an den Absender zurück, adressiert an @ALL.

### M17-Modulwahl
Ein M17-Client wählt sein Modul beim Verbinden: Das `CONN`- (oder `LSTN`-)Paket enthält den Modulbuchstaben, und der Client hört dann dieses Modul. Zum Wechseln trennt man die Verbindung und verbindet sich mit dem anderen Buchstaben neu, z.B. `URF363 B` als Reflektor in Pi-Star/WPSD, über die Modulauswahl in DroidStar oder mvoice, oder `URF363 B` als Ziel am M17-Funkgerät. Eine neue Verbindung ersetzt die alte mit demselben Rufzeichen und derselben IP.

Wie bei mrefd geht eine Aussendung an das Modul, mit dem der Client verbunden ist, egal welches Ziel sie hat (`URF363 B`, `M17-XXX B` oder @ALL); Streams an die Clients sind an @ALL adressiert. Nur `PARROT` oder `ECHO` als Ziel erreicht das Echo-Modul (siehe Echo-Modul).

### M17 LSTN
Reine Hör-Clients (LSTN-Pakete) werden angenommen und auf dem gewünschten Modul eingetragen. Darüber prüfen Überwachungsdienste wie dvref.com, ob ein Reflektor erreichbar ist. Sprache und Paketdaten von Hör-Clients werden verworfen, wie bei mrefd.

### M17-Protokoll
- **TYPE-Feld**: Sprachframes werden im alten und im V#3-Format des TYPE-Felds der M17-Spezifikation erkannt; verschlüsselte Streams werden in beiden verworfen.
- **Paketmodus**: `M17P`-Pakete (z.B. SMS) werden gegen beide CRCs geprüft und unverändert an die anderen M17-Clients auf dem Modul des Absenders weitergeleitet; der Absender erscheint im Last-Heard.
- **URF-Kopplung 4**: Der Stream-Header an URF-Partner enthält die Art der M17-META-Daten und die CAN, eine GPS-Position bleibt also auch über gekoppelte Reflektoren eine Position. Die Version wird pro Partner ausgehandelt: Nur Partner, die selbst 4 melden, bekommen die 2 zusätzlichen Bytes, Partner mit 3 (andere urfd-Forks) bekommen den unveränderten alten Header. `[URF] InterlinkVersion = 3` erzwingt den Legacy-Modus: Der Reflektor meldet 3 und schickt die Erweiterung nie.
- **mrefd-Kopplung**: Module lassen sich mit mrefd-Reflektoren teilen, eingetragen als `M17-XXX <address> <modules> [port]` in `urfd.interlink` (mit angehängtem `L`, `M17-XXXL`, für ein altes mrefd < 1.0), oder als `M17-XXX <modules>` mit der Adresse aus dem Ham-DHT. urfd koppelt unter der M17-Kennung aus `[M17] Callsign = M17-XXX`; ohne sie ist die M17-Kopplung aus. Verbindungsaufbau, Keepalive, Sprache und Paketdaten folgen mrefd: Daten an ein mrefd >= 1.0 tragen das Modul als zusätzliches Byte, ein altes wird als `M17-XXX <Modul>` adressiert und bekommt keine Paketdaten. Wie bei mrefd wird ein Stream von einem mrefd nicht an ein anderes weitergegeben. Gekoppelte mrefds erscheinen als M17-Peers.
- **Rufzeichen**: Wie bei mrefd müssen ein `CONN`, ein `LSTN` (großzügiger) und die Quelle von Sprache und Paketdaten wie ein Rufzeichen aussehen, alles andere wird verworfen. Ein Client, der nach dem Verbinden auf die Blacklist kommt, bekommt ein `DISC` und wird entfernt.
- **Neu verbinden**: Ein `CONN`/`LSTN` ersetzt jede frühere Verbindung mit demselben Rufzeichen und derselben IP, ein Modulwechsel hinterlässt also keinen verwaisten Eintrag.
- **META**: Die META-Bytes erreichen andere M17-Clients mit einem TYPE, der angibt, was sie enthalten (Text, GPS oder ECD), und mit der CAN der Quelle, eine GPS-Position erscheint also nicht mehr als Text. Eine GPS-Position wird in einen Maidenhead-Locator umgerechnet und in der Last-Heard-Liste (mit Link auf OpenStreetMap) und in `/json/stations` angezeigt.

### Protokolle ein- und ausschalten
Jedes Protokoll lässt sich einzeln ein- und ausschalten. Standard ist `true` (wie bisher).

```ini
[DPlus]
Enable = false
Port = 20001

[P25]
Enable = false
Port = 41000
```

### Ausfallsicheres Laden der ID-Datenbanken
Die DMR-ID- und die YSF-TX/RX-Datenbank überstehen einen Ausfall ihrer Quelle.

```ini
[DMR ID DB]
Mode = http
FilePath = /usr/local/etc/urfd/dmrid.dat
# Comma separated: first entry is primary, the rest are tried in order
URL = https://radioid.net/static/user.csv, http://www.pistar.uk/downloads/DMRIds.dat, http://xlxapi.rlx.lu/api/exportdmr.php
```

**Mehrere Quellen**: `URL` nimmt eine durch Komma getrennte Liste. Die Quellen werden der Reihe nach versucht, bis eine antwortet; wird eine Ersatzquelle verwendet, steht das im Log.

**Trennzeichen**: Die Felder dürfen durch `;`, `,` oder Tabulator getrennt sein, die üblichen Anbieter funktionieren also unverändert: radioid.net-CSV (`id,call,first,last,…`), Pi-Star `DMRIds.dat` (Tabulator) und xlxapi (`id;call;`). xlxapi liefert keine Namen, der DMR-Talker-Alias wäre dann nur das Rufzeichen; deshalb steht es an letzter Stelle. Die Reihenfolge der Felder ist überall gleich; Zeilen, die nicht mit einer ID beginnen, etwa eine CSV-Kopfzeile, werden übersprungen.

**Download-Cache**: Jeder erfolgreiche Download wird in `<FilePath>.cache` gespiegelt. Antwortet beim Start keine Quelle, wird stattdessen der Cache geladen; die Datenbank ist also nie leer, nur weil ein Verzeichnis ausgefallen ist. Das ist getrennt von `FilePath`, das bei `Mode = both` die eigenen zusätzlichen Einträge des Betreibers enthält und nie überschrieben wird.

Für die YSF-TX/RX-Daten ist keine zweite Quelle bekannt, dort ist der Cache der einzige Ersatz. Fehlende YSF-Frequenzen betreffen nur die Frequenzangabe bei Wires-X.

### Erweiterte XML-Ausgabe
Die XML-Statusdatei enthält jetzt:

- **Reflektor-Angaben**: Rufzeichen, Land, Sponsor, Dashboard-URL, E-Mail
- **Modul-Konfiguration**: Beschreibung, Anzahl verbundener Knoten, Transcodierung, DMR+-TG-ID, YSF-DG-ID, NXDN-RAN, NXDN-Ziel-ID (nur wenn `ModuleDstIdBase` gesetzt ist)
- **Zuordnungen pro Modul**: Autolinks (YSF, NXDN, P25), TG-Zuordnungen (MMDVM, MMDVMClient, SvxReflector, SVXServer), D-Star-Client-Zuordnungen (DCSClient, DExtraClient, DPlusClient mit Verbindungsstatus), YSF-Client-Zuordnungen (YSFClient mit DG-ID)
- **Aktive Protokolle**: Name und Port jedes eingeschalteten Protokolls
- **Protokoll pro Station**: Über welches Protokoll eine Station gehört wurde (DCS, MMDVMClient, YSF usw.)
- **Name pro Station**: `<Name>` mit dem Vornamen aus der DMR-ID-Datenbank, sonst aus der NXDN-Datenbank, wenn das Rufzeichen darin steht
- **Auf Sendung**: `<OnAir>` in jedem Modul mit offenem Stream, mit dem sendenden Rufzeichen
- **Position pro Station**: `<Locator>`, `<Latitude>` und `<Longitude>` für Stationen, die eine M17-GPS-, eine D-Star-DPRS- oder eine YSF-GPS-Position gesendet haben
- **Kennzeichnung dynamischer TGs**: `<Dynamic>true</Dynamic>` und `<Remaining>` Sekunden für zur Laufzeit hinzugefügte TGs

Modulnamen werden einmal in `urfd.ini` eingetragen und stehen automatisch im Dashboard zur Verfügung.

Der JSON-Report enthält außerdem ein Array `DynamicTGs` mit Protokoll, TG, Modul und Restlaufzeit aller aktiven dynamischen Zuordnungen.

### Dashboard v2.14.2
In v2.9.0 neu gestaltet als Frontplatte eines Funkgeräts: Ein bernsteinfarbenes LCD zeigt, wer gerade sendet, die Module sind ein Tastenfeld, der Rest sind schlichte Tabellen. Davor: dunkles Design (v2.6.0), robust gegen Ausfälle des Verzeichnisses (v2.6.1), M17-Positionen in der Last-Heard-Liste (v2.7.0), Eintrag ins Verzeichnis an den Reflektor abgegeben (v2.8.0), Sicherheitskorrekturen (v2.8.1, v2.8.2), Korrekturen an Statusdatei und Admin-Socket (v2.8.3). Seit v2.10.0 steht der Vorname neben dem Rufzeichen, aus den ID-Datenbanken, die urfd lädt (ab urfd 4.12.0).

**Seiten:**
- **Last heard**: Das LCD zeigt die Station, die gerade sendet, mit Modul, Betriebsart, Weg und laufender Sendezeit, sonst gedimmt die zuletzt gehörte. Daneben eine Taste pro Modul: bernsteinfarben, wenn Knoten verbunden sind, mit roter Markierung, solange jemand sendet. Eine Taste schaltet LCD und Log auf ihr Modul; ohne Taste zeigt das LCD die zuletzt begonnene Aussendung und nennt die weiteren Module auf Sendung; der Rufzeichenfilter versteht Platzhalter (`DL*`). Beide Filter überstehen das Neuladen.
- **Modules**: Eine Tabelle mit einer Zeile pro Modul: Name, verbundene Knoten (aufklappbar), DMR+-TG, YSF-DG-ID, NXDN-RAN und -Ziel-ID, Transcoding und Brücken (statisch, und gestrichelt für die zur Laufzeit angelegten). Spalten ausgeschalteter Protokolle werden ausgeblendet. Darunter die Server-Protokolle mit ihren Ports; Client-Protokolle, die nach außen verbinden, fehlen. `?show=modulesd` aus alten Links landet hier.
- **Links**, **Peers**: Verbundene Knoten und Partner-Reflektoren, mit Verbindungsdauer und der IP, soweit `IPModus` es erlaubt.
- **Reflector list**: Das XLX-Verzeichnis mit Suche und 25 pro Seite.
- **QuadNet live**: D-Star-Routing-Verkehr aus dem OpenQuad-Feed, mit Suche.
- **Admin** (versteckt): Talkgroups mit Kerchunk, D-Star- und YSF-Links, SVX- und MMDVM-Nutzer und -Knoten, Transcoder, Protokoll-Sperren, Log. Ein Index zeigt nur die Abschnitte, die die laufenden Protokolle nutzen. Zugang über das Pi-Symbol in der Fußzeile oder `?show=admin`.

Jede Seite außer Last heard und Admin trägt oben eine schmale LCD-Zeile mit der laufenden Aussendung. Den Sendezustand liefert das Element `<OnAir>` der Statusdatei (ab urfd 4.11.0); mit einem älteren urfd zeigt das Dashboard stattdessen die zuletzt gehörte Station.

**Funktionen:**
- Helles und dunkles Design, folgt dem System, mit Umschalter in der Kopfleiste
- Schrift (Saira, OFL) vom Dashboard selbst ausgeliefert, keine Anfrage an Google Fonts
- Kein Bootstrap mehr; jQuery nur noch auf der Admin-Seite
- Inhalt lädt sich alle `PageRefreshDelay` ms an Ort und Stelle neu, bei einem Sprecherwechsel sofort, nicht während man in einem Feld tippt
- Locator von Stationen, die eine M17-GPS-, eine D-Star-DPRS- oder eine YSF-GPS-Position senden, mit Link auf OpenStreetMap
- MOTD-/Wartungsbanner über `$PageOptions['MOTD']` in config.inc.php
- Modulnamen aus dem urfd-XML (eine einzige Quelle)
- Kontakt-E-Mail gegen Bots verschleiert

**Robust gegen einen Ausfall des Verzeichnisses**: Das Dashboard hängt nicht davon ab, dass `xlxapi.rlx.lu` erreichbar ist. Die Reflektorliste hat ein festes Timeout und wird auf der Platte zwischengespeichert (`<CacheDir>/reflectorlist.cache`). Ein frischer Eintrag liefert die Seite ganz ohne Netzwerkzugriff, ein veralteter hält die Liste am Laufen, solange das Verzeichnis ausgefallen ist. Seit v2.8.0 trägt das Dashboard den Reflektor nicht mehr im Verzeichnis ein; das macht der Reflektor selbst, siehe [CallingHome](#xlx-verzeichnis-callinghome).

```php
// pgs/config.inc.php
$Directory['ServerURL'] = 'http://xlxapi.rlx.lu/api.php';
$Directory['Timeout']   = 5;                    // Seconds before giving up on the directory server
$Directory['CacheDir']  = '/var/lib/xlxd-ch';   // Reflector list cache
```

Ältere Konfigurationen ohne `$Directory` funktionieren weiter: Die Werte werden dann aus ihren `$CallingHome`-Einstellungen genommen.

### JSON-API
Nur lesende HTTP-Endpunkte unter `/json/`, ausgeliefert vom Dashboard:

| Endpunkt | Liefert |
|---|---|
| `/json/metadata` | Dashboard-Version, Reflektor-Rufzeichen und -Version, öffentliche IPv4/IPv6, Sysop-E-Mail |
| `/json/status` | Reflektor an/aus, Laufzeit, Zeitpunkte der letzten Aktualisierung |
| `/json/reflector` | Vollständiger Reflektor-Datensatz mit Modulen und Angaben |
| `/json/links` | Verbundene Knoten mit Rufzeichen, IP, Modul, Protokoll, Verbindungs- und Last-Heard-Zeit |
| `/json/peers` | Gekoppelte Partner-Reflektoren |
| `/json/stations` | Zuletzt gehörte Stationen mit Land und `name` (Vorname aus der DMR- oder NXDN-ID-Datenbank, leer wenn unbekannt); `locator`, `latitude` und `longitude`, wenn die Station eine M17-GPS-, eine D-Star-DPRS- oder eine YSF-GPS-Position gesendet hat |
| `/json/modulesinuse` | Module mit den Rufzeichen, die gerade darauf sind |

Endpunkte für Listen liefern `[]`, wenn sie leer sind (`/json/stations` liefert `{"stations":[]}`); ein unbekannter Pfad liefert 404.

**Anforderungen an den Webserver**: Die Endpunkte sind saubere Pfade, die in `json/index.php` verteilt werden; der Webserver muss `/json/<name>` also dorthin leiten.

- **nginx**: Erledigt die mitgelieferte `nginx-dashboard.conf` mit einem Block `location /json/`. Ohne ihn fällt das allgemeine `try_files` auf die `index.php` des Dashboards zurück, und jeder Endpunkt antwortet mit `200` und einer HTML-Seite.
- **Apache**: Verlässt sich auf `json/.htaccess`, die eine Standard-Installation von Debian ignoriert: `apache2.conf` setzt `AllowOverride None` für `/var/www/`, und `mod_rewrite` ist nicht aktiv. Ohne beides liefern die Endpunkte 404:

```bash
sudo a2enmod rewrite
# and in /etc/apache2/apache2.conf, for the <Directory /var/www/> block:
#   AllowOverride FileInfo
sudo systemctl restart apache2
```

### D-Star-Slow-Data für transcodierte Streams
Transcodierte Streams (DMR, YSF, SVX, M17, P25, USRP, Client-Protokolle -> D-Star) enthalten jetzt korrekte D-Star-Slow-Data:
- **Header**: Rufzeichen des Anrufers (MY), Reflektor-Rufzeichen (RPT1/RPT2)
- **Textnachricht**: der Vorname des Betreibers (wenn das Rufzeichen in der DMR- oder NXDN-ID-Datenbank steht) und die Quelle: `Max DMR TG26363` (seit 4.29.0; vorher kam der Name als zweiter Text, den viele Funkgeräte nie anzeigen)

Beispiele der Textnachricht pro Protokoll:

| Quelle | Slow-Data-Text |
|--------|---------------|
| MMDVMClient | `Max DMR TG26363` |
| SvxReflector | `Max SVX TG317424` |
| YSF (Server) | `YSF DG28` |
| NXDN | `NXDN RAN6` |
| YSFClient | `YSF DG15` |
| M17, P25, USRP, DMR+ | `M17`, `P25` usw. |

YSFClient hängt die DG-ID an, wenn eine eingestellt ist (sonst nur `YSF`). D-Star-Quellen, auch die D-Star-Client-Anbindungen, werden nicht nach D-Star transcodiert: ihre eigenen Slow Data gehen unverändert durch.

Die Textnachricht hat 20 Zeichen. Ist `<Name> <Quelle> TG<n>` länger, wandert die TG in einen zweiten Text, der sich etwa alle 5 Sekunden mit dem ersten abwechselt; ist es dann noch zu lang, wird der Name gekürzt, damit die Quelle ganz bleibt:

```
Max DMR TG262                                  (alles passt)
Maximilian DMR    <-> DMR TG263112              (die TG wechselt sich ab)
Bartholomae YSF DG15  <-> Bartholomaeus         (der Name gekürzt)
```

Header und Textnachricht wechseln mit jedem Superframe (etwa 420 ms):

```
[Header: N0CALL > URF999 S] → [Max DMR TG262] → [Header] → [Max DMR TG262] → ...
```

Die Namenssuche funktioniert für alle Protokolle: Jedes Rufzeichen mit Eintrag in der DMR-ID-Datenbank erscheint mit Namen auf D-Star-Funkgeräten.

## Einführung

Der URF Multi-protocol Gateway Reflector Server, **urfd**, ist Teil der Software für ein digitales Sprachnetz. Er unterstützt D-Star (DPlus, DCS, DExtra, G3), DMR (MMDVM, DMR+, MMDVMClient), M17, YSF, P25, NXDN, USRP (AllStar), SvxReflector (SvxLink-FM-Client) und SVXServer (SvxLink-FM-Server).

Ein zentraler Bestandteil ist der hybride Transcoder [tcd](tcd/) im Ordner `tcd/` dieses Repositorys; Reflektor und Transcoder stammen also immer aus derselben Version. Der Reflektor lässt sich auch ohne Transcoder bauen, dann hören Clients aber nur andere Clients mit demselben Codec.

Diese Version unterstützt den Betrieb mit IPv4 und IPv6 gleichzeitig (Dual-Stack).

## Betrieb mit Docker

Reflektor und Dashboard laufen in Docker, der Transcoder in Docker oder direkt mit systemd (setup.sh fragt), mit AMBE-Geräten oder in Software mit einem Codec-Plugin, auf demselben Rechner oder auf einem zweiten ([tcd in Docker](tcd/README.de.md#tcd-in-docker)). Details in [`docker/README.md`](docker/README.md).

### Schnellstart

`setup.sh` stellt ein paar Fragen, schreibt `urfd.ini`, die Dashboard-Konfiguration und `tcd.ini`, installiert bei Bedarf Docker und baut und startet alles. Ein erneuter Aufruf oder `sudo ./setup.sh update` aktualisiert und behält die Konfiguration. Die Schritt-für-Schritt-Anleitung steht in [docs/INSTALL.de.md](docs/INSTALL.de.md).

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd
sudo ./setup.sh
```

Von Hand, mit der Konfiguration in `/opt/urfd` (die Schritte davor stehen in [docs/INSTALL-ADVANCED.de.md](docs/INSTALL-ADVANCED.de.md#reflektor-in-docker)):

```bash
cd docker
docker compose build
docker compose up -d
```

Mit einem [Codec-Plugin](tcd/README.de.md#codec-plugins) für DMR braucht der Transcoder nur ein DVSI-Gerät, DMR/YSF läuft dann in Software.

### Aufbau des Containers

Im urfd-Container laufen zwei Dienste unter supervisord:
- **urfd**: der Reflektor
- **nginx + php-fpm**: Dashboard auf Port 8363, das Live-Dashboard unter `/live/` und der WebSocket von urfd unter `/ws`

### Konfigurationsdateien

Die gesamte Konfiguration liegt in `/opt/urfd/config/` (als Volume eingebunden):
- `urfd.ini`: Hauptkonfiguration des Reflektors (lässt sich per SIGHUP neu laden)
- `urfd.interlink`: Kopplung (URF-, XLX-, DCS- und M17-Partner, mit DNS-Unterstützung)
- `urfd.blacklist` / `urfd.whitelist`: Zugangskontrolle (wird per SIGHUP neu geladen)
- `urfd.terminal`: Konfiguration für G3 (Icom Terminal/AP-Modus). Optionen: `address <public-ip>` (hinter NAT nötig), `modules <letters>` (erlaubte Reflektor-Module), `alias <callsign>` (zusätzliches Reflektor-Rufzeichen, das von Terminals angenommen wird, z.B. um sowohl `URFxxx` als auch `XLXxxx` anzunehmen; mehrfach möglich). Wird bei Änderung über `stat()` neu geladen.

Dashboard-Konfiguration in `/opt/urfd/dashboard/config.inc.php` (in den Container eingebunden):
- `$PageOptions['MOTD']`: Wartungsbanner (auf allen Seiten)
- `$PageOptions['ContactEmail']`: Kontakt in der Fußzeile (verschleiert)
- `$Directory`: XLX-Verzeichnisserver und Cache für die Reflektorliste (der Eintrag ins Verzeichnis ist `[CallingHome]` in `urfd.ini`)

## Installation von Hand

Ohne Setup-Script: den Reflektor von Hand in Docker, den Reflektor ohne Docker (`reflector/build.sh`), das Dashboard auf einem eigenen Webserver und den Transcoder allein beschreibt [docs/INSTALL-ADVANCED.de.md](docs/INSTALL-ADVANCED.de.md).

```bash
sudo bash reflector/build.sh   # urfd ohne Docker, mit systemd-Unit
sudo bash tcd/build.sh         # tcd
```

## Verwaltungswerkzeuge

### setup.sh

Neben Installation und Update erledigt `setup.sh` die Alltagsarbeit am Reflektor (Docker-Container `urfd`) und am Transcoder (systemd-Dienst `tcd`), je nachdem, was auf dem Rechner läuft.

```bash
sudo ./setup.sh status              # was hier läuft, in welcher Version
sudo ./setup.sh restart [urfd|tcd]  # ebenso start, stop
sudo ./setup.sh logs [urfd|tcd]     # Log verfolgen (Strg-C beendet)
sudo ./setup.sh reload              # urfd liest seine Konfiguration neu: TG-Zuordnungen, Listen, Interlink (SIGHUP)
sudo ./setup.sh check               # urfd.ini mit inicheck prüfen
sudo ./setup.sh update              # pull, neu bauen, neu starten
```

Ohne `urfd` oder `tcd` wirken die Befehle auf alles Installierte. `reload` schickt das SIGHUP an urfd selbst, nicht an den Container: Dessen PID 1 ist supervisord, der bei SIGHUP alle Programme neu startet.

### inicheck

Prüft die Syntax von `urfd.ini` und die Querbezüge (z.B. müssen TG-Module in `[Modules]` stehen, transcodierte Protokolle brauchen einen Transcoder, das Echo-Modul muss ein eingerichtetes Modul sein).

```bash
cd reflector
make inicheck
./inicheck -q urfd.ini   # Errors and warnings only
./inicheck -n urfd.ini   # Also print public config keys
./inicheck -v urfd.ini   # Print all keys (including internal)
```

Exit-Code 0 = OK, 1 = Fehler gefunden. Warnungen (fehlende optionale Felder) führen nicht zu einem Exit-Code ungleich 0.

## Kopplung / Peering

In `urfd.interlink` werden vier Arten der Kopplung unterstützt. Koppeln dürfen nur die dort eingetragenen Reflektoren; ohne Einträge keiner (bis 4.3.2 durfte bei leerer Datei jeder Reflektor koppeln).

| Art | Präfix | Standard-Port | Protokoll | Voraussetzung |
|-----|--------|---------------|-----------|---------------|
| URF-Partner | `URF` | 10017 | URF nativ | - |
| XLX-Partner | `XLX` | 10002 | XLX | `[XLXPeer] Enable = true` |
| DCS-Partner | `DCS` | 30051 | DCS nativ | - |
| DCS-Partner (XLX) | `DCS` + `XLX` | 10002 | XLX | `[XLXPeer] Enable = true` |
| M17-Partner (mrefd) | `M17-` | 17000 | M17-Kopplung | `[M17] Callsign = M17-XXX` |
| M17-Partner (altes mrefd < 1.0) | `M17-…L` | 17000 | M17-Kopplung | `[M17] Callsign = M17-XXX` |

DNS-Namen werden beim Laden über `getaddrinfo()` aufgelöst. Beide Seiten müssen sich gegenseitig in ihrer Interlink-Datei eintragen. Kopplung über DHT (ohne IP-Adresse) ist für URF-Partner möglich, wenn OpenDHT eingeschaltet ist, nicht für M17-Partner.

URF-Partner handeln die Version der Kopplung aus: urfd meldet 4 und ergänzt im Stream-Header die Art der M17-META-Daten und die CAN für Partner, die ebenfalls 4 melden; Partner mit 3 (andere urfd-Forks) bekommen den alten Header. Mit `[URF] InterlinkVersion = 3` meldet dieser Reflektor 3 und schickt die Erweiterung nie.

## Firewall

Nur die Ports der eingeschalteten Protokolle öffnen. Alle sind eingehend; angegeben sind die Standardwerte, der Schlüssel in `urfd.ini` ändert sie.

Offene Ports finden Scanner aus dem Internet innerhalb von Stunden. urfd weist sie ab und loggt eine abgewiesene Verbindung (Transcoder, MMDVM- und SVXServer-Anmeldungen) pro Adresse und Art einmal in der Stunde; die übrigen dieser Stunde fasst eine Zeile zusammen, wenn sie vorbei ist.

| Port | Transport | Protokoll | Zweck | Schlüssel |
|------|-----------|-----------|-------|-----------|
| 80 | TCP | HTTP | Dashboard (direkte Installation) | – |
| 5300 | TCP + UDP | SVXServer | SvxLink-Knoten verbinden sich hierher | `[SVXServer] Port` |
| 8363 | TCP | HTTP | Dashboard (Docker) | `docker-compose.yml` |
| 8880 | UDP | DMR+ | DMR+-DMO-Modus | `[DMRPlus] Port` |
| 10002 | UDP | XLXPeer | XLX- und BrandMeister-Kopplung | `[XLXPeer] Port` |
| 10017 | UDP | URF | URF-Kopplung | `[URF] Port` |
| 10100 | TCP | Transcoder | tcd, nur wenn er auf einem anderen Rechner läuft | `[Transcoder] Port` |
| 12345, 12346 | UDP | G3 | Icom Terminal und Access-Point-Modus | fest |
| 17000 | UDP | M17 | M17-Clients und mrefd-Kopplung | `[M17] Port` |
| 20001 | UDP | DPlus | D-Star DPlus | `[DPlus] Port` |
| 30001 | UDP | DExtra | D-Star DExtra | `[DExtra] Port` |
| 30051 | UDP | DCS | D-Star DCS | `[DCS] Port` |
| 34000 | UDP | USRP | AllStar-Knoten (urfd sendet an den `TxPort` des Knotens, 32000) | `[USRP] RxPort` |
| 40000 | UDP | G3 | Icom-Terminal-Sprache | fest |
| 41000 | UDP | P25 | P25 | `[P25] Port` |
| 41400 | UDP | NXDN | NXDN | `[NXDN] Port` |
| 42000 | UDP | YSF | YSF und Wires-X | `[YSF] Port` |
| 62030 | UDP | MMDVM | DMR-Hotspots und -Relais | `[MMDVM] Port` |

Nur lokal, nicht öffnen: der Admin-Socket (`[Admin] Port`, 10101) und der WebSocket (`[WebSocket] Port`, 10102, erreichbar über `/ws` des Dashboards).

Nur ausgehend, nichts zu öffnen: SvxReflector (zum Port des SvxLink-Servers, meist 5300), MMDVMClient (zum Master, `MasterPort`), die DCS-, DExtra-, DPlus- und YSF-Clients und die Anmeldung beim XLX-Verzeichnis.

## YSF

URF arbeitet als YSF-Master und stellt Wires-X-Räume bereit (einen pro Modul). YSF-Nutzer verbinden sich über Hotspot-Software (Pi-Star/WPSD), die URF-Reflektoren über die XLX-API findet. Eine Registrierung bei ysfreflector.de ist nicht nötig. Mit `EnableDGID = true` können Nutzer das Modul über DG-ID-Werte wechseln (10=A, 11=B, ... 35=Z), und der Reflektor sendet jedes Modul mit seiner DG-ID, sodass Funkgeräte, die danach filtern, das Modul hören. DN und VW (seit 4.31.0) werden angenommen: VW, IMBE wie bei P25, geht als P25 durch den Transcoder und auf Modulen ohne Transcoder als VW an YSF.

## Änderungen gegenüber dem Original

### Protokoll-Anbindungen
- **YSF-Positionen von verbundenen YSF-Reflektoren (4.31.1)**: Die YSF-Client-Anbindung (die Verbindungen von urfd zu anderen YSF-Reflektoren) liest die GPS-Position im DN-Datenkanal ebenfalls, sodass Stationen auf einem verbundenen YSF-Reflektor ihren Locator in der Liste der zuletzt Gehörten bekommen, wie am eigenen YSF-Port von urfd. Ein Durchgang, der ohne seinen Kopf aufgenommen wird (Late Entry), wird bei beiden neu gelesen; vorher behielt er den Zustand des vorigen Durchgangs und zeigte keine Position
- **Ohne DMR-ID kein DMR und P25 (4.31.0)**: Ein Sprecher ohne DMR-ID (in keiner ID-Datenbank registriert: AllStar, ein D-Star- oder M17-Rufzeichen) ging an die Hotspots an `[MMDVM]`, an DMR+ und an P25 mit Quell-ID 0; jetzt wird er für sie verworfen, außer `[MMDVM] FallbackDmrId` ist gesetzt, wie es MMDVMClient und NXDN schon tun. Nur Sprecher mit ID erreichen also DMR, NXDN und P25. Im Log steht `dropping outbound stream from … - no DMR ID`
- **YSF-Positionen (4.31.0)**: Die GPS-Position, die Yaesu-Geräte im Datenkanal senden (DN: DT1/DT2, V/D Mode 1: DT1 bis DT5; Kurz- und Langform, gegen ihre Prüfsumme geprüft, wie YSFGateway sie liest), erscheint als Locator in der Liste der zuletzt Gehörten, im XML und in `/json/stations`, wie D-Star- und M17-Positionen, veröffentlicht als Mitte ihres Locator-Feldes
- **YSF VW (4.31.0)**: Ein Yaesu-Gerät im VW-Modus (Voice Wide, IMBE-Vollrate) wurde kommentarlos verworfen; urfd nimmt VW jetzt an. Sein IMBE ist der Codec von P25, deshalb macht der Transcoder daraus D-Star, DMR/YSF/NXDN, M17 und die anderen wie aus P25 (mit OP25 oder einem Codec-Plugin, `CodecP25`), P25-Hörer bekommen genau diese Frames, YSF-Hörer auf einem transcodierten Modul DN. Auf einem Modul ohne Transcoder geht ein IMBE-Strom (VW oder P25) als VW an YSF (sein Kopf sagt IMBE), mit der Gerätenummer des sendenden Funkgeräts. Das Dashboard zeigt den Sprecher weiter als YSF; der Monitor von tcd nennt den Codec IMBE (P25, YSF VW). V/D Mode 1 (DN mit doppeltem Datenanteil und den AMBE+2-Frames im DMR-Format, ohne den zusätzlichen Sprach-Fehlerschutz von DN) wird wie DN angenommen; Data FR ist keine Sprache (Wires-X nutzt es). Die Verbindungen von urfd zu anderen YSF-Reflektoren (YSF-Client-Anbindung) tragen noch kein VW. VW-Sprache kommt bei den anderen Betriebsarten etwa 85 ms später an als DN: Der erste VW-Frame nach dem Kopf trägt 40 ms Sprache
- **MMDVMClient: unbekannte DMR-IDs (4.30.5)**: Ein Ruf von einer DMR-ID, die die ID-Datenbank noch nicht hat (neue Registrierungen brauchen etwa einen Tag bis in die Exporte), zeigte das Rufzeichen des Connectors als Sprecher, als würde der Betreiber selbst senden. Jetzt steht dort N0CALL mit der DMR-ID, und sobald der Talker Alias, den BrandMeister mitschickt, angekommen ist (etwa eine Sekunde nach Beginn), zeigt das Dashboard schon in diesem Durchgang das Rufzeichen; die anderen Betriebsarten bekommen es ab dem nächsten Durchgang, bis die Datenbank die ID hat. Im Log steht `voice from unknown (ID …)`. DMR-Hotspots bekommen für N0CALL keinen Talker Alias
- **NXDN-Codec in der Transcoder-Tabelle (4.30.4)**: Der NXDN-Header meldete P25, deshalb zeigte die Admin-Seite einen NXDN-Stream auf einem transcodierten Modul als P25; jetzt steht dort DMR (AMBE+2). Der Ton war nicht betroffen
- **Version ohne `-dht` (4.30.3)**: Die Version heißt `4.30.3` im Log, im XML, im XLX-Verzeichnis und im DHT, mit oder ohne OpenDHT gebaut; beim Start steht im Log, ob DHT eingebaut ist. `URFD_DHT=false` baut das Docker-Image ohne, wie `DHT = false` in `urfd.mk`
- **MMDVMClient-Frequenzen (4.30.1)**: Der Hotspot ist duplex (beide Zeitschlitze brauchen das), und BrandMeister lehnt einen Duplex-Hotspot mit RX = TX gleich nach seiner Konfiguration ab; die Vorgaben ohne `RxFreq`/`TxFreq` (beide 439,000 MHz) kamen nie hinein. Sie haben jetzt eine Ablage, urfd und inicheck warnen bei `RxFreq = TxFreq`, und eine abgelehnte Konfiguration wird als solche gemeldet
- **Protokoll-Korrekturen (4.30.0)**: Ein Stream endet mit seinem Endpaket statt 1,6 s später, ein abgelaufener Stream sendet in jedem Protokoll das normale Ende; das Echo spielt auch auf transcodierten Modulen ab; DMR TS1 und Dual-Slot funktionieren; DMR+ beschädigt eingehend nicht mehr jedes dritte AMBE-Frame und sendet ausgehend sein Endpaket; NXDN-Header und TX_REL realer Gateways werden erkannt, das Gesprächsende erreicht die Hörer und NXDN-Clients teilen sich keinen Stream mehr; P25 sendet seine TDU und Sprecher schneiden sich nicht mehr ab; das USRP-Unkey beendet den Stream und SET_INFO wird gelesen, wie chan_usrp es schreibt; die D-Star-Client-Anbindungen sprechen das zugeordnete entfernte Modul an und nehmen nur dessen Streams, Header-CRC und Slow-Data-Header stimmen; kein Deadlock beim ersten YSFClient-Stream auf ein transcodiertes Modul; P25 und NXDN sind wie die anderen Protokolle standardmäßig an; ein zweiter tcd für dieselben Module wird abgewiesen
- **MMDVMClient**: Vollständiger MMDVM-Client für BrandMeister-/DMR+-Master: mehrere TGs pro Modul (primär TX/RX, sekundär nur RX), Timeslots, Ersatz-DMR-ID, Erzeugung des Options-Strings; bis zu neun Verbindungen gleichzeitig (4.21.0) mit Brücken-Regeln je Netz
- **BrandMeister-API**: Optionale Anbindung an die REST API v2: Abgleich beim Start behält dynamische TGs (entfernt nur TGs, die weder in der Konfiguration noch in der Laufzeit-Zuordnung stehen), TGs per API statt per Kerchunk hinzufügen und entfernen, eingestellt über `BrandMeisterApiKey`
- **SvxReflector**: TCP/UDP-Client für SvxLink-FM-Server: OPUS-Codec, Audio in beide Richtungen, einstellbare RX/TX-Verstärkung, eine gewählte TG, die übrigen überwacht und bei Aktivität gewählt, Trennung bei TCP-Sendefehler mit sofortiger Neuverbindung
- **SVXServer**: SvxReflector-V2-Server: nimmt SvxLink-Knoten mit HMAC-SHA1-Anmeldung an, OPUS-Audio, TG-Auswahl und -Überwachung wie beim SvxReflector, Weiterleitung zwischen Knoten, Modulwahl über TGs, Anmeldung pro Nutzer, Nutzer-/TG-Verwaltung zur Laufzeit über die Admin-API
- **DCS-Kopplung**: Native DCS-Kopplung von Reflektor zu Reflektor, mit PeerCallsign und Protokollfeld in der Interlink-Datei
- **D-Star-Client-Protokolle**: DCSClient, DExtraClient, DPlusClient: Verbindung mit fremden D-Star-Reflektoren als Client-Knoten mit Modul-zu-Modul-Zuordnung, dynamisch über die Admin-API, Protokoll-Sperren, im Dashboard sichtbar
- **YSF-Client-Protokoll**: YSFClient: Verbindung mit fremden YSF-Reflektoren mit optionaler DG-ID, dynamische Zuordnung über die Admin-API, Reflektor-Rufzeichen im YSF-Feld "Radio"

### Verwaltung zur Laufzeit
- **Admin-Oberfläche**: JSON-over-TCP-Socket und Web-Dashboard: dynamische TG-Verwaltung, Protokolle neu verbinden, Transcoder-Statistik, Live-Log, Protokoll-Sperren, ein Thread pro Client mit 5 s Leerlauf-Timeout
- **Protokoll-Sperren zur Laufzeit**: Beliebige Protokoll-Paare in beide Richtungen im Admin-Dashboard sperren und freigeben: threadsicher mit shared_mutex, per Klick entfernbar, Rücksetzen auf die Konfiguration, wird nicht gespeichert (gilt nach Neustart nicht mehr)
- **Dynamische TG-Verwaltung**: TGs zur Laufzeit mit einstellbarer Laufzeit hinzufügen und entfernen, Timer über alle Protokolle hinweg erneuert, Aktivierung per BM-API oder Kerchunk, abgelaufene SVX-TGs fallen aus den überwachten TGs
- **Transcodierte Module von tcd (4.25.0)**: `[Transcoder] Modules = auto` transcodiert ein Modul, solange tcd dafür verbunden ist, nur noch die `tcd.ini` nennt also die Module; ein Stream auf einem Modul ohne tcd läuft wie auf einem Modul ohne Transcoding, Dashboard und DHT-Eintrag folgen
- **Name im D-Star-Text (4.29.0)**: Der D-Star-Slow-Data-Text lautet `<Vorname> <Quelle>` (`Max DMR TG262`, ohne „via“) in seinen 20 Zeichen, statt den Namen nur als zweiten Text zu zeigen, den viele Funkgeräte nie anzeigen; was nicht passt, wechselt sich ab. Der Name steht in ASCII (Umlaute ausgeschrieben), und der Text wird nicht mehr vom Thread des Protokolls geändert, während der Codec-Thread ihn sendet
- **tcd 0.12.0**: Codec-Plugin-Schnittstelle 2 (`TCD_CODEC_API 2`): `start` und `stop` sagen einem Plugin, wo eine Aussendung beginnt und endet, damit es jede Aussendung frisch beginnt, auch unter einer Stream-ID, die innerhalb von 30 s wiederkommt; der Monitor zählt einen Stream ab seinem letzten Frame nicht mehr. Plugins für Schnittstelle 1 müssen neu gebaut werden (tcd-mbelib-neo-plugin: `git pull`, `make clean && make`)
- **tcd 0.11.1**: Der Monitor liest ein Session-Cookie beliebiger Länge (hinter einem Reverse-Proxy kommen die übrigen Cookies der Seite mit), Fehlversuche älter als 10 Minuten zählen nicht, höchstens 8 tcdmon-Verbindungen warten auf ihre Passwortprüfung
- **TGs = auto für MMDVM-Server und SVXServer (4.29.0)**: `[MMDVM]` und `[SVXServer]` übernehmen die Modul-Talkgroups mit `TGs = auto` (und `[MMDVM]` mit `TS1` / `TS2` wie ein MMDVMClient), Hotspots und SvxLink-Knoten erreichen damit jedes Modul mit Talkgroup
- **Modul-Talkgroups (4.28.0)**: `[Modules] TG<M> = <nummer>` legt die Talkgroup eines Moduls einmal fest; eine MMDVMClient-Sektion bindet sie mit `TS1 = <module>` / `TS2 = <module>` ein, Plugins können sie übernehmen (`CConfigure::GetModuleTG`)
- **MMDVMClient-Verbindungen mit Namen (4.27.0)**: `[MMDVMClient.<name>]` statt `[MMDVMClient2]` ... `[MMDVMClient9]`, mit allem Weggelassenen aus `[MMDVMClient]`; eine `DmrId` aus ein oder zwei Ziffern (oder ein Name aus Ziffern) ist eine Endung an dessen `DmrId`; der Name steht im Log, auf der Admin-Seite, in `BlockProtocols` und bei den Admin-Befehlen
- **MMDVMClient sendet gleich nach dem Login (4.26.0)**: BrandMeister nimmt die Optionen ohne ACK an, deshalb wartete eine Verbindung auf dessen Beacon-Anfrage (Sekunde 18 einer Minute, bis zu einer Minute), bevor sie Ton zum Master schickte, und schickte die Optionen dabei alle 10 s neu; die Optionen gehen einmal raus und gelten nach 2 s ohne NAK als angenommen
- **BrandMeister-API-Abgleich wiederholt (4.26.0)**: Ein Abgleich der statischen TGs, der nicht durchging (eine neue Hotspot-ID kennt die API erst einige Minuten nach ihrem ersten Login, „Device not found“), wird nach 1 Minute wiederholt, danach seltener bis alle 30 Minuten, statt erst beim nächsten Start; das Log nennt die Verbindung
- **Standby-Transcoder (4.26.0, tcd 0.11.0)**: Ein tcd mit `StandbyModules` bekommt ein Modul nur, solange kein anderer tcd es hat, und gibt es zurück, sobald der wiederkommt; der Reflektor beantwortet seinen Handshake mit übernommen oder belegt und prüft jede Transcoder-Verbindung auf eine tote, nicht nur bis zum ersten Modul ohne Verbindung
- **Plugins mit urfd gebaut und aktualisiert**: `make plugins` / `make plugins-install` in `reflector/` bauen die Plugins in `plugins/<name>/` nativ, das Docker-Image nutzt dieselben Ziele; `setup.sh update` holt jedes Plugin-Verzeichnis, das ein git-Clone ist, auf den neuen Stand (auch Codec-Plugins in `tcd/plugins/`)
- **Protokoll-Plugins (4.24.0)**: Protokolle aus Shared Libraries, die beim Start geladen werden (`[Plugins]`), mit eigenem Ini-Abschnitt, Protokollnamen, Admin-Befehlen und Dashboard-Teilen, geprüft gegen das urfd, für das sie gebaut wurden
- **Zugangsdaten aus einer Datenbank**: Logins von MMDVM und SVXServer (und Plugins) aus SQLite, MySQL/MariaDB oder PostgreSQL neben der `urfd.ini`, LRU-Cache mit Laufzeit und Negativ-Cache, Logins aus dem Cache überstehen einen Datenbank-Ausfall, Flush und Status über die Admin-API
- **Neu laden per SIGHUP**: TG-Zuordnungen, Whitelist/Blacklist/Interlink und Transcoder-Module neu laden, ohne Verbindungen zu trennen: threadsicher über sigwait, bei Fehlern mit Rückgängigmachen

### Audio und Transcodierung
- **Verlorene Sprach-Frames aufgefüllt (4.32.0)**: D-Star nummeriert seine Sprach-Frames 0..20, DMR seine Bursts A..F mit je drei Frames; fehlen auf dem Weg zum Reflektor bis zu 6 Frames, setzt urfd an ihre Stelle Frames mit dem eigenen Verlust-Code des Vocoders (D-Star b0 127, AMBE+2 b0 120 als Erasure, mit FEC). Jeder Decoder, die Codec-Plugins von tcd, die DVSI-Chips und die Funkgeräte, wiederholt dafür den letzten Frame und schaltet nach dreien stumm, wie bei einem Frame, den er nicht korrigieren konnte, und der Stream behält seine Frame-Zahl, den D-Star-Sync und die DMR-Bursts; vorher hörten die Decoder den nächsten Frame zu früh und D-Star-Empfänger verloren die Sync-Lage. Die aufgefüllten Frames gehen gebündelt vor dem Frame hinaus, der die Lücke zeigte, die Verzögerung des Streams wächst also um die Lücke; ein Frame bis zu 6 Stellen zurück, der binnen 0,2 s kommt, ist verspätet oder wiederholt und wird verworfen, seine Stelle ging schon hinaus (tiefere Umordnung lässt sich von Verlust nicht unterscheiden). Längere Lücken bleiben, wie sie sind, XLX-Peers (D-Star und DMR in einem Frame) und die Echo-Wiedergabe werden nicht aufgefüllt, und das Transcoder-Protokoll ist unverändert. DMR+ nutzt eine Stream-ID pro Adresse: Ein neuer Durchgang wenige Frames nach einem verlorenen Endpaket schließt sich wie bisher dem alten Stream an, jetzt mit bis zu 6 aufgefüllten Frames dazwischen. Das Log meldet `Module A: 5 lost voice frames filled in`, wenn ein Stream endet. P25, NXDN, YSF, USRP und M17 tragen keinen Frame-Zähler, den urfd nutzen könnte (SVX verbirgt seine Lücken selbst)
- **NXDN-Modulwahl**: Modulwahl über die RAN (Radio Access Number): RAN 1-26 = Modul A-Z, mit getauschter RAN 1 und RAN des AutoLinkModule, sodass RAN 1 das AutoLinkModule erreicht; RAN 0 = AutoLinkModule. Alternativ Wahl über die Ziel-ID (`ModuleDstIdBase`, Ziel-ID base+1…base+26 = A-Z) für Clients ohne RAN, ausgewertet beim Verbindungs-Poll und bei jeder Aussendung, mit der Ziel-ID des Moduls in ausgehenden Frames. NXDN-ID-Auflösung über die NXDN-Datenbank mit DMR-Datenbank als Ersatz. FallbackNxdnId für unbekannte Anrufer (ohne Einstellung wird verworfen). Nicht initialisiertes `m_uiNXDNid` im Standardkonstruktor von CCallsign behoben.
- **D-Star-Slow-Data**: Transcodierte Streams enthalten Rufzeichen des Anrufers, Protokoll-/TG-/RAN-/Modulangabe und den Betreibernamen aus der DMR- und NXDN-ID-Datenbank, im Wechsel alle etwa 5 Sekunden. Angezeigt wird die tatsächliche Quell-TG (nicht die primäre/statische), damit es bei mehreren TGs stimmt. YSFClient zeigt die DG-ID
- **Getrennte SVX- und USRP-Codecs**: Unabhängige Codec-Pfade (`ECodecType::svx` und `ECodecType::usrp`) mit Verstärkung pro Anbindung in urfd.ini (`RxGain`/`TxGain` für SvxReflector, SVXServer und USRP); in tcd laufen beide durch die AGC analog
- **MMDVM Dual-Slot**: Jeder Timeslot (TS1/TS2) verbindet sich unabhängig mit seinem eigenen Reflektor-Modul, ein Hotspot kann also auf zwei Timeslots gleichzeitig zwei Module empfangen. Das Dashboard zeigt beide verbundenen Module pro Client.
- **MMDVM bei NAT-Neuzuordnung**: Verbindet sich ein Hotspot mit neuem UDP-Quellport neu (NAT), wird der alte Eintrag anhand der exakten DMR-ID ersetzt. Die Modulverbindungen der Slots bleiben erhalten. Mehrere Clients mit verschiedenen DMR-IDs (auch Endungen) hinter derselben IP werden unterstützt.
- **MMDVM Late-Entry**: Ermittelt die DMR-ID aus dem Rufzeichen des laufenden Streams (bevorzugt die zwischengespeicherte ID des Quell-Protokolls vor der Datenbank), damit lassen sich Sperren mitten im Stream aufheben
- **Kein Eigenecho**: MMDVMClient leitet nichts zu BrandMeister zurück, was von dort kam

### Dashboard und Ausgabe
- **Protokollnamen**: Einheitliche Namen in der Oberfläche: `MMDVM` = direkte Hotspot-Verbindungen, `MMDVMClient` = Verbindung zu BrandMeister/Master-Server (früher hießen beide je nach Stelle "MMDVM")
- **Dashboard v2.6.0**: Neugestaltung im dunklen Design, Admin-Panel, Modulübersicht mit TG-Zuordnungen und NXDN-RAN, Spalten ausgeschalteter Protokolle ausgeblendet, Protokollliste, QuadNet-Live-Proxy, Reflektorliste mit Suche und Seitenaufteilung
- **Dashboard v2.6.1**: Übersteht einen Ausfall des XLX-Verzeichnisses. Feste Timeouts bei jeder ausgehenden Anfrage, CallingHome-Fehler werden ins Log geschrieben, statt die Seite abzubrechen, Reflektor- und Peer-Listen aus einem Cache auf der Platte, Admin-API nicht mehr hinter der PHP-Session-Sperre aufgereiht
- **Dashboard v2.7.0**: Locator-Spalte für Stationen, die eine M17-GPS-Position senden, auch in `/json/stations`
- **Dashboard v2.14.2**: die Version im Kopf ohne `-dht`, damit der Status in der Kopfzeile bleibt
- **Dashboard v2.14.1**: die Seite Links zeigt ein Band nur bei D-Star-Links, wo der Suffix der Port-Buchstabe des Repeaters ist; sonst ist er das Modul, und ein Knoten auf Modul D erschien als Dongle
- **Dashboard v2.14.0**: Admin-Seite: die Abschnitte geladener Plugins, gezeichnet aus der Beschreibung, die jedes Plugin in `status` liefert (ab urfd 4.24.0); die Protokoll-Liste zeigt die Beschreibung eines Plugins. Seit urfd 4.26.0 verteilen sich mehr als 8 Module gleichmäßig auf höchstens 5 Spalten (9 = 5 + 4, 12 = 4 x 3), jede Taste mit dem Namen ihres Moduls, und die Plugin-Abschnitte haben denselben Abstand zueinander wie die übrigen
- **Dashboard v2.13.0**: Admin-Seite: mehrere MMDVMClient-Verbindungen, jede beim Hinzufügen einer TG wählbar, Name und Netz in der TG-Tabelle, ein Reconnect-Button pro Verbindung (ab urfd 4.21.0); die Protokoll-Liste lässt MMDVMClient2 bis 9 wie die erste weg
- **Dashboard v2.12.3**: Admin-Seite: eine Karte „Credential database“ mit Backend, Verbindung, Cache-Zählern und einem Button, der den Cache leert (ab urfd 4.20.0); Nutzer aus der Datenbank stehen in den Nutzerlisten mit „(database)“
- **Dashboard v2.12.2**: Das Rufzeichen in den Links führt wie in der Last-Heard-Liste zu QRZ, nicht zu aprs.fi
- **Dashboard v2.12.1**: Die Tabelle der SVX-Nodes zeigt neben der gewählten TG die beobachteten TGs und seit wann der Node verbunden ist
- **Dashboard v2.12.0**: "also on" nennt höchstens drei Module und zählt den Rest (alle im Tooltip); mehr als 8 Module bekommen ein kompaktes Tastenfeld mit 7 Spalten, der Modulname steht im Tooltip
- **Dashboard v2.11.0**: Das LCD folgt der gewählten Modultaste (wer dort sendet oder zuletzt gehört wurde); ohne Auswahl zeigt es die zuletzt begonnene Aussendung, und "also on S" nennt die weiteren Module auf Sendung, im LCD und in der Sendezeile
- **Dashboard v2.10.0**: Vorname neben dem Rufzeichen im LCD, in der Last-Heard-Liste, in der Sendezeile und in `/json/stations`; ein Sprecherwechsel erscheint nach etwa zwei Sekunden (eine kleine Abfrage, `api/onair.php`, läuft alle 1,5 s und stößt das Neuladen an); ein kleineres LCD; der Theme-Umschalter hält die Kopfleiste in einer Zeile
- **tcd 0.7.0: Version im Monitor**: Der tcd-Monitor zeigt die laufende tcd-Version in der Kopfleiste, wie das Dashboard
- **tcd 0.6.0 – 0.7.2: DMR-Pegel im Bitstrom für andere Codecs**: `OutputGainDMRBitstream` wendet einen Teil von `OutputGainDMR` bei Ton aus D-Star, M17, P25, USRP und SVX über b2 nach dem AMBE2+-Encoder an, der Encoder bekommt also mehr Pegel; seit tcd 0.7.0 eine Aufteilung in dB (alles hinterließ Artefakte auf leisen Silben), seit 0.7.1 ohne Knacken, seit 0.7.2 standardmäßig -6 dB (0 = aus), live im tcd-Monitor
- **tcd 0.5.0: Rauschunterdrückung für analoge Eingänge**: Ton von SvxLink und AllStar läuft vor der AGC analog durch den speexdsp-Präprozessor (`AGCAnalogNoiseSuppress`, -10 dB, der Gate floor der AGC analog jetzt bei 0), das FM-Rauschen unter der Sprache klingt nach dem AMBE-Encoder auf DMR also nicht mehr roboterhaft; siehe [tcd/README.de.md](tcd/README.de.md#agc)
- **tcd 0.9.0: sprachgesteuerte AGC**: Der Pegel folgt der oberen Hüllkurve der Sprache und steigt nur auf deutlich stimmhafter Sprache weit über einem pro Stream gemessenen Rauschboden, die Verstärkung pumpt also nicht mehr bei Atem, Rauschen oder in Pausen; auf FM-Funknetz-Verkehr bewegt sich die Verstärkung nur ein Drittel so stark, und die Sprache steht 2,5 dB weiter über dem Rauschen, bei gleichem Pegel und gleicher Streuung zwischen den Stationen; siehe [tcd/README.de.md](tcd/README.de.md#agc)
- **tcd 0.10: Codec-Plugins**: D-Star und DMR/YSF in Software aus Shared Libraries (`CodecPlugin` in der `tcd.ini`), mit oder ohne AMBE-Sticks, der Vocoder im Monitor zur Laufzeit umschaltbar; der MD380 ist weg, IMBE gehört zum Quelltext, der Monitor läuft auf libwebsockets und bekommt eine kompakte Ansicht für viele Module, und tcd kann ohne Sticks in Docker laufen; siehe [tcd/README.de.md](tcd/README.de.md#codec-plugins)
- **SVXServer-UDP-Heartbeat (4.12.5)**: urfd schickt einem Node einen UDP-Heartbeat, wenn es ihm 15 s nichts geschickt hat, wie SvxReflector; bisher hat es nur auf die Heartbeats des Nodes geantwortet, und SvxLink sendet keine, während es spricht, so dass es nach 60 s Sendung die Verbindung trennte ("UDP Heartbeat timeout")
- **SVXServer-Nodes in den Links (4.12.4)**: Ein SvxLink-Node ist auf dem Modul seiner gewählten TG verlinkt und, solange er keine gewählt hat (SvxLink wartet auf TG 0), auf dem Modul der ersten beobachteten TG mit Mapping; er erscheint also in den Links wie ein MMDVM-Hotspot, der Eintrag folgt Änderungen am TG-Mapping innerhalb von 5 s
- **SVXServer-Nodeliste (4.12.3)**: Die Node-Info wird mit den Keys gelesen, die SvxLink wirklich schickt (`sw` + `swVer`, `nodeLocation`/`Location`, `Locator`, die `qth`-Standorte aus `node_info.json`), Software und Standort sind also nicht mehr leer; `svxs_peer_list` liefert zusätzlich `monitored_tgs` und `connected_since`
- **Knotenzahl je Modul ohne Links (4.12.2)**: `<LinkedNodes>` zählt nur noch die Nodes, die auch im Status-File stehen; ein Link des Reflectors zu einem anderen Server (MMDVMClient, DCS-, DExtra-, DPlus- oder YSF-Client, DCS-Peer) zählt nicht mehr auf jedem Modul mit, auf das er gemappt ist
- **Sauberes Beenden der Transcoder-Verbindung (4.12.1)**: Beim Stoppen von urfd füllt sich das Log nicht mehr mit `POLLNVAL received on module`; ein ungültig gewordener Socket wird verworfen, die Socket-Liste wird den laufenden Codec-Threads nicht mehr unter den Füßen geleert, und ein Fehler oder Verbindungsabbruch ohne Daten wird nicht mehr als Paket weitergereicht
- **Namen in der Statusdatei (4.12.0)**: `<Name>` pro Station aus der DMR-ID-Datenbank (NXDN als Rückfall), nachgeschlagen auf einer Kopie der Liste, damit ein Neuladen der Datenbank nie die Last-Heard-Sperre aufhält; die Statusdatei wird gleich nach dem Öffnen oder Schließen eines Streams oder dem Hören einer Station geschrieben, nicht nur alle 10 s
- **Dashboard v2.9.0**: Neugestaltung als Frontplatte eines Funkgeräts: LCD mit der Station auf Sendung, Modul-Tastenfeld, zusammengelegte Modules-Seite, helles und dunkles Design, Schrift lokal, Bootstrap entfernt
- **Sendezustand in der Statusdatei (4.11.0)**: Jedes Modul mit offenem Stream trägt `<OnAir>` mit dem sendenden Rufzeichen, sodass das Dashboard TX vom Reflektor erfährt, statt es aus der Last-Heard-Zeit zu raten
- **Dashboard v2.8.3**: Ein Seitenaufruf bricht bei leerer Statusdatei nicht mehr ab; Admin-Antworten über 8 KB (Nutzer- und Peer-Listen, das Log) kommen vollständig an statt als "No response from admin socket"; `&`, `<` und `>` in Rufzeichen und Beschreibungen erscheinen als solche
- **Dashboard v2.8.2**: Das Admin-Passwort steht nicht mehr in `config.inc.php` (`setup.sh update` entfernt alte Kopien); die Admin-Seite meldet sich mit dem Passwort aus `urfd.ini` an
- **Dashboard v2.8.1**: Alles, was von außen kommt, wird escapet (der Parameter `show`, die Reflektorliste aus dem XLX-Verzeichnis, Hotspot- und Knotendaten auf der Admin-Seite, Rufzeichen und Filter); die Admin-API nimmt nur Anfragen der eigenen Seite an (JSON und `X-Requested-With`, kein CORS), ihr Session-Cookie ist `HttpOnly` und `SameSite=Strict` und wird beim Anmelden erneuert
- **Dashboard v2.8.0**: Kein CallingHome mehr, der Reflektor trägt sich selbst ein; die Reflektorliste liest `$Directory` (mit Rückfall auf alte `$CallingHome`-Einstellungen); `/json/metadata` nimmt die Dashboard-URL aus dem XML des Reflektors
- **Erweitertes XML/JSON**: Modul-Zuordnungen (statisch und dynamisch mit Laufzeit), Reflektor-Angaben, Protokollliste, Protokoll pro Nutzer, Array der dynamischen TGs
- **Anzeige der Protokoll-Sperren**: Aktive Sperren als anklickbare Markierungen im Admin-Panel
- **WebSocket (4.13.0)**: Live-Ereignisse für das Dashboard (Clients, Peers, sprechende Stationen, beim Verbinden der komplette Stand) auf `127.0.0.1:10102`, von nginx als `/ws` durchgereicht; basiert auf dem Notifier von Jason Kendall (coolacid), mit den Fixes aus dem Review (lws-Aufrufe nur in seinem Service-Thread, Verbindungen werden wirklich geschlossen, kein Log jedes Ereignisses), siehe [docs/WEBSOCKET.md](docs/WEBSOCKET.md); `setup.sh` schaltet ihn ein, das Docker-Image liefert `dashboard-ws` unter `/live/` aus
- **WebSocket: Bridges und Namen (4.14.0)**: `initial_connect` enthält die Bridges jedes Moduls (TG-Mappings, Autolinks, Connectoren zu anderen Reflektoren), ein `module_mappings`-Ereignis folgt ihren Änderungen, und Stations-Ereignisse, Last Heard und offene Kanäle tragen den Namen des Funkers aus der ID-Datenbank; die XML-Datei nimmt ihre Bridges aus demselben Code (Issue #2)
- **WebSocket-Links nur echte Nodes (4.14.1)**: `links`, `client_added` und `client_removed` lassen die Pseudo-Clients der Connectoren und Bridges (MMDVMClient, SvxReflector, DCS/DExtra/DPlus/YSF-Clients, USRP) und die Clients gekoppelter Peers weg, wie die XML-Datei; die SvxReflector- und USRP-Clients, die urfd selbst anlegt, erscheinen auch in der XML-Datei nicht mehr als verlinkte Nodes
- **SVXServer-Node doppelt gelistet (4.14.3)**: der Client eines Nodes, angelegt bevor seine UDP-Adresse bekannt war, blieb neben dem mit Adresse in den Links stehen, und die Ausgabe seiner leeren Adresse protokollierte "CIp::GetAddress: unknown socket family=0"; ein Node bekommt seinen Client erst, wenn die UDP-Adresse bekannt ist, und der Server hält ihn über einen Zeiger
- **M17-Streams an @ALL (4.14.4)**: Clients, die ihre Sprache an das Broadcast-Ziel @ALL (FF FF FF FF FF FF) statt an den Reflektor senden, wurden als "Unknown M17 packet" verworfen, mit "Callsign code is too large" bei jedem Frame; so ein Stream geht jetzt auf das Modul, auf dem der Client verlinkt ist
- **M17 wie bei mrefd (4.15.0)**: mrefd-Kopplungen über das Ham-DHT (`M17-XXX AB` ohne Adresse, urfd veröffentlicht seine `[M17] Callsign` als mrefd-Dokument); Packet Mode im Last-Heard, und `PARROT`/`ECHO` schickt ein Paket direkt an den Absender zurück; ein Client spricht unabhängig vom Ziel auf dem Modul, auf dem er verlinkt ist, und bekommt die Streams an @ALL adressiert; die Rufzeichenprüfung von mrefd für CONN, LSTN und die Datenquelle; ein nach dem Verlinken gesperrter Client wird getrennt; ein NACK eines mrefd wird geloggt statt gedumpt, und abgelehnte Stream-Frames erzeugen keinen Dump pro Frame mehr

### Transcoder (tcd)
- **tcd 0.15.0**: Codec-Plugin-Schnittstelle 3 (`TCD_CODEC_API 3`): `decode_info` meldet, wie jeder Frame ausgefallen ist (von der FEC korrigierte Bitfehler, wiederholt, stummgeschaltet, Ton, Stille), und der Monitor zählt die Frames mit Bitfehlern, die wiederholten und die stummgeschalteten je Codec-Plugin (Vocoder-Tabelle und `/api/stats`: `error_frames`, `repeats`, `mutes`); `frame_format` gibt die Frame-Größe eines Codecs an, und tcd lehnt ein Plugin ab, dessen Frames es nicht tragen kann. tcd lädt Plugins der Schnittstellen 2 und 3, ältere Plugins laufen ohne die Zähler weiter. Der Monitor hat Platz für 16 Software-Vocoder (vorher 8). tcd-mbelib-neo-plugin 1.1.0 hat Schnittstelle 3
- **tcd 0.14.1**: Die eigenen Vocoder von tcd sind Zeilen der Vocoder-Tabelle im Monitor neben den Geräten und Codec-Plugins: OP25 (P25, YSF VW) und Codec2 (M17), mit ihren Zählern für Kodieren und Dekodieren und dem Modul, für das sie arbeiten; die Tabelle zeigt also auch, wer P25 macht, wenn es auf OP25 steht (auch in tcdmon, `"builtin": true` in `/api/stats`)
- **tcd 0.14.0**: P25 über ein Codec-Plugin als Wahl neben dem eigenen OP25-Vocoder von tcd: `CodecP25 = op25` (Standard) oder der Name eines Plugins, umschaltbar im Web-Dashboard und mit `POST /api/codec` wie D-Star und DMR, „Speichern“ schreibt es; die Plugin-API bleibt `TCD_CODEC_API 2` (`TCD_CODEC_P25`, 11-Byte-IMBE-Frames), ältere Plugins laufen weiter. tcd-mbelib-neo-plugin 1.0.3 macht P25 mit mbelib-neo (PESQ etwa 0,1 über OP25 auf 19 Clips, vor allem vom Encoder)
- **tcd 0.13.2**: Ohne AMBE-Geräte (nur Codec-Plugins) meldet der Start nicht mehr „No USB serial devices found, is the AMBE device plugged in?“; das sagt „No AMBE device: D-Star and DMR in software“, und ein fehlendes Gerät wird weiter gemeldet wie bisher
- **tcd 0.13.1**: ein verlorenes AMBE-Gerät beendet tcd wieder (seit 0.12.4 meldete er "Lost the AMBE device" und lief ohne es weiter), und ein Gerät, das 3 s lang nichts beantwortet, gilt als verloren: der Dienst startet tcd ohne es neu, und ein Codec-Plugin oder ein Standby-tcd übernimmt; `OutputGainDMR = -14` in der Vorlage, der Pegel der Sprache bei BrandMeister (-18 ließ DMR/YSF rund 4 dB darunter)
- **tcd 0.13.0**: ein IMBE-Encoder und -Decoder je Modul (zwei P25-Module gleichzeitig stören sich nicht mehr), ein 300-Hz-Hochpass vor Codec2 für P25-Audio, baut und läuft unter macOS, DV3000/DV3003 werden an der Produkt-ID des Chips erkannt, ein Gerät, das nicht antwortet, wird ausgelassen statt tcd zu beenden, exklusiver Zugriff auf die serielle Schnittstelle
- **In diesem Repository**: tcd liegt mit seiner Historie in `tcd/`; es baut gegen die gemeinsamen urfd-Quellen (`TCPacketDef.h`, `TCSocket.*`) aus `../reflector`, beide Seiten sprechen also immer dasselbe Protokoll
- **Serieller Zugriff ohne libftd2xx**: AMBE-Geräte über den Kernel-Treiber `ftdi_sio` und `/dev/serial/by-id` (POSIX-Anbindung von Jason Kendall), RTS/CTS-Flusskontrolle, USB-Latenz-Timer auf 1 ms; Geräte, die ein älterer libftd2xx-tcd vom Treiber gelöst hat, werden beim Start wieder gebunden; ein verlorenes Gerät startet tcd über systemd neu
- **Geräteauswahl**: `DeviceSerial` muss exakt passen: USB-Seriennummer, libftd2xx-Form mit Port-Buchstabe, Name unter by-id oder Gerätedatei; `tcd --list-devices` zeigt sie an
- **Mischbetrieb, Codec-Plugins, AGC und Verstärkung**: siehe [tcd/README.de.md](tcd/README.de.md)

### Infrastruktur
- **Ruhigeres Log bei Scannern (4.22.0)**: Eine abgewiesene Verbindung wird pro Adresse und Art einmal in der Stunde geloggt, der Rest in einer Zeile zusammengefasst
- **DMR-Talker-Alias in UTF-16 (4.10.2)**: Unser Alias ist UTF-16 wie der von BrandMeister, höchstens 13 Zeichen; Motorola-Funkgeräte (R7) zeigten den in ISO 8 Bit nicht an
- **MMDVMClient-Sprecherwechsel (4.10.2)**: Ein BrandMeister-Sprecher, der anfängt, während das Modul noch den vorigen Stream hält (der schließt kurz nach seinem letzten Frame), steigt ein, sobald das Modul frei ist, statt für den ganzen Ruf verworfen zu werden
- **Analoge Verstärkung in urfd (4.10.0)**: `[USRP] RxGain/TxGain` setzen die AllStar-Pegel in der `urfd.ini`, wie bei SVX; tcd 0.3.2 hat `UsrpGainIn`/`UsrpGainOut` entfernt, urfd regelt also pro Anbindung und tcd pro Codec
- **Software-Name am SvxReflector (4.9.0)**: `[SvxReflector] Software` setzt den Software-Namen in der Node-Info für Reflektoren, die einen bestimmten erwarten (Standard `urfd`); das JSON der Node-Info wird sauber maskiert
- **SvxLink (4.8.0)**: Der SvxReflector-Client wählt eine TG und überwacht die übrigen zugeordneten, wie ein SvxLink-Knoten (bisher wählte er jede zugeordnete TG, der Reflektor behielt nur die letzte, und Audio jedes Moduls landete dort); der SVX-Server führt pro Knoten eine gewählte TG, leitet Audio zwischen seinen Knoten weiter, schickt Sprecherbeginn und -ende nur an Knoten auf oder mit Überwachung der TG und bittet Knoten mit Protokoll 3 um einen Downgrade. Beide: OPUS-Pakete von 10 bis 60 ms, verlorene Pakete werden überbrückt, verspätete verworfen, ein verlorenes Flush kostet nicht mehr den nächsten Durchgang, die Verstärkung begrenzt statt überzulaufen, 8-kHz-Sprach-Encoder statt Hochrechnen, frischer Encoder pro Durchgang, TCP-Nachrichten in Stücken, keine Verklemmung zwischen Admin-Socket und Protokoll-Thread
- **Statusdatei und Admin-Socket (4.7.4)**: Die XML/JSON-Statusdatei wird daneben geschrieben und umbenannt, das Dashboard liest sie also nie leer oder halb geschrieben; Rufzeichen und Texte aus `urfd.ini` darin sind XML-maskiert; der Admin-Socket nimmt einen Befehl an, der auf mehrere TCP-Segmente verteilt ankommt; ein zweites SIGHUP während eines Konfigurations-Reloads beendet urfd nicht mehr
- **Hotspots im XLX-Modus (4.7.1, 4.7.2)**: Ein Modul, das per Private Call 4001-4026 verbunden wurde, wie es der XLX-Master-Modus von Pi-Star/WPSD (DMRGateway) tut, wird auf dem Slot bedient, auf dem verbunden wurde, und auf TG9, wie bei xlxd; das XLX-Netz von DMRGateway nimmt nichts anderes an. Ein Group Call an 4001-4026 behält seine TG (4.7.1 antwortete auch darauf auf TG9). Der Terminator eines Link-Rufs erzeugt keinen Orphaned Frame mehr
- **DMR Talker Alias (4.7.0)**: Rufe an Hotspots am MMDVM-Server tragen einen Talker Alias, Rufzeichen und Vorname aus der DMR-ID-Datenbank wie bei BrandMeister, oder den Alias, den die DMR-Quelle schickt (BrandMeister, das Funkgerät), sobald er vollständig angekommen ist; seit 4.10.1 nur in jedem zweiten Superframe, damit MMDVMHost in die anderen sein Voice-LC für Late Entry setzen kann
- **D-Star-Positionen (4.6.0)**: DPRS-Positionen aus den Slow Data von D-Star-Geräten (GPS-A, gegen seine CRC geprüft, oder NMEA `RMC`/`GGA`) erscheinen als Locator in der Liste der zuletzt Gehörten, im XML und in `/json/stations`, wie M17-GPS-Positionen; veröffentlicht wird bei beiden nur die Mitte des Locator-Feldes (etwa 4,6 × 2,3 km), nicht die genaue Position, die andere D-Star-Stationen auf dem Modul weiterhin mit den Slow Data bekommen; aus allen D-Star-Protokollen und von URF-Partnern
- **CallingHome ohne Hash-Datei (4.5.0)**: Der Reflektor erzeugt seinen Hash fürs Verzeichnis beim ersten Start und speichert ihn als `[CallingHome] Hash` in `urfd.ini`; keine Hash-Datei mehr, `HashFile` entfällt und `setup.sh` fasst den Hash nicht mehr an (Anregung von Jason, VE3YCA)
- **CallingHome-Hash in urfd.ini (4.4.0)**: `[CallingHome] Hash` setzt den Hash fürs Verzeichnis direkt (Anregung von Jason, VE3YCA)
- **Härtung (4.3.5)**: DPlus und DMR+ prüfen die Paketlänge vor dem Lesen; der JSON-Bericht übersteht Rufzeichen, die kein gültiges UTF-8 sind; unbekannte Pakete landen nur mit ihren ersten 64 Byte im Log; ein MMDVM-Login-Salt gehört zur DMR-ID und zur anfragenden Adresse, und ein Konfigurationspaket eines nicht angemeldeten Hotspots bekommt keine Antwort; `setup.sh` macht `urfd.ini` nur für root lesbar und gibt `xlxd-ch` dem Dashboard statt allen (`update` korrigiert bestehende Installationen)
- **Sicherheitskorrekturen (4.3.4)**: Der SVX-Server nimmt UDP nur von der Adresse der TCP-Verbindung des Knotens an (die Client-ID ist ein Zähler, jeder hätte als Knoten Audio senden oder dessen Audio übernehmen können), gibt einer Verbindung 30 s ab Aufbau für die Anmeldung und lässt höchstens 8 gleichzeitig warten; der SvxReflector-Client nimmt Audio nur von seinem Reflektor an; P25-Frames, die kürzer als ihre IMBE-Daten sind, werden verworfen (Bytes hinter dem Paket gingen als Audio hinaus); der Admin-Socket liest die SVX-Knoten aus einer Kopie; Docker hält höchstens 5 × 10 MB Log
- **Sicherheitskorrekturen (4.3.3)**: Koppeln dürfen nur die in `urfd.interlink` eingetragenen Reflektoren (bei leerer Datei durfte jeder über URF und XLX koppeln); der MMDVM-Client nimmt Pakete nur noch von seinem Master an und löst dessen Adresse bei jeder Anmeldung neu auf (vorher lenkte jedes beliebige Paket die BrandMeister-Anbindung um, samt dem Passwort-Hash der nächsten Anmeldung); Hotspot-Angaben werden nur für angemeldete Hotspots gespeichert und unter einer Sperre gelesen; der Admin-Socket lehnt nach einem falschen Passwort 2 s lang alle Anmeldungen ab, erzeugt 128-Bit-Token und stürzt bei einem fehlschlagenden Befehl nicht mehr ab; keine irreführende `MMDVMPort`-Fehlermeldung beim Start ohne `[MMDVM]`
- **CallingHome im Reflektor**: `[CallingHome]` in `urfd.ini` trägt den Reflektor mit denselben Daten wie bisher das Dashboard im XLX-Verzeichnis ein, auch ohne Dashboard
- **Sauberes Beenden**: urfd beendet sich bei SIGTERM in etwa 2 Sekunden. Vorher warteten die Router-Threads der Module ewig auf Pakete, Protokolle wurden gelöscht, während ihre Threads noch liefen (ein Absturz, der nur nicht auffiel, weil Docker urfd vorher abschoss), und die ID-Datenbank-Threads schliefen je bis zu 10 s; `docker stop` endete so immer mit einem Kill
- **Transcoder-Verbindung ohne Nagle-Verzögerung**: `TCP_NODELAY` an beiden Enden der Verbindung urfd–tcd. Vorher wartete ein transcodierter Frame auf das verzögerte ACK des Reflektors, das erst mit dem nächsten DMR-Bündel kam; ein Frame ist jetzt nach etwa 23 ms statt 60 ms zurück (URF363, am Reflektor gemessen)
- **Setup-Script**: `setup.sh` installiert und aktualisiert Reflektor und Transcoder, schreibt die Konfiguration aus den Vorlagen und prüft sie mit inicheck, und erledigt status, start, stop, restart, logs, reload und check; Docker Compose baut aus jedem Klon, Daten in `$URFD_DATA` (Standard `/opt/urfd`)
- **Echo-Modul**: Eingebauter Papagei, ein- und ausschaltbar
- **M17 LSTN**: Reine Hör-Clients für Überwachungsdienste, die nicht senden dürfen
- **mrefd-Kopplung**: Module mit M17-Reflektoren teilen (`M17-XXX` in `urfd.interlink`, `[M17] Callsign`), auch mit alten mrefd
- **URF-Kopplung 4**: Art der M17-META-Daten und CAN im URF-Stream-Header, pro Partner ausgehandelt mit Rückfall auf 3; `[URF] InterlinkVersion = 3` für den Legacy-Modus
- **M17-Kompatibilität**: V#3-TYPE-Feld, Weiterleitung im Paketmodus (SMS), `PARROT`/`ECHO` als Ziel für das Echo-Modul, neue Verbindung ersetzt die alte, META-Art und CAN bleiben erhalten, GPS-Position als Locator
- **Robuste Transcoder-Verbindung**: TCP-Keepalive, nicht blockierendes poll, Warteschlangen leeren bei Verbindungsabbruch, automatische Neuverbindung nach Netzausfällen
- **Wiederholung beim Datenbank-Download**: Wiederholung mit wachsender Pause, wenn das erste Laden scheitert (verhindert leere Datenbanken nach kurzen Fehlern)
- **Ersatzquellen für Datenbanken**: `URL` nimmt eine durch Komma getrennte Liste, der Reihe nach versucht; das Trennzeichen (`;`, `,`, Tabulator) wird pro Zeile erkannt, die üblichen Anbieter funktionieren also unverändert; jeder erfolgreiche Download wird in `<FilePath>.cache` gespiegelt und geladen, wenn keine Quelle antwortet
- **Dienst-Optimierungen**: Echtzeit-Scheduling und CPU-Priorität für Sprache; seit 4.7.3 mit `-O2` gebaut
- **Threadsicheres Logging**: Atomares cout über ostringstream
- **Threadsichere Datenbankabfragen**: DMR-/NXDN-ID-Abfragen in CodecStream halten während des Zugriffs die nötigen Sperren und vermeiden so Konflikte mit der Aktualisierung im Hintergrund
- **P25 aufgeräumt**: Totes festes `P25_MODULE_ID` entfernt, P25 verwendet durchgängig `AutoLinkModule`. Debug-Ausgaben abgesichert.
- **YSF-Korrekturen**: Prüfung auf WiresX-Pakete vor den Sprach-Parser verlegt (CONN_REQ-Datenframes wurden als Sprache verarbeitet). CONN_REQ nimmt Raum-IDs 4001-4026 und direkte Indizes 1-26 an (war fest 0). DX_RESP-Status `'3'` (kompatibel mit YSFGateway, war das undokumentierte `'5'`). ALL_RESP-Byte `0x26` (war `0x29`). Nullzeiger in `YSFPayload::getSource()` behoben (prüfte das falsche Element). Hinweis: Der Raumwechsel über WiresX braucht direkte YSF-Clients, MMDVMHost fängt CONN_REQ lokal ab. Die DG-ID ist der wichtigste Weg der Modulwahl.
- **DCS-Server-Korrektur**: "EEEE"-Statuspakete (35 Byte) von ircDDBGateway-Clients werden ignoriert, statt als unbekannt im Log zu landen. Die 9-Byte-Variante des Keepalive wird angenommen.
- **Übernommen von [dbehnke/urfd](https://github.com/dbehnke/urfd)**: Bereinigung von Rufzeichen, Behebung von YSF-Radio-ID-Kollisionen, Durchsetzung der Transcoder-Modul-ID, Modulwahl per DG-ID

## Copyright

- Copyright (c) 2016 Jean-Luc Deltombe LX3JL und Luc Engelmann LX1IQ
- Copyright (c) 2022 Doug McLain AD8DP und Thomas A. Early N7TAE
- Copyright (c) 2024 Thomas A. Early N7TAE
- Copyright (c) 2025-2026 Jens-Christian Merg DL4JC (Erweiterungen von urfd-ng)
- tcd: Copyright (c) 2021 Thomas A. Early N7TAE; POSIX-Anbindung von Jason Kendall (2026); Erweiterungen von urfd-ng Jens-Christian Merg DL4JC
