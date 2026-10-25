# urfd-ng - Universal Digital Voice Reflector and Transcoder

**English** | [Deutsch](README.de.md)

Based on [urfd](https://github.com/n7tae/urfd) by Thomas Early N7TAE, with extended features, the hybrid transcoder [tcd](tcd/) and a setup script that installs both.

**urfd 4.29.3-dht** | tcd 0.12.3 | Dashboard v2.14.1

**New to this? Install with the setup script, see [docs/INSTALL.md](docs/INSTALL.md):**

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd && cd urfd && sudo ./setup.sh
```

## What's New in urfd-ng

### Setup Script and Transcoder
`setup.sh` installs the reflector (Docker, with the dashboard) and the transcoder (systemd), on one machine or split across two, after a few questions; it also updates them. The transcoder lives in [`tcd/`](tcd/), so reflector and transcoder always come from the same version. tcd uses the AMBE sticks through the kernel's `ftdi_sio` driver with the USB latency timer at 1 ms, so it no longer needs the libftd2xx driver, which ftdichip.com no longer lets scripts download. A tcd on another machine logs in with a shared password since 4.16.0 (`[Transcoder] Password` in `urfd.ini`, `Password` in `tcd.ini`), so the transcoder port can face the internet; the setup script generates one. A connection that stalls in its handshake no longer holds up the reflector's status thread. Since tcd 0.10, [codec plugins](tcd/README.md#codec-plugins) bring D-Star and DMR/YSF in software, so tcd also works with one AMBE stick, with more modules than the sticks have channels, or with no stick at all. Since 4.25.0 `[Transcoder] Modules = auto` leaves the choice to tcd: a module is transcoded while tcd is connected for it, so the list lives in `tcd.ini` alone and a module added there needs no change and no restart of urfd. Since 4.26.0 a second tcd can stand by for modules (`StandbyModules`): it takes them over while their tcd is gone, e.g. a software tcd in Docker next to urfd for the one with the sticks. See [docs/INSTALL.md](docs/INSTALL.md) and [tcd/README.md](tcd/README.md).

### Even Levels Across All Modes
tcd's AGC brings every call to the same level, whatever radio or vocoder it came from, and it also covers DMR → DMR/YSF, where the AMBE2+ frames pass through without a vocoder: the AGC's gain goes into the frame's gain parameter (b2) instead of a decode and re-encode, so the audio stays as clean as the original. SvxLink and AllStar (SVX, USRP) go through their own AGC analog: FM audio is not silent between words but carries the receiver's noise, which the AGC for digital sources raised by up to 30 dB; 20 minutes of FM-Funknetz traffic kept 32 dB speech-to-noise with it instead of 19. Two DVSI devices, e.g. DV3000 + DV3003 for two modules, need no codec plugin; a Raspberry Pi 4 is plenty. See [tcd/README.md](tcd/README.md#dmrysf--dmrysf-level-dmrgainmode).

### XLX Directory Registration (CallingHome)
The reflector registers itself in the XLX directory, which lists it in Pi-Star, WPSD and other hotspot software. This used to be the dashboard's job; now it works without the dashboard too, and the dashboard no longer makes the request.

```ini
[CallingHome]
Enable = true
Country = Germany              # country name as shown in the directory
Comment = My URF Reflector     # max 100 characters
# Hash =                       # letters and digits; blank = made at first start
# OverrideIP =                 # blank = the directory uses the address it sees
# PushDelay = 300              # seconds between updates
```

It sends what the dashboard sent (name `XLXnnn`, uptime, `DashboardUrl` from `[Names]`, country, comment, version, interlinks). Nothing is sent unless `Enable = true`. The settings are read at startup.

The hash identifies the reflector in the directory, and the directory ties `XLXnnn` to the first hash it sees. The directory hands out no hashes: with `Hash` blank, the reflector makes one at its first start and saves it as `Hash = ...` in `[CallingHome]` of its `urfd.ini`, the only line it ever writes there. No hash file is used. If it cannot write `urfd.ini` (mounted read-only, say), it does not register and says so in the log. Keep the hash from then on.

#### Moving a reflector that is listed already

A reflector that was registered before, by an XLX dashboard or an older urfd, has to keep its hash, the directory would not accept a new one for the same name. Set it before the first start with `Enable = true`.

1. Check whether your name is in the directory (here `XLX363`, the number of your `URFnnn`):
   ```bash
   curl -s 'http://xlxapi.rlx.lu/api.php?do=GetReflectorList' | grep -A6 '<name>XLX363</name>'
   # without curl:
   wget -qO- 'http://xlxapi.rlx.lu/api.php?do=GetReflectorList' | grep -A6 '<name>XLX363</name>'
   ```
   Not listed: nothing to do, the reflector makes its hash itself. If the listed reflector isn't yours, choose another number.
2. Listed and yours: copy the hash from the old server. The file is `$CallingHome['HashFile']` in the old dashboard's `config.inc.php`:
   ```bash
   grep '\$Hash' /path/to/callinghome.php
   ```
   It can only be read there, not over the web: the dashboard runs the file as PHP and sends nothing.
3. Put it in `urfd.ini` and start:
   ```ini
   [CallingHome]
   Enable = true
   Hash = Ab3dE4fG5hIj6K7L
   ```
   `docker logs urfd | grep CallingHome` then shows `registered as XLXnnn`.

### MMDVMClient Connector
Connect to any DMR master server (e.g. BrandMeister) via the MMDVM protocol. Maps talkgroups to reflector modules with per-timeslot support. Multiple TGs can share a module: the first becomes the primary (TX/RX), additional TGs are secondary (inbound RX only). Dynamic TGs can be added at runtime via the Admin interface.

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
RxFreq = 430412500
TxFreq = 439812500
# FallbackDmrId = 1234567           # DMR ID for unknown callers (0 or omit = drop)
# BlockProtocols = SvxReflector,YSF # Block protocols bidirectionally
# Network = brandmeister.network    # default: brandmeister.network for BM masters, else the host (see "Several connections")
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

**Multi-TG per Module**: The first TG listed for a module becomes the primary and is used for outbound traffic (TX). Additional TGs on the same module are secondary and only receive inbound traffic (RX). Dynamic TGs added via the Admin interface are always secondary unless the module has no primary yet.

**FallbackDmrId**: When a callsign cannot be resolved to a DMR ID, this ID is used instead. If not configured or set to 0, the stream is dropped.

**BrandMeisterApiKey** (optional): When set, enables BrandMeister REST API v2 integration. Get your key from BrandMeister SelfCare -> Profile -> Security -> API Keys. Effects:
- **Startup sync**: Fetches BM's static TGs, removes orphans (TGs not in config), adds missing ones
- **Dynamic TG add**: Registers as static on BM via API (no kerchunk needed)
- **Dynamic TG remove**: Deletes from BM via API
- Without API key, falls back to kerchunk behavior as before

**Several connections** (4.21.0, named since 4.27.0): further sections next to `[MMDVMClient]`, at most 9 connections. Each is a hotspot of its own with two timeslots and its own TG map, so more modules can talk to BrandMeister at the same time (one call per timeslot and connection), or the reflector joins other networks (TGIF, FreeDMR, ...). Use a hotspot ID of its own per connection (e.g. your DMR ID with the suffixes `01`, `02`, ...); one BrandMeister hotspot password covers all of them. Keys a further section leaves out (`Latitude`, `Longitude`, `Location`, `Description`, `URL`, `RxFreq`, `TxFreq`) come from `[MMDVMClient]`: BrandMeister drops a hotspot that sends the bare defaults. A connection in the network of `[MMDVMClient]` without a `BrandMeisterApiKey` uses that one's key (it covers all hotspots of the account): BrandMeister ignores the TGs of the login options, only the API makes them static on the new hotspot.

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
TG26363 = F,TS1

[MMDVMClient.TGIF]                  # another network: its own master, password and ID
MasterAddress = tgif.network
Password = ...
DmrId = 123456790
TG31665 = T,TS2
```

**Module talkgroups** (4.28.0): `[Modules]` can give each module its talkgroup once, `TG<M> = <number>` (each number on one module). A connection then maps it with `TS1 = <modules>` and `TS2 = <modules>` instead of the TG number: `TS1 = R` is `TG2625 = R,TS1` with `TGR = 2625`. The TG lines keep working next to it (e.g. a second, RX only TG on a module). Plugins can take the talkgroups too (`CConfigure::GetModuleTG`).

```ini
[Modules]
Modules = ADFR
TGD = 262
TGF = 26363
TGR = 2625

[MMDVMClient.50]
TS1 = F
TS2 = D,R                           # several modules on one timeslot
```

**Named connections** (4.27.0): `[MMDVMClient.<name>]`, the name of letters, digits, `-` and `_`. A named connection takes what it leaves out from `[MMDVMClient]`: master, port, password, callsign, `FallbackDmrId`, `Software`, `Firmware`, the location keys, `Network` with the master, and in that network the `BrandMeisterApiKey`. So its section holds what is its own: its hotspot ID and its TGs (`LocalPort`, `BlockProtocols` and `Enable` stay per connection). Its `DmrId` of one or two digits is a suffix to the `DmrId` of `[MMDVMClient]`, a longer one the whole ID; without a `DmrId` a name of digits is the suffix (`[MMDVMClient.50]` is hotspot `<DmrId>50`). `[MMDVMClient]` with TGs of its own is a connection too, without them it only gives the shared settings. The name shows in the log, the admin page (TG table, reconnect buttons, protocol controls), `BlockProtocols` (`BlockProtocols = MMDVMClient.TGIF`) and the admin commands (`"client": "MMDVMClient.TGIF"`). The numbered `[MMDVMClient2]` to `[MMDVMClient9]` keep working, with all their keys; a file uses named or numbered sections, not both. Adding, removing or reordering connections needs a restart.

- **Network.** Every connection, `[MMDVMClient]` included, belongs to the network in its `Network` key. Without it every BrandMeister master (`*.brandmeister.network`) is `brandmeister.network`, any other master is a network of its own (its host name or address). Set `Network` for two masters of another network, or for a BrandMeister master given by its IP address.
- **Bridging.** A call never goes back to the connection it came from, nor to another connection in the same network (the network has it already). Between networks, a module that both map is bridged; `BlockProtocols = MMDVMClient3` switches that off for this pair.
- **Names.** In every `BlockProtocols` and in the admin `block`/`unblock` commands `MMDVMClient` means every connection, a connection's name (`MMDVMClient.TGIF`, or `MMDVMClient1` to `MMDVMClient9` for numbered ones; `MMDVMClient1` is `[MMDVMClient]`) one of them. With several connections the admin status lists them by these names.
- **Checks.** `inicheck` refuses a TG or a hotspot ID used by two connections of one network; the admin page refuses such a dynamic TG.
- **Modules without transcoding.** An MMDVMClient TG may sit on a module tcd does not transcode. Only AMBE+2 streams (DMR, YSF, NXDN) go to the master from there, judged by their first frame; D-Star, M17, SVX and the others stay local, they would reach the network as silence.
- **Admin.** `tg_add`, `tg_remove`, `reconnect` and `kerchunk` take `"client": 2` or `"client": "MMDVMClient.50"` (or the protocol `mmdvmclient2`); without it they mean `[MMDVMClient]`, for a TG the connection that has it, and `reconnect` all of them. `tg_list` and `status` list every connection with its network. The dashboard (v2.13.0) offers each connection when adding a TG and has a reconnect button for each.

### SvxReflector Client
Connect to SvxLink SvxReflector servers (e.g. FM-Funknetz) for bidirectional FM audio bridging. Uses TCP for signaling and UDP for OPUS-encoded audio. Requires transcoded modules. Dynamic TGs can be added at runtime via the Admin interface.

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

**BlockProtocols**: Supported on MMDVMClient, SvxReflector, SVXServer, DCSClient, DExtraClient, DPlusClient, and YSFClient. Prevents audio from the listed source protocols from being routed through this connector. Each client protocol also always blocks self-routing (e.g. DCSClient never routes DCSClient-originated audio back out). Available protocol names (one table for all of them since 4.20.0): `MMDVM`, `MMDVMClient` (every connection) or `MMDVMClient1` to `MMDVMClient9` (one), `SvxReflector`, `SVX` or `SVXServer`, `DExtra`, `DExtraClient`, `DPlus`, `DPlusClient`, `DCS`, `DCSClient`, `DMRPlus`, `YSF`, `YSFClient`, `M17`, `NXDN`, `P25`, `USRP`, `URF`, `XLXPeer`, `G3`, and the name of a loaded [plugin](#plugins). Comma-separated.

**One TG at a time**: An SvxReflector node has one selected TG; it gets the audio of that TG only, and its audio goes there. urfd selects the first static TG (the lowest number, the home TG) and monitors all mapped TGs (`MsgTgMonitor`, like SvxLink's `MONITOR_TGS`). A talker on a monitored TG selects that TG when urfd has had no traffic for 3 s; audio from a urfd module whose TG isn't selected selects it unless a talker is on the selected one, then that module's transmission is not sent. After 30 s without traffic urfd goes back to the home TG. With one TG mapped nothing changes.

**RxGain / TxGain**: Static gain applied to SVX audio, per connection like `[USRP] RxGain/TxGain`. RxGain is applied after OPUS decode before the transcoder, TxGain after the transcoder before OPUS encode; samples past full scale are clipped. tcd's AGC analog (its own gate at -40 dBFS, see [tcd/README.md](tcd/README.md#agc)) runs on SVX audio after RxGain, so keep RxGain near 0 and let the AGC set the level; a strongly negative RxGain puts quiet FM speech below the gate. Default is 0 dB for both.

**Software**: The software name urfd reports in its node info after the login (`sw`), `urfd` by default. Some reflectors expect a particular one; set it only in agreement with the reflector's operator. The value is sent as JSON-escaped text.

### SVXServer
Accept incoming connections from SvxLink nodes. Acts as a SvxReflector server (protocol V2, HMAC-SHA1 auth, OPUS audio). Requires transcoded modules. Users authenticate with callsign + password. Each node selects one TG with `MsgSelectTG`, mapped TGs lead to a module, and nodes on the same TG hear each other. Users and TGs can be managed at runtime via the Admin interface.

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

**User management**: Users can be added/removed at runtime via the Admin interface or configured statically in `urfd.ini`. Each user authenticates with callsign + password using HMAC-SHA1.

**TG mapping**: Modules must be transcoded. Multiple TGs per module supported. Since 4.29.0 `TGs = auto` maps every (transcoded) module with a talkgroup (`[Modules] TG<M>`); `TG` lines win. Dynamic TGs can be added via the Admin interface with configurable TTL.

**TG selection, like SvxReflector**: A node has one selected TG, `MsgSelectTG` replaces it; `MsgTgMonitor` sets the TGs whose talker start/stop it gets. A TG has one talker at a time. The other nodes on the TG get the talker's OPUS packets as they came (no transcoding), also on TGs that aren't mapped to a module; mapped TGs also go to and come from their urfd module. Nodes must speak protocol 2.0. SvxLink with an `AUTH_KEY` does, also current versions (tested with SvxLink 1.9.99.36 / 25.05, 2026-10-15); a node set up for certificates only uses protocol 3 (TLS), which urfd does not support: it is asked to downgrade and logs that it can't.

**Setting up a SvxLink node**, in `[ReflectorLogic]` (example for TG26363 on a urfd module):
```ini
HOSTS = urf.example.org:5300    # instead of DNS_DOMAIN
CALLSIGN = N0CALL-HS            # the user in [SVXServer]
AUTH_KEY = changeme             # its password
DEFAULT_TG = 26363              # selected on local traffic
MONITOR_TGS = 26363             # heard while idle, and the module urfd links the node on
```
- With `DEFAULT_TG` set, SvxLink mutes the first transmission after it fell back to TG 0 (`TG_SELECT_TIMEOUT`, 30 s by default): `MUTE_FIRST_TX_LOC` is on by default and keeps that transmission (no audio is sent at all) to suppress test keyups and DTMF commands. The second transmission goes through; `MUTE_FIRST_TX_LOC = 0` sends the first one too.
- Level: the node's receiver must not clip. A SHARI node (CM108) with `Mic Capture Volume` 23 of 35 clipped every transmission (speech at -6 dBFS, peaks at full scale), which sounds harsh after the AMBE encoder; at 15 the speech came at -16 dBFS without clipping (`amixer -c Device cset numid=8 15`, then `alsactl store`). Keep `RxGain` in urfd.ini at 0 and let tcd's AGC analog set the level.
- The FM noise under the speech turns robotic after the AMBE encoder on DMR; tcd since 0.5.0 removes most of it before encoding (`AGCAnalogNoiseSuppress`, see [tcd/README.md](tcd/README.md#agc)).
- The node shows in the links on the module of its selected TG, or while it has none (TG 0) on the one of its first mapped monitored TG, with the address urfd sees: a node in the reflector's own LAN shows its internal IP.

**Admin commands**:
```json
{"cmd": "svxs_user_add", "token": "...", "callsign": "N0CALL-HS", "password": "secret"}
{"cmd": "svxs_user_remove", "token": "...", "callsign": "N0CALL-HS"}
{"cmd": "svxs_user_list", "token": "..."}
{"cmd": "svxs_peer_list", "token": "..."}
```

### Plugins
A protocol can also come from a shared object that urfd loads at start, for one that is not part of this repository. `[Plugins]` names the files, each `Load` line one of them:

```ini
[Plugins]
Load = /usr/local/lib/urfd/myproto.so
```

A plugin is C++ built against this source tree (see [reflector/Plugin.h](reflector/Plugin.h)): its protocol is a `CProtocol` like the built-in ones and uses the classes and globals of urfd, which the `urfd` executable exports (it is linked with `-rdynamic`). It fits only the urfd it was built for: urfd compares the version, the build options (`DHT`, `debug`) and the size of its main classes and refuses a plugin built from other sources, so rebuild the plugins with every urfd update.

- **Settings**: a plugin named `Foo` reads the section `[Foo]` of `urfd.ini`; urfd hands it the lines at every start and reload. `inicheck` does not know plugin sections and only notes them.
- **Routing**: `Foo` is a protocol name in every `BlockProtocols` and in the admin `block`/`unblock` commands; its clients pass the gatekeeper like the nodes of the other protocols.
- **Admin socket**: its own commands work by name or as `{"cmd": "plugin", "name": "Foo", "command": "...", ...}`; `status` lists the plugins (`plugins`) with what they add, `tg_list` their modules.
- **Dashboard**: the admin page knows no plugin. Each plugin describes its sections in the `status` answer (tables filled by one of its commands, a form and a remove button calling others) and the page draws them, see [docs/PLUGINS.md](docs/PLUGINS.md). The protocol list shows the plugin's description.
- **Logins**: it may keep its logins in the [credential database](#credential-database-4200) under a service name of its own.
- **Building**: put the plugin's source into `plugins/<name>/` (with a `Makefile` that takes `URFD=`), e.g. as a `git clone` of its repository; git ignores it there. The Docker image builds it with urfd into `/usr/local/lib/urfd/`; natively `make plugins` and `sudo make plugins-install` in `reflector/` do the same. `setup.sh update` pulls every plugin directory that is a git clone (also in `tcd/plugins/`) before it builds.

### USRP (AllStar)
Bridges one AllStar node (app_rpt's USRP channel) to a module. Requires a transcoded module.

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

**RxGain / TxGain**: like the SVX ones, per connection in `urfd.ini`; samples past full scale are clipped. tcd runs AllStar audio through its AGC analog, so they normally stay 0.

### MMDVM Server
Accept incoming connections from MMDVM hotspots and repeaters directly (no BrandMeister needed). Implements the HomeBrew DMR protocol (RPTL→RPTK→RPTC→RPTO handshake). Works without transcoder for pure DMR-to-DMR operation; transcoded modules needed only for cross-mode bridging. TG mapping works like MMDVMClient — TGs map to modules with timeslot support. **Dual-slot**: Each timeslot (TS1/TS2) independently links to its own module, so a hotspot can be on two modules simultaneously.

```ini
[MMDVM]
Enable = true
Port = 62030
# FallbackDmrId = 0                # DMR ID for unknown callers (0 or omit = drop)
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

**Extended DMR IDs**: BrandMeister-style hotspots append a 2-digit suffix to the 7-digit base DMR ID (e.g. `123456701` = base `1234567` + suffix `01`). urfd strips the suffix automatically — one user entry with a base id covers all extensions.

**User authentication**: Users can authenticate with a DMR-ID (7-9 digits) or callsign. Callsigns are resolved to DMR IDs at startup via the DMR ID database. If no users are configured, all logins are rejected. Individual users can be configured with an empty password to allow any password (useful for compat hotspots). Set `RequireAuth = false` to disable authentication entirely and accept all connections (legacy behavior). Users can be managed at runtime via the Admin interface.

**TG mapping**: Same semantics as MMDVMClient — first TG per module is primary (TX+RX), additional TGs are secondary (RX only). Since 4.29.0 the module talkgroups (`[Modules] TG<M>`) map with `TS1 = <modules>` / `TS2 = <modules>`, and `TGs = auto` maps every other module with a talkgroup (on TS2, `TGs = auto,TS1` for TS1), so hotspots reach every module; `TG` lines win. Dynamic TGs can be added via the Admin interface with configurable TTL.

**Dual-slot linking**: Each timeslot links independently. When a hotspot transmits on TS1 with TG 4019 (module S) and on TS2 with TG 4004 (module D), it receives traffic from both modules simultaneously — module S on the timeslot configured for that TG, module D on the other. For this to work, map TGs for different modules to different timeslots. The dashboard shows both linked modules (e.g. "S (TS1), D (TS2)").

**Talker alias**: Every call reaches the hotspots with a talker alias, as from BrandMeister: the callsign and the first name from the DMR ID database (e.g. `N0CALL Max`), at most 13 characters in UTF-16 like BrandMeister's, as Motorola radios ignore an ISO 8 bit alias. If the DMR source sends its own alias (BrandMeister, or a radio with one), its text is passed on once it arrived complete, re-encoded in UTF-16 (sources send 7 bit, ISO 8 bit or UTF-8 too), starting over with the header so no blocks of the two get mixed; until then, and for D-Star, YSF, M17 and the other modes, it is ours. It goes in the embedded signalling, one LC in every other superframe; the hotspot shows it and passes it to the radio. MMDVMHost fills the superframes in between with the voice LC (source, TG, its own OVCM setting), so a radio that joins a call late still gets it. The service options in the voice LC (OVCM, emergency, privacy) are always 0: a single source's flags are not passed on to every linked hotspot.

**Hotspot config**: Point the hotspot directly at the reflector IP on port 62030. In WPSD/Pi-Star, use "MMDVM_BRIDGE" or single-network mode. DMRGateway multi-network mode routes traffic to BrandMeister by default and must be disabled.

**XLX master mode** (WPSD/Pi-Star "XLX Master" with a startup module) works too: DMRGateway links the module with a private call to 4001-4026 and gets the module on TS2/TG9, which it maps to the radio's slot and TG (TG6 by default). DMRGateway logs in with the fixed password `passw0rd` from `[XLX Network]` in `/etc/dmrgateway`, which xlxd never checks. Either put the user's urfd password there (WPSD/Pi-Star: Configuration → Expert → DMR GW; check it after saving the main configuration page, which may write `passw0rd` back) or give the user an empty password in `[MMDVM]`, which accepts any password. Otherwise the reflector logs `auth FAILED`.

**Admin commands**:
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

### D-Star Client Connectors (DCS, DExtra, DPlus)
Connect to external D-Star reflectors as a client node. Maps remote reflector modules to local reflector modules. All three D-Star linking protocols are supported with their respective wire formats. Mappings can be configured statically in `urfd.ini` or added dynamically at runtime via the Admin interface.

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

**Map format**: `host,port,remote_module,local_module` — each mapping creates one connection to the remote reflector on the specified module and routes audio to the local module.

**Callsign requirements**: DPlus reflectors require registration at [dstargateway.org](https://regist.dstargateway.org/). DExtra and DCS reflectors are typically open.

**Dynamic mappings**: Use the Admin dashboard "D-Star Client Mappings" section or the socket API:
```json
{"cmd": "dcs_map_add", "token": "...", "host": "dcs002.xreflector.net", "port": 30051, "remote_module": "A", "local_module": "S"}
{"cmd": "dcs_map_remove", "token": "...", "local_module": "S"}
{"cmd": "dcs_map_list", "token": "..."}
{"cmd": "dextra_map_add", "token": "...", "host": "xrf757.openquad.net", "port": 30001, "remote_module": "A", "local_module": "A"}
{"cmd": "dplus_map_add", "token": "...", "host": "ref001.dstargateway.org", "port": 20001, "remote_module": "A", "local_module": "Z"}
{"cmd": "reconnect", "token": "...", "protocol": "dcsclient"}
```

### YSF Client Connector
Connect to external YSF reflectors as a client node. Supports optional DG-ID for room selection on DG-ID-aware reflectors. Maps each connection to a local reflector module.

```ini
[YSFClient]
Enable = true
Callsign = N0CALL
Map1 = 90.187.72.177,42099,A             # YSF reflector -> local module A
Map2 = 85.215.138.68,42000,F,15          # YSF reflector -> local module F, DG-ID 15
# BlockProtocols = MMDVMClient            # Optional
```

**Map format**: `host,port,local_module[,dgid]` — DG-ID is optional (0 = disabled). When set, the DG-ID is included in the FICH header of all outbound voice packets.

**YSF Radio field**: Outbound voice packets carry the reflector callsign (e.g. `URF363`) in the YSF radio/description field (CSD1 bytes 0-9). This is displayed as the "Radio" column on YSF dashboards like ysf-deutschland.de.

**Dynamic mappings**:
```json
{"cmd": "ysf_map_add", "token": "...", "host": "90.187.72.177", "port": 42099, "local_module": "A", "dgid": 0}
{"cmd": "ysf_map_remove", "token": "...", "local_module": "A"}
{"cmd": "ysf_map_list", "token": "..."}
```

### NXDN Module Routing
Select reflector modules via the NXDN RAN (Radio Access Number) or via the destination ID.

```ini
[NXDN]
Enable = true
Port = 41400
AutoLinkModule = S             # Module for RAN 0 and RAN 1 (comment out to disable)
ReflectorID = 26363
# FallbackNxdnId = 8970        # NXDN ID for unknown callers (0 or omit = drop)
ModuleDstIdBase = 4000         # dstId 4001-4026 = A-Z (omit to disable)
```

**RAN-to-Module mapping**: RAN 1-26 maps to Module A-Z, with RAN 1 and the AutoLinkModule's own RAN swapped. RAN 1 always reaches the AutoLinkModule, and Module A takes the AutoLinkModule's RAN in exchange. RAN 0 maps to the AutoLinkModule as well. With `AutoLinkModule = S`:

| RAN | Module | RAN | Module | RAN | Module |
|-----|--------|-----|--------|-----|--------|
| 0   | S      | 10  | J      | 19  | **A**  |
| 1   | **S**  | 11  | K      | 20  | T      |
| 2   | B      | ... | ...    | 26  | Z      |

The swap exists because RAN 1 is the only RAN most clients can produce. DroidStar hardcodes `set_sacch_ran(0x01)`, and MMDVMHost overwrites the radio's RAN with the hotspot's configured `[NXDN] RAN` (default 1) before forwarding, and rejects RF whose RAN differs. Per-transmission RAN switching from the radio therefore only works on a direct connection; through a hotspot the RAN is a per-hotspot setting.

**dstId-to-Module mapping** (takes precedence over the RAN): set `ModuleDstIdBase` to enable. dstId `base+1` to `base+26` maps to Module A-Z, any other dstId falls through to the RAN. Evaluated in the connect poll and in every frame, so the module is correct from the first packet on. With `ModuleDstIdBase = 4000` the NXDN destination IDs line up with the DMR+ TG mapping (4001 = A, 4002 = B, …).

This is the only module selection available to clients that cannot set a RAN. DroidStar users add one custom host per module under Settings, where the number is the dstId:

```
NXDN 4001 my.reflector.org 41400     -> Module A
NXDN 4003 my.reflector.org 41400     -> Module C
NXDN 26363 my.reflector.org 41400    -> AutoLinkModule
```

Outgoing frames carry the destination ID each client polled with, encoded per distinct dstId on the module. This is required, not cosmetic: `NXDNGateway` drops every frame from the reflector whose dstId differs from the TG the hotspot is linked to, so a client linked to the plain reflector ID and one linked to a module dstId must each receive their own.

**NXDN ID Resolution**: Incoming NXDN IDs are resolved to callsigns via the NXDN ID database. If not found, the DMR ID database is used as fallback (many operators reuse their DMR ID as NXDN ID). This enables cross-protocol callsign display and D-Star slow data name lookup. Unknown IDs are dropped unless FallbackNxdnId is configured.

**D-Star Slow Data**: NXDN sources show `NXDN RAN<n>` in the D-Star text field. Operator names are looked up from both DMR and NXDN databases.

**Dashboard**: The NXDN RAN column is shown in the module overview when NXDN is enabled, and a separate `NXDN DST-ID` column appears next to it once `ModuleDstIdBase` is set. DMR+ and YSF DG-ID columns are hidden when their respective protocols are disabled.

### Dynamic Talkgroup Timer Behavior

Both MMDVM and SVX dynamic TGs have a 15-minute inactivity TTL. The timer is refreshed on any transmission start (DvHeader) on the module, regardless of which protocol originated the traffic. This means cross-protocol activity keeps all timers alive consistently.

**BrandMeister note**: BM maintains its own TG subscription timer independently. When the BM subscription expires, urfd's local mapping will also expire if there is no further activity. Use the **kerchunk** admin command to manually extend the BM-side subscription without needing a radio.

**SVX note**: When a dynamic SVX TG expires, urfd takes it off the monitored TGs; if it was selected, the home TG is selected again (or none if no TG is left).

### Admin Interface
Runtime management via a JSON-based TCP socket, accessible through the dashboard web panel. Runs in its own thread and does not block audio processing. Each client connection is handled in a separate detached thread with a 5-second idle timeout, so slow or stalled clients cannot block the admin socket.

```ini
[Admin]
Enable = true
Port = 10101
Password = yoursecretpassword
# BindingAddress = 127.0.0.1      # Default: loopback only
```

Dashboard config (`config.inc.php`):
```php
$Admin['Enable']   = true;
$Admin['Host']     = '127.0.0.1';
$Admin['Port']     = 10101;
# Log in on the admin page with the [Admin] Password from urfd.ini
```

**Features:**
- **Dynamic TG Management**: Add/remove talkgroups at runtime with configurable TTL. With BM API key: registers as static via REST API (no kerchunk needed). Without: kerchunk to activate on BM. For SVX, updates the monitored TGs.
- **Runtime Protocol Blocking**: Block/unblock any protocol pair bidirectionally at runtime. Not persisted — resets on restart. "Reset to Config Default" button restores INI settings.
- **Multi-TG per Module**: Dynamically add secondary TGs (RX only) to modules that already have a primary TG.
- **Kerchunk on Demand**: Send a kerchunk to BrandMeister for a specific TG without a radio. MMDVM only. Not needed when BM API key is configured.
- **D-Star Client Mappings**: Add/remove DCS, DExtra, DPlus reflector connections dynamically with module-to-module mapping. Protocol selector for DCS/DExtra/DPlus with auto-port defaults.
- **YSF Client Mappings**: Add/remove YSF reflector connections with optional DG-ID.
- **MMDVM Server Management**: Add/remove/list users (by DMR-ID or callsign) and TG mappings. View connected hotspot nodes with DMR-ID, frequencies, location, and software info.
- **Protocol Reconnect**: Force reconnect of MMDVM, SVX, DCSClient, DExtraClient, DPlusClient, or YSFClient connections.
- **Transcoder Statistics**: Connection status, active codec, packet counts, round-trip time per transcoded module.
- **Live Log Viewer**: Last 50 log lines with timestamps, auto-refreshing every 10 seconds (the socket gives up to 200).
- **Clear Last Heard**: Clears the last heard users list from the dashboard.
- **Hidden Access**: Access via the pi symbol in the bottom-right corner or directly via `?show=admin`.

**Socket Protocol**: Line-delimited JSON over TCP. Authenticate first, then send commands with the returned token:
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

### Credential Database (4.20.0)

The logins of `[MMDVM]` and `[SVXServer]` (and of [plugins](#plugins) that use it) can also come from a database, so another program can manage them: SQLite, MySQL/MariaDB or PostgreSQL. Users in `urfd.ini` stay valid and win; only a user who is not there is looked up, and the answer stays in an LRU cache (`CacheTTL`, default 300 s; unknown users `NegativeTTL`, 60 s). If the database is down, cached logins keep working. The admin page adds new users to the database. The passwords are stored as plain text, as the protocols need them for their challenge. Off by default:

```ini
[Credentials]
Backend = sqlite           # ini (no database), sqlite, mysql or postgresql
Path = /usr/local/etc/urfd/credentials.db
# Host = localhost         # mysql, postgresql: Host, Port, Database, User, Password
# Table = urfd_credentials
# CacheTTL = 300
```

Table, cache, building and the admin commands `credentials_status` and `credentials_flush`: [docs/CREDENTIALS.md](docs/CREDENTIALS.md). Suggested by Jason Kendall (coolacid), issue #11.

### WebSocket (4.13.0, bridges and names 4.14.0)

Live events for the dashboard: clients, peers and talking stations (with the operator's name from the ID database) as they change, the bridges of each module (TG mappings, autolinks, connectors to other reflectors) and their changes, and the whole state when a client connects, without IP addresses. Read only and without a password, so it listens on loopback (`127.0.0.1:10102`) and the dashboard's nginx passes `/ws` through. Always built in (needs `libwebsockets-dev`), off until `[WebSocket] Enable = true`, which `setup.sh` sets on install and on `setup.sh update`; changes need a restart. Based on the notifier by Jason Kendall (coolacid), whose live dashboard the Docker image serves at `/live/`, next to this dashboard. Messages and setup: [docs/WEBSOCKET.md](docs/WEBSOCKET.md).

### SIGHUP Configuration Reload

The reflector supports hot-reloading configuration without dropping client sessions. Send `SIGHUP` to the urfd process to trigger a reload.

**Reloaded on SIGHUP:**
- TG mappings (MMDVMClient, SvxReflector and SVXServer) — dynamic TGs are preserved
- Whitelist, blacklist, and interlink files
- Transcoded modules (`[Transcoder] Modules`, also to and from `auto`, since 4.25.1); a tcd connection for a module no longer listed is closed
- Reflector metadata (callsign, sponsor, country, URLs)
- Database refresh parameters

**Requires full restart:**
- Module list changes (adding/removing modules)
- Protocol enable/disable or port changes
- IP address binding changes
- Admin interface settings
- Echo module assignment
- Transcoder port, binding address and password

```bash
sudo ./setup.sh reload

# by hand, Docker: the signal must reach urfd, not supervisord (PID 1 restarts everything on SIGHUP)
docker exec urfd pkill -HUP -x urfd

# bare metal
sudo pkill -HUP -x urfd
```

### Reflector Interlinking
Peer with URF, XLX, DCS and M17 (mrefd) reflectors. DNS hostnames are supported. Configure interlinking in `urfd.interlink`:

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

DCS entries default to native DCS protocol (port 30051). Append `XLX` to use XLX peering (port 10002) instead. The protocol field is only supported for DCS entries.

XLX and DCS peering support optional callsign overrides (e.g. present as XLX363 instead of URF363):

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

M17 entries need the same modules on both sides, and this reflector's `[M17] Callsign` listed in the mrefd's `mrefd.interlink`. As on mrefd, an entry with only the modules (`M17-XXX AB`) takes address, port and legacy state from the mrefd's Ham-DHT document. urfd publishes its `[M17] Callsign` there in mrefd's format, so an mrefd can list this reflector the same way; pick a designator no other M17 reflector uses.

### Echo Module
Built-in echo/parrot function. Assign it to any module - audio is recorded and played back after 3 seconds of silence.

```ini
[Modules]
Modules = AFSZ

[Echo]
Enable = true
Module = Z
```

Set `Enable = false` to disable without removing the configuration.

M17 clients reach the echo module from whatever module they are linked to by transmitting to `PARROT` (as on mrefd) or `ECHO`; the playback of their own recording is sent back to them on their linked module as well. A packet mode packet (`M17P`) to `PARROT` or `ECHO` goes straight back to its sender, addressed to @ALL, as on mrefd.

### M17 Module Selection
An M17 client picks its module when it links: the `CONN` (or `LSTN`) packet carries the module letter, and the client then hears that module. To change modules, unlink and link again with the other letter, e.g. `URF363 B` as the reflector in Pi-Star/WPSD, the module selector in DroidStar or mvoice, or `URF363 B` as the destination on an M17 radio. A new link replaces the old one of the same callsign and IP.

As on mrefd, a transmission goes to the module the client is linked on, whatever its destination (`URF363 B`, `M17-XXX B` or @ALL); streams to the clients are addressed to @ALL. Only `PARROT` or `ECHO` as the destination reaches the echo module (see Echo Module).

### M17 LSTN Support
M17 listen-only clients (LSTN packets) are accepted and registered on the requested module. This enables monitoring services like dvref.com to check reflector availability. Voice and packet mode data from a listen-only client are dropped, as on mrefd.

### M17 Protocol
- **TYPE field**: Voice frames are recognised in both the legacy and the V#3 TYPE layout of the M17 specification; encrypted streams are dropped in both.
- **Packet mode**: `M17P` packets (e.g. SMS) are checked against both CRCs and forwarded unchanged to the other M17 clients on the sender's module; the sender shows in last heard.
- **URF interlink 4**: The stream header to URF peers carries the M17 META type and CAN, so GNSS stays GNSS across linked reflectors. The interlink version is negotiated per peer: only peers announcing major 4 get the 2 extra bytes, peers announcing 3 (other urfd forks) get the unchanged legacy header. `[URF] InterlinkVersion = 3` forces legacy mode, announcing 3 and never sending the extension.
- **mrefd interlink**: Modules can be shared with mrefd reflectors, listed as `M17-XXX <address> <modules> [port]` in `urfd.interlink` (a trailing `L`, `M17-XXXL`, for a legacy mrefd < 1.0), or as `M17-XXX <modules>` with the address from the Ham-DHT. urfd links under the M17 designator set in `[M17] Callsign = M17-XXX`; without it M17 interlinking is off. Link, keepalive, voice and packet mode data follow mrefd: data to an mrefd >= 1.0 carries the module as an extra byte, a legacy one is addressed as `M17-XXX <module>` and gets no packet mode data. As in mrefd, a stream from one mrefd is not passed on to another one. Interlinked mrefds show up as M17 peers.
- **Callsigns**: As on mrefd, a `CONN`, an `LSTN` (more permissive) and the source of voice and packet data must look like a callsign, anything else is dropped. A client put on the blacklist after it linked gets a `DISC` and is removed.
- **Relinking**: A `CONN`/`LSTN` replaces any earlier link of the same callsign and IP, so switching modules does not leave a stale entry behind.
- **META**: The META bytes reach other M17 clients with a TYPE that says what they carry (text, GNSS or ECD) and the source's CAN, so a GNSS position is no longer shown as text. A GNSS position is decoded into a Maidenhead locator, shown in the last-heard list (linked to OpenStreetMap) and in `/json/stations`.

### Protocol Enable Flags
All protocols can now be individually enabled/disabled. Default is `true` (backwards-compatible).

```ini
[DPlus]
Enable = false
Port = 20001

[P25]
Enable = false
Port = 41000
```

### Resilient ID Database Loading
The DMR ID and YSF TX/RX databases survive an outage of their publisher.

```ini
[DMR ID DB]
Mode = http
FilePath = /usr/local/etc/urfd/dmrid.dat
# Comma separated: first entry is primary, the rest are tried in order
URL = https://radioid.net/static/user.csv, http://www.pistar.uk/downloads/DMRIds.dat, http://xlxapi.rlx.lu/api/exportdmr.php
```

**Multiple sources**: `URL` accepts a comma separated list. Sources are tried in
order until one answers, and a message is logged whenever a fallback is used.

**Separator tolerance**: fields may be separated by `;`, `,` or a tab, so the common
publishers work unchanged — radioid.net CSV (`id,call,first,last,…`),
Pi-Star `DMRIds.dat` (tab separated) and xlxapi (`id;call;`) all parse. xlxapi carries
no names, so the DMR talker alias would be the bare callsign; it is therefore last. Field
order is the same everywhere; lines not starting with an ID, such as a CSV header,
are skipped.

**Download cache**: every successful download is mirrored to `<FilePath>.cache`. If
no source answers at startup, the cache is loaded instead, so the database is never
empty just because a directory is down. This is separate from `FilePath`, which in
`Mode = both` holds the operator's own additional entries and is never overwritten.

Note that no second publisher of the YSF TX/RX data is known, so there the cache is
the only fallback. Missing YSF frequencies only affect Wires-X frequency reporting.

### Extended XML Output
The XML status file now includes:

- **Reflector metadata**: callsign, country, sponsor, dashboard URL, email
- **Module configuration**: description, linked node count, transcoded status, DMR+ TG ID, YSF DG-ID, NXDN RAN, NXDN destination ID (only when `ModuleDstIdBase` is configured)
- **Per-module mappings**: autolinks (YSF, NXDN, P25), TG mappings (MMDVM, MMDVMClient, SvxReflector, SVXServer), D-Star client mappings (DCSClient, DExtraClient, DPlusClient with connection status), YSF client mappings (YSFClient with DG-ID)
- **Enabled protocols**: name and port for each active protocol
- **Per-station protocol**: which protocol a user was heard on (DCS, MMDVMClient, YSF, etc.)
- **Per-station name**: `<Name>` with the operator's first name from the DMR ID database, else the NXDN one, when the callsign is in there
- **On air**: `<OnAir>` in each module that has an open stream, with the callsign transmitting
- **Per-station position**: `<Locator>`, `<Latitude>` and `<Longitude>` for stations that sent an M17 GNSS or a D-Star DPRS position
- **Dynamic TG indicators**: `<Dynamic>true</Dynamic>` and `<Remaining>` seconds for runtime-added TGs

Module names are configured once in `urfd.ini` and automatically available in the dashboard.

The JSON report also includes a `DynamicTGs` array with protocol, TG, module, and remaining seconds for all active dynamic mappings.

### Dashboard v2.14.1
Redesigned in v2.9.0 as the front panel of a radio: an amber LCD shows who is on the air, the modules are a keypad, the rest are plain tables. Before that: dark mode (v2.6.0), hardened against directory outages (v2.6.1), M17 positions in the last-heard list (v2.7.0), directory registration moved to the reflector (v2.8.0), security fixes (v2.8.1, v2.8.2), status file and admin socket fixes (v2.8.3). Since v2.10.0 the operator's first name stands next to the callsign, from the ID databases urfd loads (urfd 4.12.0 and later).

**Pages:**
- **Last heard** - The LCD shows the station transmitting right now with its module, mode, route and a running timer, or the last station heard, dimmed. Next to it a key per module: amber when nodes are linked, a red mark while someone transmits. A key switches the LCD and the log to its module; without a key the LCD shows the transmission that started last and names the other modules on the air; the callsign filter takes wildcards (`DL*`). Both filters survive the page refresh.
- **Modules** - One table per module: name, linked nodes (expandable), DMR+ TG, YSF DG-ID, NXDN RAN and destination ID, transcoding and bridges (static, and dashed for runtime ones). Protocol columns are hidden when the protocol is off. Below it the server protocols with their ports; client protocols that connect outbound are left out. `?show=modulesd` from older links lands here.
- **Links**, **Peers** - Linked nodes and peer reflectors, with how long they have been linked and the IP as far as `IPModus` allows.
- **Reflector list** - The XLX directory with search and 25 per page.
- **QuadNet live** - D-Star routing traffic from the OpenQuad feed, with search.
- **Admin** (hidden): talkgroups with kerchunk, D-Star and YSF links, SVX and MMDVM users and nodes, transcoder, protocol blocks, log. An index lists only the sections the running protocols use. Access via the pi symbol in the footer or `?show=admin`.

Every page but Last heard and Admin carries a slim LCD line with the current transmission. The TX state comes from the `<OnAir>` element of the status file (urfd 4.11.0 and later); with an older urfd the dashboard shows the last station heard instead.

**Features:**
- Light and dark theme, following the system, with a switch in the top bar
- Font (Saira, OFL) served from the dashboard itself, no Google Fonts request
- No Bootstrap any more; jQuery only on the admin page
- Content refreshes in place every `PageRefreshDelay` ms, and at once on a speaker change, not while you type in a field
- Locator of stations that send an M17 GNSS or a D-Star DPRS position, linked to OpenStreetMap
- MOTD/maintenance banner via `$PageOptions['MOTD']` in config.inc.php
- Module names read from urfd XML (single source of truth)
- Contact email obfuscated against bots

**Resilience against a directory outage**: the dashboard does not depend on
`xlxapi.rlx.lu` being reachable. The reflector list has an explicit timeout and is
cached on disk (`<CacheDir>/reflectorlist.cache`), so a fresh entry serves the page
with no network call at all and a stale one keeps the list working while the directory
is down. Since v2.8.0 the dashboard no longer registers the reflector in the directory
(the reflector does, see [CallingHome](#xlx-directory-registration-callinghome)).

```php
// pgs/config.inc.php
$Directory['ServerURL'] = 'http://xlxapi.rlx.lu/api.php';
$Directory['Timeout']   = 5;                    // Seconds before giving up on the directory server
$Directory['CacheDir']  = '/var/lib/xlxd-ch';   // Reflector list cache
```

Older configs without `$Directory` keep working: the values are taken from their
`$CallingHome` settings.

### JSON API
Read-only HTTP endpoints under `/json/`, served by the dashboard:

| Endpoint | Returns |
|---|---|
| `/json/metadata` | Dashboard version, reflector callsign and version, public IPv4/IPv6, sysop email |
| `/json/status` | Reflector up/down, uptime, last update timestamps |
| `/json/reflector` | Full reflector record including modules and metadata |
| `/json/links` | Connected nodes with callsign, IP, module, protocol, connect and last-heard time |
| `/json/peers` | Linked peer reflectors |
| `/json/stations` | Last-heard stations with country lookup and `name` (first name from the DMR or NXDN ID database, empty if unknown); `locator`, `latitude` and `longitude` when the station sent an M17 GNSS or a D-Star DPRS position |
| `/json/modulesinuse` | Modules with the callsigns currently on them |

Collection endpoints return `[]` when empty (`/json/stations` returns
`{"stations":[]}`); an unknown path returns 404.

**Web server requirements**: the endpoints are clean paths dispatched inside
`json/index.php`, so the web server has to route `/json/<name>` there.

- **nginx** — handled by the shipped `nginx-dashboard.conf`, which has a `location
  /json/` block. Without it the generic `try_files` falls through to the dashboard's
  own `index.php` and every endpoint answers `200` with an HTML page.
- **Apache** — relies on `json/.htaccess`, which a stock Debian install ignores:
  `apache2.conf` ships `AllowOverride None` for `/var/www/`, and `mod_rewrite` is not
  enabled. Without both of the following the endpoints return 404:

```bash
sudo a2enmod rewrite
# and in /etc/apache2/apache2.conf, for the <Directory /var/www/> block:
#   AllowOverride FileInfo
sudo systemctl restart apache2
```

### D-Star Slow Data for Transcoded Streams
Transcoded streams (DMR, YSF, SVX, M17, P25, USRP, client protocols -> D-Star) now include proper D-Star slow data:
- **Header**: Caller callsign (MY), reflector callsign (RPT1/RPT2)
- **Text message**: the operator's first name (if the caller's callsign is in the DMR or NXDN ID database) and the source: `Max DMR TG26363` (since 4.29.0; before, the name came as a second text that many radios never show)

Examples of text messages per protocol:

| Source | Slow Data Text |
|--------|---------------|
| MMDVMClient | `Max DMR TG26363` |
| SvxReflector | `Max SVX TG317424` |
| YSF (server) | `YSF DG28` |
| NXDN | `NXDN RAN6` |
| DCSClient | `DCS002 A` |
| DExtraClient | `XRF757 A` |
| DPlusClient | `REF001 C` |
| YSFClient | `YSF DG15` |
| M17, P25, USRP, DMR+ | `M17`, `P25`, etc. |

D-Star client protocols extract the remote reflector name from the hostname (e.g. `dcs002.xreflector.net` → `DCS002`) and append the remote module letter. YSFClient appends the DG-ID if configured (otherwise just `YSF`).

The text message has 20 characters. When `<name> <source> TG<n>` is longer, the TG moves to a second text that takes turns with the first every ~5 seconds; when it is still too long, the name is shortened so the source stays whole:

```
Max DMR TG262                                  (all fits)
Maximilian DMR    <-> DMR TG263112              (the TG takes turns)
Bartholomae YSF DG15  <-> Bartholomaeus         (the name shortened)
```

Header and text message alternate every superframe (~420ms):

```
[Header: N0CALL > URF999 S] → [Max DCS002 A] → [Header] → [Max DCS002 A] → ...
```

Name lookup works for all protocols — any callsign with a DMR ID database entry will have its name displayed on D-Star radios.

## Introduction

The URF Multi-protocol Gateway Reflector Server, **urfd**, is part of the software system for a Digital Voice Network. It supports D-Star (DPlus, DCS, DExtra, G3), DMR (MMDVM, DMR+, MMDVMClient), M17, YSF, P25, NXDN, USRP (AllStar), SvxReflector (SvxLink FM client) and SVXServer (SvxLink FM server).

A key part of this is the hybrid transcoder, [tcd](tcd/), in the `tcd/` folder of this repository, so reflector and transcoder always come from the same version. The reflector can be built without a transcoder, but clients will only hear other clients using the same codec.

This build supports dual-stack operation (IPv4 + IPv6).

## Docker Deployment

The reflector and its dashboard run in Docker, the transcoder in Docker or natively with systemd (setup.sh asks), with AMBE devices or in software with a codec plugin, on the same machine or on a second one ([tcd in Docker](tcd/README.md#tcd-in-docker)). See [`docker/README.md`](docker/README.md) for details.

### Quick start

`setup.sh` asks a few questions, writes `urfd.ini`, the dashboard config and `tcd.ini`, installs Docker if needed, and builds and starts everything. Run it again, or `sudo ./setup.sh update`, to update while keeping the configuration. The step-by-step guide is [docs/INSTALL.md](docs/INSTALL.md).

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd
sudo ./setup.sh
```

By hand, with the configuration in `/opt/urfd` (the steps before are in [docs/INSTALL-ADVANCED.md](docs/INSTALL-ADVANCED.md#reflector-in-docker)):

```bash
cd docker
docker compose build
docker compose up -d
```

With a [codec plugin](tcd/README.md#codec-plugins) for DMR, the transcoder needs only one DVSI device, DMR/YSF then runs in software.

### Container architecture

The urfd container runs two services via supervisord:
- **urfd** - The reflector daemon
- **nginx + php-fpm** - Dashboard on port 8363, the live dashboard at `/live/` and urfd's WebSocket at `/ws`

### Configuration files

All configuration is in `/opt/urfd/config/` (mounted as volume):
- `urfd.ini` - Main reflector configuration (supports SIGHUP reload)
- `urfd.interlink` - Peer linking (URF, XLX, DCS and M17 peers with DNS support)
- `urfd.blacklist` / `urfd.whitelist` - Access control (reloaded on SIGHUP)
- `urfd.terminal` - G3 (Icom Terminal/AP mode) configuration. Options: `address <public-ip>` (required behind NAT), `modules <letters>` (allowed reflector modules), `alias <callsign>` (additional reflector callsign accepted from terminals, e.g. to accept both `URFxxx` and `XLXxxx`; repeatable). Hot-reloaded via `stat()` on file change.

Dashboard config at `/opt/urfd/dashboard/config.inc.php` (mounted into container):
- `$PageOptions['MOTD']` - Maintenance banner (shown on all pages)
- `$PageOptions['ContactEmail']` - Footer contact (obfuscated)
- `$Directory` - XLX directory server and cache for the reflector list (registration is `[CallingHome]` in `urfd.ini`)

## Installation by Hand

Without the setup script: the reflector in Docker by hand, the reflector without Docker (`reflector/build.sh`), the dashboard on your own web server and the transcoder alone are in [docs/INSTALL-ADVANCED.md](docs/INSTALL-ADVANCED.md).

```bash
sudo bash reflector/build.sh   # urfd without Docker, with its systemd unit
sudo bash tcd/build.sh         # tcd
```

## Administration Tools

### setup.sh

Besides installing and updating, `setup.sh` handles the day-to-day work on the reflector (Docker container `urfd`) and the transcoder (systemd service `tcd`), whichever of the two runs on the machine.

```bash
sudo ./setup.sh status              # what runs here, in which version
sudo ./setup.sh restart [urfd|tcd]  # also start, stop
sudo ./setup.sh logs [urfd|tcd]     # follow the log (Ctrl-C ends it)
sudo ./setup.sh reload              # urfd rereads its config: TG mappings, lists, interlink (SIGHUP)
sudo ./setup.sh check               # check urfd.ini with inicheck
sudo ./setup.sh update              # pull, rebuild, restart
```

Without `urfd` or `tcd`, the commands act on everything installed. `reload` sends SIGHUP to urfd itself, not to the container: its PID 1 is supervisord, which restarts all programs on SIGHUP.

### inicheck

Validates `urfd.ini` syntax and cross-references (e.g. TG modules must exist in `[Modules]`, transcoded protocols need a transcoder, Echo module must be a configured module).

```bash
cd reflector
make inicheck
./inicheck -q urfd.ini   # Errors and warnings only
./inicheck -n urfd.ini   # Also print public config keys
./inicheck -v urfd.ini   # Print all keys (including internal)
```

Exit code 0 = OK, 1 = errors found. Warnings (missing optional fields) don't cause a non-zero exit.

## Interlink / Peering

Four types of peering are supported in `urfd.interlink`. Only the reflectors listed there may peer; with no entries no reflector can (up to 4.3.2 an empty file let any reflector peer).

| Type | Prefix | Default Port | Protocol | Requires |
|------|--------|-------------|----------|----------|
| URF Peer | `URF` | 10017 | URF native | - |
| XLX Peer | `XLX` | 10002 | XLX | `[XLXPeer] Enable = true` |
| DCS Peer | `DCS` | 30051 | DCS native | - |
| DCS Peer (XLX) | `DCS` + `XLX` | 10002 | XLX | `[XLXPeer] Enable = true` |
| M17 Peer (mrefd) | `M17-` | 17000 | M17 interlink | `[M17] Callsign = M17-XXX` |
| M17 Peer (legacy mrefd < 1.0) | `M17-…L` | 17000 | M17 interlink | `[M17] Callsign = M17-XXX` |

DNS hostnames are resolved via `getaddrinfo()` at load time. Both sides must list each other in their interlink files. DHT-based peering (no IP needed) is supported for URF peers when OpenDHT is enabled, not for M17 peers.

URF peers negotiate the interlink version: urfd announces 4 and adds the M17 META type and CAN to the stream header for peers that announce 4 too; peers announcing 3 (other urfd forks) get the legacy header. `[URF] InterlinkVersion = 3` makes this reflector announce 3 and never send the extension.

## Firewall

Open only the ports of the protocols you enable. All of them are incoming; the defaults are shown, the key in `urfd.ini` changes them.

Open ports are found by internet scanners within hours. urfd turns them away and logs a rejected connection (transcoder, MMDVM and SVXServer logins) once per address and kind each hour; the others of that hour are summed up in one line when it is over.

| Port | Transport | Protocol | Purpose | Key |
|------|-----------|----------|---------|-----|
| 80 | TCP | HTTP | Dashboard (native install) | – |
| 5300 | TCP + UDP | SVXServer | SvxLink nodes connect here | `[SVXServer] Port` |
| 8363 | TCP | HTTP | Dashboard (Docker) | `docker-compose.yml` |
| 8880 | UDP | DMR+ | DMR+ DMO mode | `[DMRPlus] Port` |
| 10002 | UDP | XLXPeer | XLX and BrandMeister peering | `[XLXPeer] Port` |
| 10017 | UDP | URF | URF interlinking | `[URF] Port` |
| 10100 | TCP | Transcoder | tcd, only if it runs on another host | `[Transcoder] Port` |
| 12345, 12346 | UDP | G3 | Icom Terminal and Access Point mode | fixed |
| 17000 | UDP | M17 | M17 clients and mrefd interlinking | `[M17] Port` |
| 20001 | UDP | DPlus | D-Star DPlus | `[DPlus] Port` |
| 30001 | UDP | DExtra | D-Star DExtra | `[DExtra] Port` |
| 30051 | UDP | DCS | D-Star DCS | `[DCS] Port` |
| 34000 | UDP | USRP | AllStar node (urfd sends to the node's `TxPort`, 32000) | `[USRP] RxPort` |
| 40000 | UDP | G3 | Icom Terminal voice | fixed |
| 41000 | UDP | P25 | P25 | `[P25] Port` |
| 41400 | UDP | NXDN | NXDN | `[NXDN] Port` |
| 42000 | UDP | YSF | YSF and Wires-X | `[YSF] Port` |
| 62030 | UDP | MMDVM | DMR hotspots and repeaters | `[MMDVM] Port` |

Local only, do not open: the admin socket (`[Admin] Port`, 10101) and the WebSocket (`[WebSocket] Port`, 10102, reached through the dashboard's `/ws`).

Outgoing only, nothing to open: SvxReflector (to the SvxLink server's port, usually 5300), MMDVMClient (to the master, `MasterPort`), the DCS, DExtra, DPlus and YSF clients, and the call home to the XLX directory.

## YSF

URF acts as a YSF Master providing Wires-X rooms (one per module). YSF users connect via hotspot software (Pi-Star/WPSD) which discovers URF reflectors through the XLX API. No registration at ysfreflector.de is needed. If `EnableDGID = true`, users can switch modules via DG-ID values (10=A, 11=B, ... 35=Z).

## Changes from upstream

### Protocol Connectors
- **MMDVMClient**: Full MMDVM protocol client for BrandMeister/DMR+ masters — multi-TG per module (primary TX/RX + secondary RX-only), per-timeslot support, fallback DMR ID, options string generation; up to nine connections at once (4.21.0) with per-network bridging rules
- **BrandMeister API**: Optional REST API v2 integration — startup sync preserves dynamic TGs (only removes TGs not in config or runtime map), API-based TG add/remove instead of kerchunk, configurable via `BrandMeisterApiKey`
- **SvxReflector**: TCP/UDP client for SvxLink FM servers — OPUS codec, bidirectional audio bridging, configurable RX/TX gain, one selected TG with the others monitored and selected on activity, auto-disconnect on TCP send failure with immediate reconnect
- **SVXServer**: SvxReflector V2 server — accept incoming SvxLink node connections with HMAC-SHA1 auth, OPUS audio, TG select and monitor like SvxReflector, node-to-node relay, TG-based module routing, per-user authentication, runtime user/TG management via admin API
- **DCS interlinking**: Native DCS reflector-to-reflector peering with PeerCallsign override and protocol field in interlink file
- **D-Star client protocols**: DCSClient, DExtraClient, DPlusClient — connect to external D-Star reflectors as a client node with module-to-module mapping, dynamic add/remove via admin API, protocol blocking, dashboard integration
- **YSF client protocol**: YSFClient — connect to external YSF reflectors with optional DG-ID, dynamic mapping via admin API, reflector callsign in YSF radio field

### Runtime Management
- **Admin interface**: JSON-over-TCP socket + web dashboard — dynamic TG management, protocol reconnect, transcoder stats, live log viewer, runtime protocol blocking, per-client threading with 5s idle timeout
- **Runtime protocol blocking**: Bidirectional block/unblock of any protocol pair via admin dashboard — thread-safe with shared_mutex, clickable labels to remove, reset-to-config-default button, not persisted (resets on restart)
- **Dynamic TG management**: Add/remove TGs at runtime with configurable TTL, cross-protocol timer refresh, BM API or kerchunk activation, expired SVX TGs leave the monitored TGs
- **Credential database**: Logins of MMDVM and SVXServer (and plugins) from SQLite, MySQL/MariaDB or PostgreSQL besides `urfd.ini`, LRU cache with TTL and negative caching, cached logins survive a database outage, admin flush and status
- **Transcoded modules from tcd (4.25.0)**: `[Transcoder] Modules = auto` transcodes a module while tcd is connected for it, so only `tcd.ini` lists the modules; a stream on a module without tcd passes as on a module without transcoding, the dashboard and the DHT entry follow
- **Name in the D-Star text (4.29.0)**: the D-Star slow data text reads `<first name> <source>` (`Max DMR TG262`, without "via") in its 20 characters, instead of showing the name only as a second text many radios never display; what does not fit takes turns with it. The name is written in ASCII (umlauts spelled out), and the text is no longer changed by the protocol's thread while the codec thread sends it
- **tcd 0.12.0**: codec plugin interface 2 (`TCD_CODEC_API 2`): `start` and `stop` tell a plugin where a transmission begins and ends, so it begins every transmission afresh, also under a stream id used again within 30 s; the monitor stops counting a stream at its last frame. Plugins built for interface 1 have to be rebuilt (tcd-mbelib-neo-plugin: `git pull`, `make clean && make`)
- **tcd 0.11.1**: the monitor reads a session cookie of any length (behind a reverse proxy the site's other cookies come along), failed logins older than 10 minutes do not count, at most 8 tcdmon connections wait in their password check
- **TGs = auto for the MMDVM server and SVXServer (4.29.0)**: `[MMDVM]` and `[SVXServer]` take the module talkgroups with `TGs = auto` (and `[MMDVM]` with `TS1` / `TS2` like an MMDVMClient), so hotspots and SvxLink nodes reach every module with a talkgroup
- **Module talkgroups (4.28.0)**: `[Modules] TG<M> = <number>` gives a module its talkgroup once; an MMDVMClient section maps it with `TS1 = <modules>` / `TS2 = <modules>`, plugins can take it (`CConfigure::GetModuleTG`)
- **Named MMDVMClient connections (4.27.0)**: `[MMDVMClient.<name>]` instead of `[MMDVMClient2]` ... `[MMDVMClient9]`, taking what they leave out from `[MMDVMClient]`; a `DmrId` of one or two digits (or a name of digits) is a suffix to its `DmrId`; the name shows in the log, the admin page, `BlockProtocols` and the admin commands
- **MMDVMClient sends right after login (4.26.0)**: BrandMeister takes the options without an ACK, so a connection waited for its beacon request (second 18 of a minute, up to a minute) before it sent any audio to the master, and resent the options every 10 s meanwhile; the options are sent once and count as taken after 2 s without a NAK
- **BrandMeister API sync retried (4.26.0)**: a static TG sync that did not go through (a new hotspot ID is known to the API only some minutes after its first login, "Device not found") is tried again after 1 minute, then less often up to every 30 minutes, instead of only at the next start; the log names the connection
- **Standby transcoder (4.26.0, tcd 0.11.0)**: a tcd with `StandbyModules` gets a module only while no other tcd has it and gives it back when that one returns; the reflector answers its handshake with taken or busy, and probes every transcoder connection for a dead one, not only up to the first module without one
- **Plugins built and updated with urfd**: `make plugins` / `make plugins-install` in `reflector/` build the plugins in `plugins/<name>/` natively, the Docker image uses the same targets; `setup.sh update` pulls every plugin directory that is a git clone (also codec plugins in `tcd/plugins/`)
- **Protocol plugins (4.24.0)**: protocols from shared objects loaded at start (`[Plugins]`), with their own ini section, protocol name, admin commands and dashboard parts, checked against the urfd they were built for
- **SIGHUP config reload**: Hot-reload TG mappings, whitelist/blacklist/interlink, transcoder modules without dropping sessions — thread-safe via sigwait, exception-safe with rollback

### Audio & Transcoding
- **NXDN module routing**: Module selection via RAN (Radio Access Number) — RAN 1-26 = Module A-Z, with RAN 1 and the AutoLinkModule's RAN swapped so RAN 1 reaches the AutoLinkModule, RAN 0 = AutoLinkModule. Alternative selection via destination ID (`ModuleDstIdBase`, dstId base+1…base+26 = A-Z) for clients that cannot set a RAN, evaluated in the connect poll and per transmission, with the module's dstId echoed on outgoing frames. NXDN ID resolution via NXDN DB with DMR DB fallback. FallbackNxdnId for unknown callers (drop if unset). Fixed uninitialized `m_uiNXDNid` in CCallsign default constructor.
- **D-Star slow data**: Transcoded streams include caller callsign, protocol/TG/RAN/module info, operator name from DMR and NXDN ID databases — rotating every ~5 seconds. TG display uses actual source TG (not primary/static) for correct multi-TG display. Client protocols show remote reflector name + module (e.g. `DCS002 A`), YSFClient shows DG-ID
- **SVX/USRP codec separation**: Independent codec paths (`ECodecType::svx` vs `ECodecType::usrp`) with gains per connection in urfd.ini (`RxGain`/`TxGain` for SvxReflector, SVXServer and USRP); in tcd both go through the AGC analog
- **MMDVM dual-slot**: Each timeslot (TS1/TS2) independently links to its own reflector module — a hotspot can receive traffic from two modules simultaneously on separate timeslots. Dashboard shows both linked modules per client.
- **MMDVM NAT rebind handling**: When a hotspot reconnects with a new UDP source port (NAT rebinding), the stale client entry is replaced by matching the exact raw DMR ID. Slot module links are preserved across reconnects. Multiple clients with different DMR IDs (including extensions) from the same IP are supported.
- **MMDVM late-entry**: Resolves DMR ID from active stream callsign (prefers cached ID from source protocol over DB lookup) — enables mid-stream block removal
- **Self-echo prevention**: MMDVMClient blocks self-routing back to BrandMeister

### Dashboard & Output
- **Protocol naming**: Consistent UI names — `MMDVM` = direct hotspot connections, `MMDVMClient` = BrandMeister/master server connection (previously both showed as "MMDVM" in different contexts)
- **Dashboard v2.6.0**: Dark mode redesign, admin panel, module overview with TG mappings and NXDN RAN, protocol-specific columns hidden when disabled, protocol list, QuadNet Live proxy, reflector list with search/pagination
- **Dashboard v2.6.1**: Survives an outage of the XLX directory. Explicit timeouts on every outbound request, CallingHome failures logged instead of aborting the page render, reflector and peer lists served from an on-disk cache, admin API no longer serialized behind the PHP session lock
- **Dashboard v2.7.0**: Locator column for stations that send an M17 GNSS position, also in `/json/stations`
- **Dashboard v2.14.1**: the Links page shows a band only for D-Star links, where the suffix is the repeater's port letter; elsewhere it is the module, and a node on module D was shown as a dongle
- **Dashboard v2.14.0**: Admin page: the sections of loaded plugins, drawn from the description each plugin gives in `status` (urfd 4.24.0 and later); the protocol list shows a plugin's description. Since urfd 4.26.0 more than 8 modules spread evenly over at most 5 columns (9 = 5 + 4, 12 = 4 x 3), each key with its module's name, and the plugin sections have the same space between them as the others
- **Dashboard v2.13.0**: Admin page: several MMDVMClient connections, each one offered when adding a TG, its name and network in the TG table, a reconnect button per connection (urfd 4.21.0 and later); the protocol list leaves out MMDVMClient2 to 9 like the first
- **Dashboard v2.12.3**: Admin page: a "Credential database" card with backend, connection, cache counters and a button that flushes the cache (urfd 4.20.0 and later); users from the database are marked "(database)" in the user lists
- **Dashboard v2.12.2**: The callsign in the links leads to QRZ like in the last heard list, not to aprs.fi
- **Dashboard v2.12.1**: The SVX nodes table shows the monitored TGs next to the selected one and when the node connected
- **Dashboard v2.12.0**: "also on" names three modules at most and counts the rest (all of them in the tooltip); more than 8 modules get a compact keypad of 7 columns with the module name in the tooltip
- **Dashboard v2.11.0**: The LCD follows the selected module key (who transmits there, or who was heard there last); without a selection it shows the transmission that started last, and "also on S" names the other modules on the air, in the LCD and the on-air line
- **Dashboard v2.10.0**: Operator's first name next to the callsign in the LCD, the last-heard list, the on-air line and `/json/stations`; a speaker change shows within about two seconds (a small probe, `api/onair.php`, is polled every 1.5 s and triggers the reload); a smaller LCD; the theme switch keeps the top bar on one line
- **tcd 0.7.0: version in the monitor**: the tcd monitor shows the running tcd version in its top bar, like the dashboard
- **tcd 0.6.0 – 0.7.2: DMR level in the bitstream for other codecs**: `OutputGainDMRBitstream` applies part of `OutputGainDMR` to D-Star, M17, P25, USRP and SVX audio through b2 after the AMBE2+ encoder, so the encoder gets more level; since tcd 0.7.0 a split in dB (all of it left artifacts on quiet syllables), since 0.7.1 without crackling, since 0.7.2 -6 dB by default (0 = off), live in the tcd monitor
- **tcd 0.5.0: noise suppression for analog input**: SvxLink and AllStar audio goes through the speexdsp preprocessor before the AGC analog (`AGCAnalogNoiseSuppress`, -10 dB, with the gate floor of AGC analog now at 0), so the FM noise under the speech no longer sounds robotic after the AMBE encoder on DMR; see [tcd/README.md](tcd/README.md#agc)
- **tcd 0.9.0: speech-driven AGC**: the level follows the upper envelope of the speech and only rises on clearly voiced speech well above a noise floor measured per stream, so the gain no longer pumps on breath, hiss or pauses; on FM-Funknetz traffic the gain moves a third as much and the speech stands 2.5 dB further above the noise, at the same level and spread between stations; see [tcd/README.md](tcd/README.md#agc)
- **tcd 0.10: codec plugins**: D-Star and DMR/YSF in software from shared objects (`CodecPlugin` in `tcd.ini`), with or without AMBE sticks, the vocoder switchable at run time in the monitor; the MD380 is gone, IMBE is part of the source, the monitor runs on libwebsockets and gets a compact view for many modules, and tcd can run in Docker without sticks; see [tcd/README.md](tcd/README.md#codec-plugins)
- **SVXServer UDP heartbeat (4.12.5)**: urfd sends a node a UDP heartbeat when it sent it nothing for 15 s, like SvxReflector; it only answered the node's heartbeats, and SvxLink sends none while it talks, so it dropped the link after a transmission of 60 s ("UDP Heartbeat timeout")
- **SVXServer nodes in the links (4.12.4)**: a SvxLink node is linked on the module of its selected TG, and while it selects none (SvxLink idles on TG 0) on the module of the first monitored TG that is mapped, so it shows in the links like an MMDVM hotspot; the entry follows TG map changes within 5 s
- **SVXServer node list (4.12.3)**: the node info is read with the keys SvxLink really sends (`sw` + `swVer`, `nodeLocation`/`Location`, `Locator`, the `qth` sites of `node_info.json`), so the software and location are no longer empty; `svxs_peer_list` also returns `monitored_tgs` and `connected_since`
- **Module node count without links (4.12.2)**: `<LinkedNodes>` counts only the nodes the status file lists; a link the reflector keeps to another server (MMDVMClient, DCS, DExtra, DPlus or YSF client, DCS peer) no longer adds one to each module it is mapped to
- **Clean transcoder shutdown (4.12.1)**: stopping urfd no longer floods the log with `POLLNVAL received on module`; a socket that turns invalid is dropped, the socket list is no longer emptied under the running codec threads, and an error or hangup without data is no longer handed on as a packet
- **Operator names in the status file (4.12.0)**: `<Name>` per station from the DMR ID database (NXDN as fallback), looked up on a copy of the list so a database reload never holds up the last-heard lock; the status file is written right after a stream opens or closes or a station is heard, not only every 10 s
- **Dashboard v2.9.0**: Redesign as a radio front panel: LCD with the station on the air, module keypad, merged Modules page, light and dark theme, self-hosted font, Bootstrap removed
- **On-air state in the status file (4.11.0)**: each module that has an open stream carries `<OnAir>` with the callsign transmitting, so the dashboard shows TX from the reflector instead of guessing from the last-heard time
- **Dashboard v2.8.3**: A page load no longer dies on an empty status file; admin replies past 8 KB (user and peer lists, the log) arrive whole instead of as "No response from admin socket"; `&`, `<` and `>` in callsigns and descriptions show as such
- **Dashboard v2.8.2**: The admin password is no longer kept in `config.inc.php` (`setup.sh update` removes old copies); the admin page logs in with the `urfd.ini` password
- **Dashboard v2.8.1**: Everything from outside is escaped (the `show` parameter, the reflector list from the XLX directory, hotspot and node data on the admin page, callsigns and filters); the admin API accepts only requests from its own page (JSON and `X-Requested-With`, no CORS), its session cookie is `HttpOnly` and `SameSite=Strict` and renewed at login
- **Dashboard v2.8.0**: No CallingHome any more, the reflector registers itself; the reflector list reads `$Directory` (falling back to old `$CallingHome` settings); `/json/metadata` takes the dashboard URL from the reflector's XML
- **Extended XML/JSON**: Module mappings (static + dynamic with TTL), reflector metadata, protocol list, per-user protocol info, dynamic TG array
- **Protocol blocking display**: Active blocks shown as clickable labels in admin panel
- **WebSocket (4.13.0)**: live events for the dashboard (clients, peers, talking stations, the whole state on connect) on `127.0.0.1:10102`, passed through as `/ws` by nginx; based on the notifier by Jason Kendall (coolacid), with the review fixes (lws calls only in its service thread, connections really closed, no log of every event), see [docs/WEBSOCKET.md](docs/WEBSOCKET.md); `setup.sh` switches it on, the Docker image serves `dashboard-ws` at `/live/`
- **WebSocket bridges and names (4.14.0)**: `initial_connect` has the bridges of each module (TG mappings, autolinks, connectors to other reflectors), a `module_mappings` event follows their changes, and the station events, last heard and open channels carry the operator's name from the ID database; the XML file takes its bridges from the same code (issue #2)
- **WebSocket links only real nodes (4.14.1)**: `links`, `client_added` and `client_removed` leave out the pseudo clients of the connectors and bridges (MMDVMClient, SvxReflector, DCS/DExtra/DPlus/YSF clients, USRP) and the clients of interlinked peers, like the XML file; the SvxReflector and USRP clients, set up by urfd itself, no longer show as linked nodes in the XML file either
- **SVXServer node listed twice (4.14.3)**: a node's client made before its UDP address was known stayed in the links next to the one with the address, and printing its unset address logged "CIp::GetAddress: unknown socket family=0"; a node gets its client only once the UDP address is known, and the server keeps it by pointer
- **M17 streams to @ALL (4.14.4)**: clients that send their voice to the broadcast destination @ALL (FF FF FF FF FF FF) instead of the reflector were dropped as "Unknown M17 packet", with "Callsign code is too large" for each frame; such a stream now goes to the module the client is linked on
- **M17 as on mrefd (4.15.0)**: mrefd interlinks by the Ham-DHT (`M17-XXX AB` without an address, urfd publishes its `[M17] Callsign` as an mrefd document); packet mode in last heard and `PARROT`/`ECHO` returns a packet straight to its sender; a client talks on the module it is linked on whatever the destination, and gets the streams addressed to @ALL; mrefd's callsign checks for CONN, LSTN and the data source; a client blacklisted after linking is disconnected; an mrefd's NACK is logged instead of dumped, and refused stream frames no longer dump each frame

### Transcoder (tcd)
- **In this repository**: tcd moved into `tcd/` with its history; it builds against the urfd sources it shares (`TCPacketDef.h`, `TCSocket.*`) from `../reflector`, so both sides always speak the same protocol
- **Serial access without libftd2xx**: AMBE devices through the kernel's `ftdi_sio` driver and `/dev/serial/by-id` (POSIX backend by Jason Kendall), RTS/CTS flow control, USB latency timer set to 1 ms; devices an older libftd2xx based tcd left unbound are bound back at startup; a lost device restarts tcd through systemd
- **Device selection**: `DeviceSerial` matches the USB serial, the libftd2xx form with the port letter, the by-id name or the device node exactly; `tcd --list-devices` shows them
- **Mixed mode, codec plugins, AGC and gains**: see [tcd/README.md](tcd/README.md)

### Infrastructure
- **Quieter log for scanners (4.22.0)**: a rejected connection is logged once per address and kind each hour, the rest summed up in one line
- **DMR talker alias in UTF-16 (4.10.2)**: our alias is UTF-16 like BrandMeister's, at most 13 characters; Motorola radios (R7) did not show the ISO 8 bit one
- **MMDVMClient talker change (4.10.2)**: a BrandMeister talker who starts while the module still holds the previous stream (it closes a moment after its last frame) joins as soon as the module is free, instead of being dropped for the whole call
- **Analog gains in urfd (4.10.0)**: `[USRP] RxGain/TxGain` set the AllStar levels in `urfd.ini`, like the SVX ones; tcd 0.3.2 dropped `UsrpGainIn`/`UsrpGainOut`, so urfd sets the gain per connection and tcd per codec
- **SvxReflector software name (4.9.0)**: `[SvxReflector] Software` sets the software name in the node info for reflectors that expect a particular one (default `urfd`); the node info JSON is escaped properly
- **SvxLink (4.8.0)**: the SvxReflector client selects one TG and monitors the other mapped ones, like an SvxLink node (it used to select every mapped TG, the reflector kept only the last one, and audio of any module went into it); the SVX server keeps one selected TG per node, relays audio between its nodes, sends talker start/stop only to nodes on or monitoring the TG and asks protocol 3 nodes to downgrade. Both: OPUS packets of 10 to 60 ms, lost packets concealed, late ones dropped, a lost flush no longer costs the next transmission, gain clips instead of wrapping around, 8 kHz voice encoder instead of upsampling, a fresh encoder per transmission, TCP messages in pieces, no lock-order deadlock between admin socket and protocol thread
- **Status file and admin socket (4.7.4)**: the XML/JSON status file is written aside and renamed into place, so the dashboard never reads it empty or half written; callsigns and `urfd.ini` texts in it are XML-escaped; the admin socket takes a command split over several TCP segments; a second SIGHUP during a config reload no longer ends urfd
- **XLX mode hotspots (4.7.1, 4.7.2)**: a module linked with a private call 4001-4026, as Pi-Star/WPSD's XLX master mode (DMRGateway) does, is served on the slot it was linked on and on TG9, like xlxd; DMRGateway's XLX network takes nothing else. A group call to 4001-4026 keeps its TG (4.7.1 answered it on TG9 too). The terminator of a link call no longer logs an orphaned frame
- **DMR talker alias (4.7.0)**: calls to hotspots on the MMDVM server carry a talker alias, the callsign and first name from the DMR ID database as BrandMeister makes it, or the alias the DMR source sends (BrandMeister, the radio) as soon as it arrived complete; since 4.10.1 only in every other superframe, so MMDVMHost can put its voice LC into the others for late entry
- **D-Star positions (4.6.0)**: DPRS positions in the slow data of D-Star radios (GPS-A, checked against its CRC, or NMEA `RMC`/`GGA`) show up as locator in the last heard list, the XML and `/json/stations`, like M17 GNSS positions; both are published as the middle of their locator field (about 4.6 × 2.3 km), not the exact position, which other D-Star stations on the module still get with the slow data; from every D-Star protocol and URF peers
- **CallingHome without a hash file (4.5.0)**: the reflector makes its directory hash at the first start and saves it as `[CallingHome] Hash` in `urfd.ini`; no hash file any more, `HashFile` is gone and `setup.sh` no longer touches the hash (suggested by Jason, VE3YCA)
- **CallingHome hash in urfd.ini (4.4.0)**: `[CallingHome] Hash` sets the directory hash directly (suggested by Jason, VE3YCA)
- **Hardening (4.3.5)**: DPlus and DMR+ check the packet size before reading it; the JSON report survives callsigns that aren't valid UTF-8; unknown packets are logged with their first 64 bytes only; an MMDVM login salt belongs to the DMR ID and the address that asked for it, and a config packet from a hotspot that isn't logged in gets no answer; `setup.sh` makes `urfd.ini` readable by root only and gives `xlxd-ch` to the dashboard instead of everyone (`update` fixes existing installations)
- **Security fixes (4.3.4)**: the SVX server takes UDP only from the address of the node's TCP connection (the client id is a counter, anyone could send audio as a node or take its audio over), gives a connection 30 s from connecting to log in and lets at most 8 wait at a time; the SvxReflector client takes audio only from its reflector; P25 frames shorter than their IMBE data are dropped (bytes past the packet went out as audio); the admin socket reads SVX nodes from a copy; Docker keeps at most 5 × 10 MB of log
- **Security fixes (4.3.3)**: only reflectors listed in `urfd.interlink` may peer (an empty file let anyone peer over URF and XLX); the MMDVM client accepts packets only from its master, which it looks up again at every login (any packet used to redirect the BrandMeister link, with the next password hash going to the sender); hotspot config is kept only for logged-in hotspots and read under a lock; the admin socket refuses logins for 2 s after a wrong password, makes 128-bit tokens and no longer dies on a failing command; no misleading `MMDVMPort` error at startup without `[MMDVM]`
- **CallingHome in the reflector**: `[CallingHome]` in `urfd.ini` registers the reflector in the XLX directory with the same data the dashboard sent, without needing the dashboard
- **Clean shutdown**: urfd stops within about 2 seconds on SIGTERM. Before, the module router threads waited for packets forever, protocols were deleted while their threads still ran (a crash, hidden because Docker killed urfd first) and the ID database threads slept up to 10 s each, so `docker stop` always ended in a kill
- **Transcoder link without Nagle delay**: `TCP_NODELAY` on both ends of the urfd–tcd connection. Before, a transcoded frame waited for the reflector's delayed ACK, which only came with the next DMR burst; a frame now returns after about 23 ms instead of 60 ms (URF363, measured at the reflector)
- **Setup script**: `setup.sh` installs and updates reflector and transcoder, writes the configuration from the templates and checks it with inicheck, and runs status, start, stop, restart, logs, reload and check; Docker compose builds from any checkout, data in `$URFD_DATA` (default `/opt/urfd`)
- **Echo module**: Built-in parrot with enable/disable flag
- **M17 LSTN support**: Listen-only M17 clients for monitoring services, which may not transmit
- **mrefd interlink**: Share modules with M17 reflectors (`M17-XXX` in `urfd.interlink`, `[M17] Callsign`), including legacy mrefd
- **URF interlink 4**: M17 META type and CAN in the URF stream header, negotiated per peer with fallback to 3; `[URF] InterlinkVersion = 3` for legacy mode
- **M17 compatibility**: V#3 TYPE field, packet mode (SMS) forwarding, `PARROT`/`ECHO` destination to the echo module, relinking replaces the old link, META type and CAN kept, GNSS position as locator
- **Transcoder resilience**: TCP keepalive, non-blocking poll, queue drain on disconnect, automatic reconnect after network outages
- **Database HTTP retry**: Retry with backoff on failed initial load (prevents empty databases after transient errors)
- **Database fallback sources**: `URL` accepts a comma separated list, tried in order; field separator (`;`, `,`, tab) is detected per line so the common publishers work unchanged; every successful download is mirrored to `<FilePath>.cache` and loaded when no source answers
- **Service optimizations**: Realtime scheduling and voice CPU priority; built with `-O2` since 4.7.3
- **Thread-safe logging**: Atomic cout via ostringstream
- **Thread-safe DB lookups**: DMR/NXDN ID database lookups in CodecStream hold proper mutex locks during access, preventing race conditions with background DB refresh
- **P25 cleanup**: Removed dead `P25_MODULE_ID` hardcode, P25 now uses `AutoLinkModule` config consistently. Guard debug dumps.
- **YSF fixes**: WiresX packet check moved before voice parser (CONN_REQ data frames were consumed as voice). CONN_REQ accepts room IDs 4001-4026 and direct index 1-26 (was forced to 0). DX_RESP status `'3'` (YSFGateway compat, was undocumented `'5'`). ALL_RESP byte `0x26` (was `0x29`). Fixed `YSFPayload::getSource()` null pointer (checked wrong member). Note: WiresX room switching requires direct YSF clients; MMDVMHost intercepts CONN_REQ locally. DG-ID is the primary module selection method.
- **DCS server fix**: Ignore "EEEE" status packets (35 bytes) from ircDDBGateway clients instead of logging as unknown. Accept 9-byte keepalive variant.
- **Cherry-picked from [dbehnke/urfd](https://github.com/dbehnke/urfd)**: Callsign sanitization, YSF radio ID collision fix, transcoder module ID enforcement, DG-ID module selection

## Copyright

- Copyright (c) 2016 Jean-Luc Deltombe LX3JL and Luc Engelmann LX1IQ
- Copyright (c) 2022 Doug McLain AD8DP and Thomas A. Early N7TAE
- Copyright (c) 2024 Thomas A. Early N7TAE
- Copyright (c) 2025-2026 Jens-Christian Merg DL4JC (urfd-ng extensions)
- tcd: Copyright (c) 2021 Thomas A. Early N7TAE; POSIX serial backend by Jason Kendall (2026); urfd-ng extensions Jens-Christian Merg DL4JC
