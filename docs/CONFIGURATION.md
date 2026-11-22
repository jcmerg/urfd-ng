# Configuration

**English** | [Deutsch](CONFIGURATION.de.md)

Each feature of urfd-ng with its settings in `urfd.ini`. The commented template [config/urfd.ini](../config/urfd.ini) lists every key; `inicheck` checks a file (see the [README](../README.md#inicheck)). The transcoder's `tcd.ini` is described in [tcd/README.md](../tcd/README.md).

- [Basics](#basics)
- [XLX directory registration (CallingHome)](#xlx-directory-registration-callinghome)
- DMR: [MMDVM server](#mmdvm-server), [MMDVMClient](#mmdvmclient), [OpenBridge](#openbridge), [DMR+](#dmr), [dynamic talkgroups](#dynamic-talkgroups)
- D-Star: [client connectors](#d-star-client-connectors-dcs-dextra-dplus), [slow data](#d-star-slow-data-for-transcoded-streams), [G3](#g3)
- [YSF](#ysf), [M17](#m17), [NXDN](#nxdn), [P25](#p25)
- SvxLink: [SvxReflector client](#svxreflector-client), [SVXServer](#svxserver); [USRP (AllStar)](#usrp-allstar)
- [Blocking protocols](#blocking-protocols), [reflector interlinking](#reflector-interlinking), [echo module](#echo-module), [plugins](#plugins)
- [Admin interface](#admin-interface), [credential database](#credential-database), [WebSocket](#websocket), [dashboard](#dashboard)
- [SIGHUP configuration reload](#sighup-configuration-reload)

## Basics

### Names and modules

```ini
[Names]
Callsign = URF999              # URF and three letters/digits (required)
SysopEmail = me@example.com    # shown in the dashboard's footer
Country = DE
Sponsor = My Club
DashboardUrl = https://my-reflector.example.com

[Modules]
Modules = ADFRSZ               # up to 26 modules, A-Z
DescriptionA = All Modes       # shown in the dashboard
TGD = 262                      # module talkgroups, see below
TGF = 26363
```

**Module talkgroups**: `TG<M> = <number>` gives a module its talkgroup once (each number on one module). The DMR connections take it from there: `TS1 = <modules>` / `TS2 = <modules>` in an MMDVMClient connection or the MMDVM server, `TGs = auto` in the MMDVM server, OpenBridge and SVXServer, and plugins (`CConfigure::GetModuleTG`). `TS1 = F` is the same as `TG26363 = F,TS1` with `TGF = 26363`. TG lines keep working next to it and win (e.g. a second, RX only TG on a module).

### Transcoder

```ini
[Transcoder]
Port = 10100                   # 0 = no transcoder
BindingAddress = 127.0.0.1     # 0.0.0.0 for a tcd on another host
Modules = auto                 # or the letters of the transcoded modules
# Password =                   # the same as Password in tcd.ini, for a tcd on another host
```

With `Modules = auto` a module is transcoded while a tcd is connected for it, so the list lives in `tcd.ini` alone and a module added there needs no change and no restart of urfd. A stream on a module without tcd passes as on a module without transcoding: it goes only to the protocols of its own codec (D-Star to D-Star; AMBE+2 to DMR, NXDN and YSF; IMBE to P25 and YSF; Codec2 to M17; SVX and USRP audio to SVX and USRP). Interlinks and plugins get every stream.

A tcd on another machine logs in with `Password`, so the port can face the internet; `setup.sh` generates one. A second tcd can stand by for modules (`StandbyModules` in its `tcd.ini`): it takes them over while their tcd is gone, e.g. a software tcd in Docker next to urfd for the one with the AMBE sticks. A tcd that stops answering for 20 s, or blocks a send or receive for 10 s, is disconnected, so a standby can take over; see [tcd/README.md](../tcd/README.md).

### Enabled protocols

Every protocol can be switched on or off. Without `Enable`, DCS, DExtra, DPlus, DMRPlus, M17, YSF and URF are on (NXDN and P25 when they have a `Port`); MMDVM, OpenBridge, SvxReflector, SVXServer, Echo, Admin and the client connectors are off; XLXPeer, G3 and USRP need the line. A disabled protocol still needs its `Port`.

```ini
[DPlus]
Enable = false
Port = 20001
```

### External address

Clients and directories need the address urfd is reached at on the internet, which behind NAT is not the bound one. `IPv4External` / `IPv6External` set it by hand. Without them urfd asks the services in `IPCheckUrl` (by default icanhazip.com, ifconfig.me, ipify.org and ident.me; the first well-formed answer wins), at start and every `IPCheckInterval` seconds (default 300, 0 = only at start). A changed address, e.g. after a switch to a fallback line, counts when a second check 10 s later confirms it; it is logged, goes to the XLX directory at once and into the DHT, and G3 and the plugins tell it to their clients. A check without answer changes nothing. When no service answers at the start (e.g. no DNS yet right after a boot), urfd starts with a warning and asks every 10 s until one does.

```ini
[IP Addresses]
IPv4Binding = 0.0.0.0
# IPv6Binding = ::                # dual-stack
# IPv4External = 203.0.113.10     # fixed: not detected
IPCheckInterval = 60              # check every minute
```

### ID databases

The DMR ID, NXDN ID and YSF TX/RX databases give callsigns, names and frequencies.

```ini
[DMR ID DB]
Mode = http
FilePath = /usr/local/etc/urfd/dmrid.dat
# comma separated: the first is primary, the rest are tried in order
URL = https://radioid.net/static/user.csv, http://www.pistar.uk/downloads/DMRIds.dat, http://xlxapi.rlx.lu/api/exportdmr.php
```

- **Several sources**: tried in order until one answers; a fallback is logged. A fallback adds to the database urfd has and leaves the cache alone: a mirror lags behind the first source, so IDs only that one has stay until it answers again.
- **Formats**: fields may be separated by `;`, `,` or a tab, so radioid.net CSV, Pi-Star `DMRIds.dat` and xlxapi all work; lines not starting with an ID (a CSV header) are skipped. xlxapi carries no names, so it comes last.
- **Cache**: every complete download is kept as `<FilePath>.cache`. At start the cache is loaded first and replaced when a download is complete; with no source answering it stays, so the database is never empty because a directory is slow or down. With `Mode = both`, `FilePath` holds your own additional entries and is never overwritten.
- No second publisher of the YSF TX/RX data is known, so there the cache is the only fallback; missing YSF frequencies only affect Wires-X frequency reporting.
- NXDN IDs not in the NXDN database are looked up in the DMR database (many operators use their DMR ID for NXDN).

### Module access by protocol

`[Modules] Protocols<M> = <protocols>` opens module M to the protocols listed and closes it to all others, for example a module apart from the amateur radio network that only the protocols of a closed user group use. A module without the line is open to every protocol.

```ini
[Modules]
Modules = ADP
DescriptionP = Closed group
ProtocolsP = MMDVM,M17     # module P: these protocols only
HideUnlisted = P           # talkers without an ID database entry stay off the dashboard
```

- **Names**: as in [BlockProtocols](#blocking-protocols), comma or space separated. `MMDVMClient` means all its connections, one connection cannot be named here. An unknown name stops urfd, as an error in `urfd.ini`.
- **Link**: a DExtra, DCS, M17 or G3 link to the module, a Wires-X connect and an MMDVM module change by TG are refused (`Module P is closed to DCS, link refused`).
- **Send**: a stream from a closed protocol is not taken, however it was addressed (TG, DG-ID, connector); the station does not appear in last heard.
- **Receive**: the module's streams go to the open protocols only; interlinks get nothing unless `URF` or `XLXPeer` is listed.
- **Connectors**: a client connector or an autolink on a closed module stays linked but carries nothing; list its protocol to use it there.
- **Reload**: SIGHUP reads the lines again; a station linked before keeps its link but hears and sends nothing on a module now closed to it.

`HideUnlisted = <modules>` keeps the talkers on these modules that are in neither ID database (DMR, NXDN) off the dashboard: no last heard entry, no on air, no position, in the WebSocket as in the admin's data, as if the module were quiet. Listed talkers are shown. A peer that has the module open (`URF`, `XLXPeer`) gets the streams and shows them on its own dashboard, so keep interlinks off such a module. The admin page still shows who talks.

## XLX directory registration (CallingHome)

The reflector registers itself in the XLX directory, which lists it in Pi-Star, WPSD and other hotspot software. The dashboard is not needed for that.

```ini
[CallingHome]
Enable = true
Country = Germany              # country name as shown in the directory
Comment = My URF Reflector     # max 100 characters
# Hash =                       # letters and digits; blank = made at first start
# OverrideIP =                 # blank = the directory uses the address it sees
# PushDelay = 300              # seconds between updates
```

It sends the name `XLXnnn`, uptime, `DashboardUrl` from `[Names]`, country, comment, version and interlinks. Nothing is sent unless `Enable = true`. The settings are read at startup.

The hash identifies the reflector in the directory, and the directory ties `XLXnnn` to the first hash it sees. The directory hands out no hashes: with `Hash` blank, the reflector makes one at its first start and saves it as `Hash = ...` in `[CallingHome]` of its `urfd.ini`, the only line it ever writes there. If it cannot write `urfd.ini` (mounted read-only, say), it does not register and says so in the log. Keep the hash from then on.

### Moving a reflector that is listed already

A reflector that was registered before, by an XLX dashboard or another urfd, has to keep its hash; the directory would not accept a new one for the same name. Set it before the first start with `Enable = true`.

1. Check whether your name is in the directory (here `XLX999`, the number of your `URFnnn`):
   ```bash
   curl -s 'http://xlxapi.rlx.lu/api.php?do=GetReflectorList' | grep -A6 '<name>XLX999</name>'
   # without curl:
   wget -qO- 'http://xlxapi.rlx.lu/api.php?do=GetReflectorList' | grep -A6 '<name>XLX999</name>'
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

## MMDVM server

Hotspots and repeaters connect directly (HomeBrew DMR protocol, RPTL→RPTK→RPTC→RPTO), no BrandMeister needed. Pure DMR works without transcoder; transcoded modules are needed for other modes. Each timeslot links to its own module, so a hotspot can be on two modules at once.

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

**Users**: a user logs in with a DMR ID or a callsign (resolved through the DMR ID database at startup). BrandMeister-style hotspots add a 2-digit suffix to the 7-digit ID (`123456701`); one entry with the base ID covers all of them. An empty password accepts any password (for hotspots with a fixed one). `RequireAuth = false` accepts every hotspot. Users can be added at runtime on the admin page or come from the [credential database](#credential-database).

**Talkers without DMR ID**: a talker that is in no ID database (AllStar, a D-Star or M17 callsign) reaches the hotspots, DMR+ and P25 only with `FallbackDmrId`; otherwise the stream is dropped for them, as for the DMR networks and NXDN.

**Talkgroups**: as with MMDVMClient, the first TG of a module is the primary (TX/RX), further ones are secondary (RX only). Map modules to different timeslots so a hotspot hears both (the dashboard shows e.g. "S TS1 D TS2"). A hotspot that reconnects from another port (NAT) keeps its modules.

**Static talkgroups**: a hotspot names its talkgroups in its options, as for BrandMeister or FreeDMR, and gets them at every login without keying up: `TS1=26363;TS2=262,2625` links the modules of TG 26363 on TS1 and of TG 262 and 2625 on TS2 (`TS1_1=…;TS1_2=…` as BrandMeister writes it works too; other keys such as FreeDMR's `DIAL` are left alone). In MMDVMHost the line is `Options=` in `[DMR Network]`, in DMRGateway `Options=` in the network's section. A TG that maps to no module, a module closed to MMDVM or a module given twice is left out and logged.

A timeslot can carry several modules, one call at a time: the first call takes the slot, a call on another of its modules meanwhile does not go out, and after the last packet the slot stays with that module for 5 s so the answer gets through (the hotspot's own call holds its slot the same way). Keying up on another TG adds that module as the slot's dynamic one (the previous dynamic one goes, the static ones stay); TG 4000 drops the dynamic one. A TG that maps to no module goes to the module the slot is busy with, else the dynamic one, else the first static one.

**Dynamic module timeout**: as on BrandMeister, a slot's dynamic module unlinks after `DynamicTimeout` minutes (default 5, 0 = never) without a call of the hotspot on it. Only the hotspot's own calls on that module start the time again, counted from the end of the call; incoming calls do not. A call going out on the slot when the time runs out ends first. Static modules never expire. The WebSocket's links and the admin's `mmdvm_peer_list` give `ts1_dynamic` / `ts2_dynamic`, `ts1_expires` / `ts2_expires` (unix time) and `ts1_left` / `ts2_left` (seconds), and the event `client_updated` follows the changes; the dashboard's Links page counts down next to the module.

**TGs of the MMDVMClient links**: a group call on a TG the server does not map goes to the module an MMDVMClient link booked it on as its primary TG on the admin page (e.g. TG91 → A on the BrandMeister link: a hotspot keying up on TG91 links A as its dynamic module and hears it on TG91). Not a secondary TG, not the links' TGs from `urfd.ini` (map those in `[MMDVM]`), not a module closed to MMDVM and not a TG two links booked on different modules. The server's own TGs win.

**Talker alias**: every call reaches the hotspots with a talker alias as from BrandMeister: callsign and first name from the DMR ID database (`N0CALL Max`), at most 13 characters in UTF-16, which Motorola radios need. An alias the DMR source sends (BrandMeister, the radio) is passed on once it is complete, re-encoded in UTF-16. It goes in every other superframe, so MMDVMHost can put its voice LC into the others for radios that join late. The service options in the voice LC (OVCM, emergency, privacy) are always 0.

**Calls without voice**: some devices key up on their own after a talkgroup change with about 3 s of all-zero voice frames. A call opens its stream only with its first real voice frame; one without any is dropped and logged (`call from ... without voice ... ignored`). The slot still follows the talkgroup, so switching by keying up works.

**Hotspot setup**: point the hotspot at the reflector's address, port 62030. In WPSD/Pi-Star use "MMDVM_BRIDGE", single-network mode, or a custom network of DMRGateway. DMRGateway passes a network only what its rules send there: give the reflector's section `PassAllTG=1`, `PassAllTG=2`, `PassAllPC=1` and `PassAllPC=2` instead of rewrites, so the radio dials the reflector's talkgroups as they are. WPSD's custom network writes rewrites of 9000001-9999999 (the radio dials 9000000 + TG); turning off `WPSD_AutoRewrites` leaves those lines in `/etc/dmrgateway`, replace them by hand (Configuration → Expert → DMR GW). An empty `Address=` in a DMRGateway network makes DMRGateway end at its start, and the hotspot logs out of the reflector.

**XLX master mode** (WPSD/Pi-Star "XLX Master" with a startup module) works too: DMRGateway links the module with a private call to 4001-4026 and gets the module on TS2/TG9, which it maps to the radio's slot and TG (TG6 by default). DMRGateway logs in with the fixed password `passw0rd` from `[XLX Network]` in `/etc/dmrgateway`. Either put the user's urfd password there (WPSD/Pi-Star: Configuration → Expert → DMR GW; check it after saving the main configuration page, which may write `passw0rd` back) or give the user an empty password in `[MMDVM]`. Otherwise the reflector logs `auth FAILED`.

**Admin commands**:
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

`mmdvm_peer_list` gives each hotspot with its `module` (the first one) and `ts1` / `ts2`: every module of that timeslot, the static ones first (left out when the slot has none).

## MMDVMClient

urfd connects to a DMR master server (e.g. BrandMeister) as a hotspot and maps talkgroups to modules per timeslot. Dynamic TGs can be added at runtime on the admin page.

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

**Several TGs per module**: the first TG of a module is the primary and carries what is said on the module out to the master (TX). Further TGs on the same module are secondary and only bring traffic in (RX). Dynamic TGs added on the admin page are secondary unless the module has no primary yet.

**FallbackDmrId**: used for a talker whose callsign has no DMR ID. Without it (or 0) the stream is not sent to the master.

**BrandMeisterApiKey** (optional; BrandMeister SelfCare → Profile → Security → API Keys): with it, urfd syncs the static TGs at startup (removes those not in the config, adds missing ones), registers a dynamic TG as static on BrandMeister and removes it there again when it is removed or expires. Without it urfd activates a TG by a short transmission (kerchunk).

**Several connections**: further sections next to `[MMDVMClient]`, at most 9. Each is a hotspot of its own with two timeslots and its own TG map, so more modules can talk to BrandMeister at the same time, or the reflector joins other networks (TGIF, FreeDMR, ...). Use a hotspot ID of its own per connection (your DMR ID with the suffixes `01`, `02`, ...); one BrandMeister hotspot password covers all of them.

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

- **Names**: `[MMDVMClient.<name>]`, the name of letters, digits, `-` and `_`. A named connection takes what it leaves out from `[MMDVMClient]`: master, port, password, callsign, `FallbackDmrId`, `Software`, `Firmware`, the location and frequency keys, `Network` with the master, and in that network the `BrandMeisterApiKey` (it covers all hotspots of the account; BrandMeister ignores the TGs of the login options, only the API makes them static). `LocalPort`, `BlockProtocols` and `Enable` stay per connection. A `DmrId` of one or two digits is a suffix to the `DmrId` of `[MMDVMClient]`, a longer one the whole ID; without a `DmrId` a name of digits is the suffix (`[MMDVMClient.50]` is hotspot `<DmrId>50`). `[MMDVMClient]` with TGs of its own is a connection too. The numbered `[MMDVMClient2]` to `[MMDVMClient9]` work as well; a file uses named or numbered sections, not both. Adding, removing or reordering connections needs a restart.
- **Network**: every connection belongs to the network in its `Network` key. Without it every BrandMeister master (`*.brandmeister.network`) is `brandmeister.network`, any other master a network of its own (its host). Set `Network` for two masters of another network, or for a BrandMeister master given by its IP address.
- **Bridging**: a call never goes back to the connection it came from, nor to another connection in the same network. Between networks a module that both map is bridged; `BlockProtocols = MMDVMClient.TGIF` switches that off for this pair.
- **Protocol names**: in `BlockProtocols` and the admin `block`/`unblock` commands `MMDVMClient` means every connection, a connection's name (`MMDVMClient.TGIF`, or `MMDVMClient1` to `MMDVMClient9` for numbered ones; `MMDVMClient1` is `[MMDVMClient]`) one of them.
- **Checks**: `inicheck` refuses a TG or a hotspot ID used by two connections of one network; the admin page refuses such a dynamic TG.
- **Modules without transcoding**: an MMDVMClient TG may sit on a module tcd does not transcode. Only AMBE+2 streams (DMR, YSF, NXDN) go to the master from there; D-Star, M17, SVX and the others stay local, they would reach the network as silence.
- **Admin**: `tg_add`, `tg_remove`, `reconnect` and `kerchunk` take `"client": 2` or `"client": "MMDVMClient.50"`; without it they mean `[MMDVMClient]`, for a TG the connection that has it, and `reconnect` all of them. `tg_list` and `status` list every connection with its network.

**Echoes from the master**: BrandMeister sends after many calls a second stream from the same ID 0.6 to 1.2 s later with the call's last second again. urfd recognises it by its voice frames and drops it (`echo of the end of the call from ... dropped`), see [docs/AUDIO.md](AUDIO.md#echo-of-a-calls-end-from-the-master).

## OpenBridge

Links urfd server to server with a DMR network: HBlink3, HBlink4, FreeDMR and BrandMeister (a bridge to BrandMeister needs a ticket and their review). There is no login and no timeslot: every talkgroup the link maps runs at the same time as the others, one call per module, so one link carries as many calls as it has modules mapped. Both ends configure the link: the other end needs urfd's address, `LocalPort`, `NetworkId` and the passphrase. See `[OpenBridge]` in [config/urfd.ini](../config/urfd.ini).

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

- **Dialects**: `classic` (the default) is what HBlink3, HBlink4 and BrandMeister speak: the 53 bytes of an MMDVM DMRD packet with the slot bit clear and `NetworkId` as the repeater ID, followed by an HMAC-SHA1 with the passphrase. `freedmr` is FreeDMR's protocol version 5, which FreeDMR uses by default and which refuses classic packets: DMRE packets with a timestamp, source server and repeater and a hop count, keyed with BLAKE2b, plus keepalives every 10 s, the version every 60 s, source quench and stun. A classic link turns `freedmr` (until a restart) when the other end shows it speaks that; `AutoUpgrade = false` keeps it classic. Set `Dialect = freedmr` for a FreeDMR end all the same: until its first keepalive FreeDMR drops classic packets, and a call in that time loses its start.
- **IDs**: `NetworkId` goes into every packet, as the `NETWORK_ID` the other end has for urfd. FreeDMR takes it as the source server of urfd's calls too (`SourceServer` sets another) and checks it: a server ID of its list (4 or 5 digits) or a DMR ID it knows (6 or 7 digits), else it drops the calls; use a DMR ID registered to you. `PeerId` makes urfd take packets with that repeater ID only (HBlink's `NETWORK_ID`, FreeDMR's `SERVER_ID`). `PreserveSourcePeer = true` (classic only) puts the ID of the hotspot a call comes from there instead, as HBlink4's `preserve_source_peer`; BrandMeister and FreeDMR expect `NetworkId`. HBlink4 does that by default: with a `PeerId` for an HBlink4 end set its `preserve_source_peer = false`, else urfd drops its calls.
- **Passphrase**: HBlink3 and FreeDMR use its first 20 characters only, HBlink4 all of it: keep it at 20 or fewer. urfd warns about a longer one.
- **Talkgroups**: `TG<number> = <Module>`, `TGs = auto` for every module with a talkgroup in `[Modules]`, or `Modules = <modules>` for some of them; TG lines win. A talkgroup on a module without transcoding carries DMR, YSF and NXDN only. Talkgroups the link does not map are dropped both ways, private calls too. FreeDMR drops TGs up to 79 and some ranges above on its own.
- **Network**: the link belongs to a network like an MMDVMClient connection: its target's host, `brandmeister.network` for a BrandMeister host, or `Network`, which a target given by its IP address needs. It shares no TG with a connection in that network, and calls never go between them; between networks a module both map is bridged, `BlockProtocols` switches that off.
- **Checks**: packets come from the target's address only (any port) and must pass their HMAC or BLAKE2b. FreeDMR packets older than 5 s or with more than 20 hops are dropped and quenched: the clocks need NTP. A call urfd sent that comes back, or a stream that ended, is dropped. A target that does not resolve is looked up again every 10 s.
- **Firewall**: the other end sends to `LocalPort` (default 62035), open it for UDP. IPv4 only.
- **Admin**: the admin page lists the link's TGs (from `urfd.ini`, a SIGHUP reloads them; no dynamic ones) and `status` has an `openbridge` entry: target, network, dialect, when the other end was last heard, whether it stunned the link.

## DMR+

DMR+ (DMO mode) for hotspots and repeaters on UDP 8880. Modules are reached by TG 4001-4026 (A-Z), shown as "DMR+ TG" on the dashboard's Modules page.

```ini
[DMRPlus]
Port = 8880
```

## Dynamic talkgroups

A TG added on the admin page (MMDVMClient, MMDVM server, SvxReflector, SVXServer) expires after its timeout (`ttl`, the admin page's "Timeout in s", default 900 s) without activity. Any transmission start on the module, from whatever protocol, starts the timer again with that timeout, so activity in any mode keeps all of the module's TGs. An MMDVMClient TG added through the BrandMeister API is removed there again when it expires. A hotspot's dynamic module is something else, see `DynamicTimeout` under [MMDVM server](#mmdvm-server).

- **BrandMeister** keeps its own subscription timer. When it expires there, urfd's mapping expires too without further activity. The **kerchunk** admin command extends the BrandMeister side without a radio.
- **SvxReflector**: an expired TG is taken off the monitored TGs; if it was selected, the home TG is selected again.

## D-Star client connectors (DCS, DExtra, DPlus)

urfd connects to other D-Star reflectors as a client node and maps a remote module to a local one. Mappings are static in `urfd.ini` or added at runtime on the admin page.

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

**Map format**: `host,port,remote_module,local_module`. Each mapping is one connection to the remote reflector's module.

**Routing**: the connector addresses the reflector as a gateway linked to it does: RPT1 is `<Callsign> <local module>`, RPT2 the reflector and the remote module (`DCS002 A`). The reflector's callsign is taken from the streams it sends, until then from the host name (`dcs002.xreflector.net` → `DCS002`). A DPlus reflector sends the streams of all its modules to every client: only those of the mapped module reach the local module.

**Keeping the link** (as ircDDBGateway does): the DCS connector answers every poll of the reflector at once and names the reflector in its link and unlink packets. A link is taken as lost after 60 s without a poll (DCS, DExtra; DPlus 30 s); the connector then tries again after 5 s, 10, 20, 40 and every 60 s. Before each try the host is looked up again; if DNS fails the last address is used and `cannot resolve <host>` logged once.

**Callsigns**: DPlus reflectors require registration at [dstargateway.org](https://regist.dstargateway.org/). DExtra and DCS reflectors are typically open.

**Admin commands**:
```json
{"cmd": "dcs_map_add", "token": "...", "host": "dcs002.xreflector.net", "port": 30051, "remote_module": "A", "local_module": "S"}
{"cmd": "dcs_map_remove", "token": "...", "local_module": "S"}
{"cmd": "dcs_map_list", "token": "..."}
{"cmd": "dextra_map_add", "token": "...", "host": "xrf757.openquad.net", "port": 30001, "remote_module": "A", "local_module": "A"}
{"cmd": "dplus_map_add", "token": "...", "host": "ref001.dstargateway.org", "port": 20001, "remote_module": "A", "local_module": "Z"}
{"cmd": "reconnect", "token": "...", "protocol": "dcsclient"}
```

## D-Star slow data for transcoded streams

Streams from other modes (DMR, YSF, SVX, M17, P25, USRP, the client connectors) reach D-Star radios with proper slow data:

- **Header**: caller callsign (MY), reflector callsign (RPT1/RPT2). The source mode is the caller's 4 character suffix (`N0CALL / DMR`), which D-Star radios show and keep in their call list: `DMR`, `YSF`, `NXDN`, `P25`, `M17`, `USRP`, `SVX`, or a plugin's type. Only D-Star gets the suffix; the dashboard, the logs and the other modes show the callsign alone.
- **Text message**: the operator's first name (if the callsign is in the DMR or NXDN ID database) and the source, e.g. `Max TG26363`.

| Source | Slow data text |
|--------|---------------|
| MMDVMClient | `Max TG26363` |
| SvxReflector | `Max TG317424` |
| YSF (server) | `Max DG28` |
| NXDN | `Max RAN6` |
| YSFClient | `Max DG15` |
| M17, P25, USRP, DMR+ | `Max` (only the name) |

YSFClient shows the DG-ID if configured. Without a name and without TG, DG-ID or RAN the text stays empty. D-Star sources, the D-Star client connectors among them, are not transcoded to D-Star: their own slow data passes through unchanged.

The text has 20 characters. When `<name> <source> TG<n>` is longer (e.g. for a caller with a suffix of their own, where the protocol stays in the text), the TG moves to a second text that takes turns with the first every ~5 seconds; when it is still too long, the name is shortened so the source stays whole:

```
Max TG262                                      (all fits)
Maximilian TG263112                            (all fits)
Maximilian DMR    <-> DMR TG263112             (own suffix: the TG takes turns)
```

Header and text message alternate every superframe (~420 ms):

```
[Header: N0CALL > URF999 S] → [Max TG262] → [Header] → [Max TG262] → ...
```

**Positions**: DPRS positions in the slow data of D-Star radios (GPS-A, checked against its CRC, or NMEA `RMC`/`GGA`) show up as a locator in last heard. The dashboard gets the middle of the locator field (about 4.6 × 2.3 km), not the exact position, which other D-Star stations on the module still get with the slow data.

## G3

Icom Terminal and Access Point mode (UDP 12345, 12346, 40000), `[G3] Enable = true`. `urfd.terminal` configures it and is reloaded when it changes:

- `address <public-ip>`: optional; without it a terminal on the internet gets urfd's external IPv4 behind NAT
- `modules <letters>`: the modules terminals may use
- `alias <callsign>`: a further reflector callsign accepted from terminals (e.g. both `URFxxx` and `XLXxxx`); repeatable

## YSF

URF is a YSF master providing Wires-X rooms, one per module. Hotspot software (Pi-Star/WPSD) finds URF reflectors through the XLX directory; no registration at ysfreflector.de is needed.

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

- **DG-ID**: with `EnableDGID = true` users switch modules by DG-ID, and the reflector sends each module with its DG-ID, so radios that filter on it hear the module.
- **DN and VW**: DN and V/D mode 1 are taken like DMR's AMBE+2. VW (Voice Wide, IMBE full rate) is P25's codec: the transcoder makes the other modes from it as from P25, P25 listeners get the very frames, YSF listeners on a transcoded module get DN. On a module without transcoder an IMBE stream (VW or P25) goes to YSF as VW. VW speech reaches the other modes about 85 ms later than DN. The YSF client connector carries no VW.
- **Positions**: the GPS position Yaesu radios send in the data channel shows up as a locator in last heard, like D-Star and M17 positions.

### YSF client connector

urfd connects to other YSF reflectors as a client node, each connection on a local module, optionally with a DG-ID for reflectors that select rooms by it.

```ini
[YSFClient]
Enable = true
Callsign = N0CALL
Map1 = 203.0.113.20,42099,A             # YSF reflector -> local module A
Map2 = 198.51.100.30,42000,F,15          # YSF reflector -> local module F, DG-ID 15
# BlockProtocols = MMDVMClient            # optional
```

**Map format**: `host,port,local_module[,dgid]`; DG-ID 0 = none. When set, it goes into the FICH header of every outgoing voice packet. Outgoing packets carry the reflector callsign (e.g. `URF999`) in the radio field, shown as "Radio" on YSF dashboards.

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

- **Module selection**: a client picks its module when it links: the `CONN` (or `LSTN`) packet carries the module letter. To change modules, link again with the other letter, e.g. `URF999 B` as the reflector in Pi-Star/WPSD, the module selector in DroidStar or mvoice, or `URF999 B` as the destination on an M17 radio. A new link replaces the old one of the same callsign and IP. As on mrefd, a transmission goes to the module the client is linked on, whatever its destination (`URF999 B`, `M17-XXX B` or @ALL); streams to the clients are addressed to @ALL.
- **Echo**: `PARROT` or `ECHO` as destination reaches the [echo module](#echo-module) from any linked module; a packet mode packet to them comes straight back to its sender.
- **Listen-only**: `LSTN` clients are registered on their module, which lets monitoring services such as dvref.com check the reflector. Their voice and data are dropped.
- **Voice and data**: voice frames in both the legacy and the V#3 TYPE layout are taken, encrypted streams dropped. Packet mode (`M17P`, e.g. SMS) is checked against both CRCs and forwarded unchanged to the other M17 clients on the module; the sender shows in last heard.
- **META**: text, GNSS and ECD reach the other M17 clients with their TYPE and the source's CAN. A GNSS position is shown as a locator in last heard, linked to OpenStreetMap.
- **Callsigns**: as on mrefd, a `CONN`, an `LSTN` (more permissive) and the source of voice and packet data must look like a callsign, anything else is dropped. A client put on the blacklist after linking gets a `DISC` and is removed.
- **mrefd interlinks**: see [Reflector interlinking](#reflector-interlinking).

## NXDN

Modules are selected by the NXDN RAN (Radio Access Number) or by the destination ID.

```ini
[NXDN]
Port = 41400
AutoLinkModule = S             # module for RAN 0 and RAN 1 (comment out to disable)
ReflectorID = 26363
# FallbackNxdnId = 8970        # NXDN ID for unknown callers (0 or omit = drop)
ModuleDstIdBase = 4000         # dstId 4001-4026 = A-Z (omit to disable)
```

**By RAN**: RAN 1-26 is module A-Z, with RAN 1 and the AutoLinkModule's own RAN swapped: RAN 1 always reaches the AutoLinkModule, and module A takes the AutoLinkModule's RAN. RAN 0 goes to the AutoLinkModule as well. With `AutoLinkModule = S`:

| RAN | Module | RAN | Module | RAN | Module |
|-----|--------|-----|--------|-----|--------|
| 0   | S      | 10  | J      | 19  | **A**  |
| 1   | **S**  | 11  | K      | 20  | T      |
| 2   | B      | ... | ...    | 26  | Z      |

The swap is there because RAN 1 is all most clients can send: DroidStar hardcodes it, and MMDVMHost replaces the radio's RAN with the hotspot's `[NXDN] RAN` (default 1) and rejects RF with another one. Switching modules by RAN from the radio therefore only works on a direct connection; through a hotspot the RAN is a per-hotspot setting.

**By destination ID** (wins over the RAN): with `ModuleDstIdBase` set, dstId `base+1` to `base+26` is module A-Z, any other dstId falls through to the RAN. It is read in the connect poll and in every frame. With `ModuleDstIdBase = 4000` the NXDN destination IDs line up with DMR+ (4001 = A, 4002 = B, …). This is the only module selection for clients that cannot set a RAN; DroidStar users add one custom host per module, the number is the dstId:

```
NXDN 4001 my.reflector.org 41400     -> Module A
NXDN 4003 my.reflector.org 41400     -> Module C
NXDN 26363 my.reflector.org 41400    -> AutoLinkModule
```

Outgoing frames carry the destination ID each client polled with: `NXDNGateway` drops every frame whose dstId differs from the TG the hotspot is linked to.

**IDs**: incoming NXDN IDs are looked up in the NXDN ID database, then in the DMR one. Unknown IDs are dropped unless `FallbackNxdnId` is set. D-Star radios see `RAN<n>` in the slow data text.

**Dashboard**: the NXDN RAN column shows when NXDN is enabled, an `NXDN DST-ID` column next to it once `ModuleDstIdBase` is set.

## P25

P25 for P25Gateway on UDP 41000. `ReflectorID` is the TG P25Gateway links to (required), `AutoLinkModule` the module it lands on. Talkers without a DMR ID reach P25 only with `[MMDVM] FallbackDmrId`.

```ini
[P25]
Port = 41000
AutoLinkModule = A
ReflectorID = 12345
```

## SvxReflector client

urfd connects to SvxLink SvxReflector servers (e.g. FM-Funknetz) and bridges FM audio both ways: TCP for signalling, UDP for Opus audio. Needs transcoded modules. Dynamic TGs can be added on the admin page.

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

**One TG at a time**: an SvxReflector node has one selected TG; it gets that TG's audio only, and its audio goes there. urfd selects the first static TG (the lowest number, the home TG) and monitors all mapped TGs (like SvxLink's `MONITOR_TGS`). A talker on a monitored TG selects that TG when there has been no traffic for 3 s; audio from a module whose TG isn't selected selects it unless a talker is on the selected one, then that transmission is not sent. After 30 s without traffic urfd goes back to the home TG.

**RxGain / TxGain**: static gain per connection. RxGain applies after Opus decoding before the transcoder, TxGain after the transcoder before Opus encoding; samples past full scale are clipped. tcd's AGC for analog sources sets the level after RxGain (it has its own gate at -40 dBFS, see [tcd/README.md](../tcd/README.md)), so keep RxGain near 0; a strongly negative RxGain puts quiet FM speech below the gate.

**Software**: the name urfd reports in its node info (`sw`), `urfd` by default. Set it only in agreement with the reflector's operator.

When the link drops, audio waiting to go out is discarded rather than sent late, and audio going out ends with a flush, so the server does not hold the talker.

## SVXServer

urfd is a SvxReflector server for SvxLink nodes (protocol 2, HMAC-SHA1 login, Opus audio). Needs transcoded modules. Users and TGs can be managed at runtime on the admin page.

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

**Users**: statically in `urfd.ini`, on the admin page or from the [credential database](#credential-database). Each node logs in with callsign and password (HMAC-SHA1).

**TG selection, as on SvxReflector**: a node has one selected TG (`MsgSelectTG`) and the TGs it monitors (`MsgTgMonitor`). A TG has one talker at a time. The other nodes on the TG get the talker's Opus packets as they came (no transcoding), also on TGs not mapped to a module; mapped TGs also go to and come from their module. Nodes must speak protocol 2.0, which SvxLink with an `AUTH_KEY` does, current versions included. A node set up for certificates only uses protocol 3 (TLS), which urfd does not support: it is asked to downgrade and urfd logs that it can't.

**Setting up a SvxLink node**, in `[ReflectorLogic]` (example for TG26363 on a urfd module):
```ini
HOSTS = urf.example.org:5300    # instead of DNS_DOMAIN
CALLSIGN = N0CALL-HS            # the user in [SVXServer]
AUTH_KEY = changeme             # its password
DEFAULT_TG = 26363              # selected on local traffic
MONITOR_TGS = 26363             # heard while idle, and the module urfd links the node on
```
- With `DEFAULT_TG` set, SvxLink mutes the first transmission after it fell back to TG 0 (`TG_SELECT_TIMEOUT`, 30 s by default): `MUTE_FIRST_TX_LOC` is on by default and drops that transmission to suppress test keyups and DTMF commands. The second one goes through; `MUTE_FIRST_TX_LOC = 0` sends the first one too.
- Level: the node's receiver must not clip; keep `RxGain` at 0 and let tcd's AGC set the level. An example with numbers is in [docs/AUDIO.md](AUDIO.md#fm-levels-from-svxlink-and-allstar).
- The FM noise under the speech turns robotic after the AMBE encoder on DMR; tcd removes most of it before encoding (`AGCAnalogNoiseSuppress`, see [tcd/README.md](../tcd/README.md)).
- The node shows in the links on the module of its selected TG, or while it has none (TG 0) on the one of its first mapped monitored TG, with the address urfd sees: a node in the reflector's own LAN shows its internal IP.

**Admin commands**:
```json
{"cmd": "svxs_user_add", "token": "...", "callsign": "N0CALL-HS", "password": "secret"}
{"cmd": "svxs_user_remove", "token": "...", "callsign": "N0CALL-HS"}
{"cmd": "svxs_user_list", "token": "..."}
{"cmd": "svxs_peer_list", "token": "..."}
```

## USRP (AllStar)

Bridges one AllStar node (app_rpt's USRP channel) to a module. Needs a transcoded module.

```ini
[USRP]
Enable = true
Callsign = ALLSTAR
IPAddress = 1.2.3.4            # AllStar node IP
RxPort = 34000
TxPort = 32000
Module = A                     # must be a transcoded module
# RxGain = 0                   # incoming audio gain in dB (-40 to +40)
# TxGain = 0                   # outgoing audio gain in dB (-40 to +40)
```

**RxGain / TxGain**: as for SVX, per connection; samples past full scale are clipped. tcd runs AllStar audio through its AGC for analog sources, so they normally stay 0.

**Talker**: a TEXT packet with a SET_INFO TLV before the voice, as DVSwitch Analog_Bridge sends it, names the talker (callsign and DMR ID); urfd sends the same packet ahead of every stream it plays to the node. Without one the stream runs under `Callsign`, with that callsign's DMR ID from the database; otherwise DMR hotspots, DMR+ and P25 get it only with `[MMDVM] FallbackDmrId`. The unkey ends the stream.

## Blocking protocols

`BlockProtocols` keeps audio from the listed protocols away from a connection, both ways. It works on MMDVM, MMDVMClient, OpenBridge, SvxReflector, SVXServer, DCSClient, DExtraClient, DPlusClient and YSFClient. A client connector never routes its own audio back out (e.g. MMDVMClient never sends BrandMeister's audio back to BrandMeister).

Protocol names (comma separated): `MMDVM`, `MMDVMClient` (every connection) or one connection's name (`MMDVMClient.TGIF`, `MMDVMClient1` to `MMDVMClient9`), `OpenBridge`, `SvxReflector`, `SVX` or `SVXServer`, `DExtra`, `DExtraClient`, `DPlus`, `DPlusClient`, `DCS`, `DCSClient`, `DMRPlus`, `YSF`, `YSFClient`, `M17`, `NXDN`, `P25`, `USRP`, `URF`, `XLXPeer`, `G3`, and the name of a loaded [plugin](#plugins). The admin page blocks and unblocks pairs at runtime (not kept over a restart; "Reset to Config Default" restores the INI settings).

## Reflector interlinking

urfd peers with URF, XLX, DCS and M17 (mrefd) reflectors listed in `urfd.interlink`. Only the reflectors listed there may peer; both sides must list each other. Host names are resolved at load time.

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

| Type | Prefix | Default port | Protocol | Requires |
|------|--------|-------------|----------|----------|
| URF peer | `URF` | 10017 | URF native | - |
| XLX peer | `XLX` | 10002 | XLX | `[XLXPeer] Enable = true` |
| DCS peer | `DCS` | 30051 | DCS native | - |
| DCS peer (XLX) | `DCS` + `XLX` | 10002 | XLX | `[XLXPeer] Enable = true` |
| M17 peer (mrefd) | `M17-` | 17000 | M17 interlink | `[M17] Callsign = M17-XXX` |
| M17 peer (legacy mrefd < 1.0) | `M17-…L` | 17000 | M17 interlink | `[M17] Callsign = M17-XXX` |

The protocol field is only for DCS entries. With OpenDHT, URF peers can also be found through the DHT (no address needed).

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

**URF interlink version**: urfd announces 4 and adds the M17 META type and CAN to the stream header for peers that announce 4 too, so GNSS stays GNSS across linked reflectors; peers announcing 3 (other urfd forks) get the legacy header. `InterlinkVersion = 3` announces 3 and never sends the extension.

**mrefd**: M17 entries need the same modules on both sides, and this reflector's `[M17] Callsign` listed in the mrefd's `mrefd.interlink`. An entry with only the modules (`M17-XXX AB`) takes address, port and legacy state from the mrefd's Ham-DHT document; urfd publishes its own there in mrefd's format, so an mrefd can list this reflector the same way. Pick a designator no other M17 reflector uses. Link, keepalive, voice and packet data follow mrefd: data to an mrefd >= 1.0 carries the module as an extra byte, a legacy one is addressed as `M17-XXX <module>` and gets no packet data. A stream from one mrefd is not passed on to another. Interlinked mrefds show up as M17 peers.

## Echo module

A parrot: what is said on the module is played back after 3 seconds of silence.

```ini
[Modules]
Modules = AFSZ

[Echo]
Enable = true
Module = Z
```

M17 clients reach it from any module by transmitting to `PARROT` or `ECHO`, see [M17](#m17).

## Plugins

A protocol can also come from a shared object that urfd loads at start, for one that is not part of this repository. `[Plugins]` names the files, one `Load` line each:

```ini
[Plugins]
Load = /usr/local/lib/urfd/myproto.so
```

A plugin is C++ built against this source tree (see [reflector/Plugin.h](../reflector/Plugin.h)): its protocol is a `CProtocol` like the built-in ones and uses urfd's classes and globals, which the `urfd` executable exports. It fits only the urfd it was built for: urfd compares the version, the build options (`DHT`, `debug`) and the size of its main classes and refuses a plugin built from other sources, so rebuild the plugins with every urfd update.

- **Settings**: a plugin named `Foo` reads the section `[Foo]` of `urfd.ini`, at every start and reload. `inicheck` only notes plugin sections.
- **Routing**: `Foo` is a protocol name in every `BlockProtocols`, `Protocols<M>` and the admin `block`/`unblock` commands; its clients pass the gatekeeper like the others.
- **Admin socket**: its own commands work by name or as `{"cmd": "plugin", "name": "Foo", "command": "...", ...}`; `status` lists the plugins with what they add, `tg_list` their modules.
- **Dashboard**: each plugin describes its sections in the `status` answer and the admin page draws them, see [docs/PLUGINS.md](PLUGINS.md).
- **Logins**: it may keep its logins in the [credential database](#credential-database) under a service name of its own.
- **Building**: put the plugin's source into `plugins/<name>/` (with a `Makefile` that takes `URFD=`), e.g. as a `git clone` of its repository; git ignores it there. The Docker image builds it with urfd into `/usr/local/lib/urfd/`; natively `make plugins` and `sudo make plugins-install` in `reflector/` do the same. `setup.sh update` pulls every plugin directory that is a git clone (also in `tcd/plugins/`) before it builds. See [plugins/README.md](../plugins/README.md).

## Admin interface

Runtime management from the dashboard's admin page, which logs in over the [WebSocket](#websocket), and through a JSON socket for scripts. It runs in its own thread; each socket connection gets its own thread with a 5 s idle timeout, so a stalled client blocks nothing.

```ini
[Admin]
Enable = true
Port = 10101
Password = yoursecretpassword
# BindingAddress = 127.0.0.1      # default: loopback only
```

The admin page (π in the dashboard's footer or `?show=admin`, shown with `[Admin] Enable = true` and `[Dashboard] Admin = true`; without the latter only the socket is open) logs in with the `[Admin] Password`. The login is a token for that browser tab, valid until an hour after its last command; a reload keeps it. A wrong password pauses logins from that address (2 s, doubling up to 5 min). The dashboard keeps no copy of the password.

**What it does:**
- **Talkgroups**: add and remove TGs at runtime with a timeout, also secondary (RX only) ones; with a BrandMeister API key they are registered as static on BrandMeister, without one a **kerchunk** activates them
- **Protocol blocking**: block and unblock protocol pairs at runtime (reset on restart)
- **D-Star and YSF links**: add and remove DCS, DExtra, DPlus and YSF reflector connections
- **MMDVM server and SVXServer**: users, TG mappings and connected nodes (DMR ID, frequencies, location, software)
- **Reconnect** MMDVMClient, SvxReflector and the client connectors
- **Transcoder**: connection, codec, packet counts and round-trip time per module
- **Log**: the last 50 lines, refreshed every 10 s (the socket gives up to 200); **clear last heard**

**Socket protocol**: line-delimited JSON over TCP. Authenticate first, then send commands with the token returned:
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

The commands of the D-Star and YSF connectors, the MMDVM server and SVXServer are in their sections above; the credential database has `credentials_status` and `credentials_flush`.

## Credential database

The logins of `[MMDVM]` and `[SVXServer]` (and of [plugins](#plugins) that use it) can also come from a database, so another program can manage them: SQLite, MySQL/MariaDB or PostgreSQL. Users in `urfd.ini` stay valid and win; only a user who is not there is looked up, and the answer stays in a cache (`CacheTTL`, default 300 s; unknown users `NegativeTTL`, 60 s). If the database is down, cached logins keep working. The admin page adds new users to the database. Passwords are stored as plain text, as the protocols need them for their challenge. Off by default:

```ini
[Credentials]
Backend = sqlite           # ini (no database), sqlite, mysql or postgresql
Path = /usr/local/etc/urfd/credentials.db
# Host = localhost         # mysql, postgresql: Host, Port, Database, User, Password
# Table = urfd_credentials
# CacheTTL = 300
```

Table, cache, building and the admin commands: [docs/CREDENTIALS.md](CREDENTIALS.md).

## WebSocket

Everything the dashboard shows comes from here: clients, peers and talking stations (with the operator's name from the ID database) as they change, the last heard list with the length and audio report of each transmission, the bridges of each module (TG mappings, autolinks, connectors) and their changes, and the whole state when a client connects; IP addresses only masked, as `[Dashboard]` allows. The admin page sends its commands over the same connection. It listens on loopback (`127.0.0.1:10102`) and the dashboard's nginx passes `/ws` through. Always built in (needs `libwebsockets-dev`); `setup.sh` sets `[WebSocket] Enable = true`; changes need a restart. urfd writes no XML or JSON status file. Messages, requests and setup: [docs/WEBSOCKET.md](WEBSOCKET.md).

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

The footer names `[Names] SysopEmail` (obfuscated against bots). The dashboard has no JSON API of its own (`/json/*` answers 410); its data is in the WebSocket's messages. Old links (`index.php?show=...`) lead to the same pages. The reflector list comes from the XLX directory through the dashboard's nginx, which keeps a copy for 5 minutes and serves an older one while the directory is down; the QuadNet feed goes through nginx as well. A tab left open reloads itself after an update.

## SIGHUP configuration reload

urfd rereads much of its configuration without dropping clients:

```bash
sudo ./setup.sh reload

# by hand, Docker: the signal must reach urfd, not supervisord (PID 1 restarts everything on SIGHUP)
docker exec urfd pkill -HUP -x urfd

# bare metal
sudo pkill -HUP -x urfd
```

**Reloaded on SIGHUP:**
- TG mappings (MMDVMClient, OpenBridge, SvxReflector, SVXServer); dynamic TGs are kept, the MMDVMClient connections reconnect
- `[MMDVM] DynamicTimeout`
- Whitelist, blacklist and interlink files
- Transcoded modules (`[Transcoder] Modules`, also to and from `auto`); a tcd connection for a module no longer listed is closed
- Module access by protocol and hidden talkers (`[Modules] Protocols<M>`, `HideUnlisted`)
- The plugins' sections, and logins changed in the credential database (its cache is flushed; `[Credentials]` itself needs a restart)
- Reflector metadata (sponsor, country, URLs)

**Needs a restart:**
- Callsign, database settings (Mode, URL, RefreshMin)
- `[MMDVM]` talkgroups and users (users can be changed at runtime on the admin page)
- Adding or removing modules
- Enabling or disabling protocols, ports
- IP address binding
- Admin interface settings
- Echo module
- Transcoder port, binding address and password
