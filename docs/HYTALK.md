# HyTalk protocol

urfd is a server for Hytera radios running the HyTalk PoC app (tested with HyTalk 3.2 on the P50 Pro). The radio logs in to urfd directly; no Hytera server is involved. This page describes the protocol as urfd speaks it, for anyone who wants to debug a radio, write a tool or build another server. It was worked out from the radio's traffic and logs for interoperability; it is not Hytera's specification, and it covers only what urfd uses.

Setup and the `urfd.ini` keys are in the README, section HyTalk. Examples use `1234567` as the account, `N0CALL` as the callsign and `203.0.113.10` as the server address.

## Overview

| Port (default) | Transport | Purpose | `urfd.ini` |
|---|---|---|---|
| 5400 | TCP, HTTPS | user profile: where the radio logs in | `Port` |
| 5400 | UDP, KCP | signalling (PNAS) | `Port` |
| 5402 | UDP | voice, RTP with AMBE+2 | `RtpPort` |
| 5403 | UDP | RTCP and NAT keepalives | `RtpPort` + 1 |
| 5404 | TCP, HTTP and HTTPS | group list | `HttpPort` |
| 7406 | TCP | GPS positions (GTRP) | `GisPort` |

A talk group is an urfd module. The order of a radio's start:

1. HTTPS `POST` of the user profile on TCP `Port`: the answer names the PNAS server.
2. KCP session on UDP `Port`, then the PNAS login with an HTTP-digest-like challenge.
3. The group list over HTTP or HTTPS on `HttpPort`, when urfd tells the radio its groups changed.
4. Group affiliation, group calls with floor control, voice over RTP.
5. A TCP connection to the GIS server for GPS positions.

All PNAS and GTRP numbers are big-endian; KCP's own header is little-endian, as upstream KCP.

## User profile (HTTPS)

The radio asks with `POST /MPTTVS/mptt-user-profile` (body JSON with its account, or its serial number as `application/sn+json`). urfd answers with a certificate it makes at start-up and:

```json
{"serverIP":{"tcf_server_1":"203.0.113.10:5400","dmshttpport":5400,"dmshttpsport":5400,
 "RedundMasterHttpsServer":"","RedundSlaveHttpsServer":"","localrealm":"","peerrealm":"",
 "app_version_server":"","Wssa_server":"","ppoc_registers_switch":1},"versionOptionFeatures":[]}
```

- `tcf_server_1` is the PNAS server, `ip:port` on UDP.
- `ppoc_registers_switch` 1 makes a radio that asked by serial number log in with its account.
- `RedundMasterHttpsServer` must stay empty, else the radio replaces the server it is programmed with.

## Transport: KCP

PNAS runs over KCP ([skywind3000/kcp](https://github.com/skywind3000/kcp)) on UDP, with three changes:

- Every segment starts with an 8 byte app id the client picks; the server echoes it. The segment header is 36 bytes instead of 24.
- Between `una` and `len` are two 16 bit fields, which the client writes as 2 and 1.
- Command `0x55` is a reset: the session is gone and the client starts a new one.

Segment header (little-endian):

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | app id |
| 8 | 4 | conv |
| 12 | 1 | cmd (`0x51` push, `0x52` ack, `0x53` window probe, `0x54` window size, `0x55` reset) |
| 13 | 1 | frg |
| 14 | 2 | wnd |
| 16 | 4 | ts |
| 20 | 4 | sn |
| 24 | 4 | una |
| 28 | 2 | 2 |
| 30 | 2 | 1 |
| 32 | 4 | len |

A session starts with a push segment with `sn` 0. urfd answers any other segment of an unknown session (for example after a restart of urfd) with a reset, at most once a second per session, so the radio logs in again instead of waiting forever. urfd runs KCP with `nodelay 1, interval 10 ms, resend 2, no congestion control`, window 32, MTU 1400.

## PNAS messages

The KCP stream carries units of one options byte and a message. Options 0 is a plain message; 1 means a 4 byte option word follows (`00 00 <type|0x80> <key id>`) and the message is encrypted with one of the app's built-in keys (AES, DES, 3DES or RC4, see [Key file](#key-file)). urfd decrypts and answers the way the radio's last unit came.

The HyTalk 3.2 app always encrypts: it sets type 3 (AES-256-CBC) when its protocol stack starts, before it talks to any server, and the radio has no setting for it. Its units start with `01 00 00 83 03`. A server cannot talk to it without the built-in keys.

Message header, 15 bytes:

```
50 <seq u32> <transaction id u32> <type u8> 00 C0 <total length u24>
```

`seq` is the sender's running message counter, the transaction id ties a request to its response, the length counts the whole message. Information elements (IEs) follow, each `<IEI u16> <length u16> <value>`. A receiver skips IEs it does not know. Some IEs nest IEs in their value (audio description, group lists).

### Message types

| Type | Name | Direction |
|---|---|---|
| `0x01` | REGISTER_REQUEST | radio → server |
| `0x02` | REGISTER_ACCEPT | server → radio |
| `0x03` | REGISTER_REJECT | server → radio |
| `0x04` | REGISTER_COMPLETE | radio → server |
| `0x05` / `0x06` | DEREGISTER_REQUEST / _ACCEPT | radio → server / back |
| `0x07` | CALL_REQUEST | both |
| `0x08` | CALL_ACCEPT | both |
| `0x09` | CALL_COMPLETE | radio → server |
| `0x0c` | CALL_REJECT | both |
| `0x12` / `0x13` | CALL_RELEASE_REQUEST / _RESPONSE | both |
| `0x14` | FLOOR_REQUEST | radio → server |
| `0x15` / `0x16` | FLOOR_GRANT / _GRANT_ACK | server → radio / back |
| `0x17` | FLOOR_REJECT | server → radio |
| `0x19` | FLOOR_INFORM | server → radio |
| `0x1a` / `0x1b` | FLOOR_RELEASE / _RELEASE_ACK | radio → server / back |
| `0x42` / `0x43` | MESSAGE_REQUEST / _RESPONSE | both |
| `0x46` / `0x47` | HEARTBEAT_REQUEST / _RESPONSE | server → radio / back (urfd does not send it) |
| `0x48` / `0x49` | GROUP_INFO_NOTIFY_REQ / _RSP | server → radio / back |
| `0x4a` / `0x4b` | GROUP_INFO_UPDATE_REQ / _RSP | radio → server / back |
| `0x4c` / `0x4d` | GROUP_AFFI_NOTIFY_REQ / _RSP | radio → server / back |
| `0x52` | UNAUTHENTICATED | server → radio |
| `0x73` | USER_CONFIG_NOTIFY_RSP | radio → server |
| `0x78` / `0x79` | UE_CAPABILITY_NOTIFY_REQ / _RSP | radio → server / back |
| `0x7a` / `0x7b` | GROUP_UPDATE_REPORT_REQ / _RSP | radio → server / back |
| `0x8e` / `0x8f` | HEARTBEAT_PING / _PONG | radio → server / back |

The app knows more (private calls `0x0a`–`0x0f`, regrouping, remote kill and stun, end-to-end keys, video, voice and picture messages); urfd does not implement them.

### Information elements

| IEI | Name | Value |
|---|---|---|
| `0x0003` | audio description | nested: IP address, port, codec list, AMBE parameters, codec priority |
| `0x0004` | call attribute | 2 bytes; urfd sends 0 (no emergency, no encryption) |
| `0x0005` | call id | u32 |
| `0x0006` | call type | u8, 3 = group call |
| `0x0007` | called number | string |
| `0x0008` | caller number | string |
| `0x0009` | cause | u8, 0 = success |
| `0x000c` | floor inform type | u8, 0 = taken, 1 = idle |
| `0x000d` | floor status | u8, 0 = granted |
| `0x0010` | group number | string, `<group>@<realm>` |
| `0x001d` | IP address | u8 type 1, then 4 bytes IPv4 |
| `0x001e` | message container | text: u16 length, the text, 2 trailing bytes |
| `0x001f` | message type | 244 text, 245 long text, 240–243 picture, voice, video, document |
| `0x0021` | port | u16 |
| `0x0028` | codec list | bit mask, `0x40` AMBE, `0x04` AMR-WB |
| `0x002d` | user name | string, the callsign shown on the radio |
| `0x0036` | message reference | echoed in the message response |
| `0x0047` | audio RTP resource | 8 bytes: `00 <RTP port u16> 01 <IPv4>` |
| `0x0049` | keepalive time | u16 seconds |
| `0x004c` | subscriber number | string, `<account>@<realm>` |
| `0x0063` / `0x0064` | HTTP / HTTPS port | u16, of the group list |
| `0x0065` | presence server | IPv4 |
| `0x0074` | realm | string |
| `0x0076` / `0x0077` | master IP / port | IPv4 / u16, the PNAS server |
| `0x0081` | AMBE parameters | 2 bytes: `04 <payload type>` |
| `0x00c2` | codec priority | u8 |
| `0x00c8` | authentication challenge | 32 hex characters |
| `0x00c9` | authentication response | 32 hex characters (`0x00a5` in older apps) |
| `0x00cb` | group update id | u32, the group list version |
| `0x00cc` | GIS server | 8 bytes: `00 <port u16> 01 <IPv4>` |
| `0x00d7` | group affiliation list | nested group number IEs |
| `0x010a` | heartbeat interval | in the ping: the radio's interval in seconds, shifted left by 1 |
| `0x0149` | group list display | u8 |

## Login

```
radio  REGISTER_REQUEST     subscriber number 1234567@<enterprise id>
server UNAUTHENTICATED      challenge (32 hex characters)
radio  REGISTER_REQUEST     subscriber number, authentication response
server REGISTER_ACCEPT      or REGISTER_REJECT with a cause
radio  REGISTER_COMPLETE
server GROUP_INFO_NOTIFY_REQ  group update id, group list display 1
```

The response is a digest over the challenge, as lowercase hex:

```
MD5( MD5(user ":" realm ":" password) ":" challenge ":" MD5("REGISTER:123456789") )
```

`user` is the account without the realm. The realm the radio hashes with is not certain, so urfd accepts the configured `Realm`, `hytera.com` and the part after the `@` of the subscriber number. A 12 digit number is a 5 digit enterprise id followed by the 7 digit account.

REGISTER_REJECT causes: 1 unknown account, 2 wrong password.

REGISTER_ACCEPT carries: user name (the callsign), subscriber number, realm, master IP and port, audio RTP resource (urfd's address and `RtpPort`), keepalive time (30 s), presence server, HTTP and HTTPS port of the group list, and the GIS server. The HTTPS port must be there: without it the radio fetches its groups from port 0 and its app crashes.

Before REGISTER_COMPLETE a session may only ping, log in and log out; urfd ignores everything else.

### Keeping the session

- The radio pings (HEARTBEAT_PING) and pings again as soon as the pong arrives. urfd holds the pong for a little less than the interval the radio sends in the ping (at most 20 s), so the radio pings every ~20 s instead of every round trip. Without a pong for 3 intervals it drops the session.
- urfd drops a radio after 120 s without a datagram, and a session that has not logged in after 30 s.
- A radio that logs in again from somewhere else replaces its old session.

## Groups

After the login urfd sends GROUP_INFO_NOTIFY_REQ with its group list version. The radio then fetches the list over HTTP or HTTPS on `HttpPort`:

- `GET …/groupversionlist/…`:
  ```json
  {"groupversionlist":{"version":"3","grouplist":"052636300003052625000003"}}
  ```
  The list is, per group, two digits of length, the group number, one attribute digit and four digits of version.
- `GET …/groupinfolist/…<account>@<realm>…`:
  ```json
  {"grouplist":{"realmlist":[{"realm":"<realm>","group":[
   {"GDN":"26363","GName":"Fernmeldezug","GPrivilege":"","GrpProper":"0000","Creator":""}]}]}}
  ```
  The realm is the one after the `@` in the path, the radio's enterprise id. The four `GrpProper` digits are type, attribute, encryption and dynamic; urfd sends `0000`. Attribute 1 would make a group listen only, 2 reachable only while selected.

The version follows the groups in `urfd.ini`, so a changed list reaches the radios at their next login.

A radio names a group as `<group>@<its realm>` and rejects a call to a group it does not have with cause 28 ("group not exist"); urfd always sends group numbers in the realm of the radio's own subscriber number.

GROUP_AFFI_NOTIFY_REQ lists the groups the radio selected (the app selects one at a time); the response lists those the server knows. urfd puts the radio on the module of the first one.

## Group calls

A group has one call id while a call runs. urfd ends the call `HangTime` seconds (default 20) after the last talker.

**The radio presses PTT:**

```
radio  CALL_REQUEST    call id (its own), call type 3, group number, audio description
server CALL_ACCEPT     call id (the group's), call type 3, floor status 0 if free, audio description
radio  CALL_COMPLETE
       ... RTP from the radio ...
radio  FLOOR_RELEASE           (PTT up)
server FLOOR_RELEASE_ACK
server FLOOR_INFORM    idle, to every radio in the call
```

Without floor status in the CALL_ACCEPT the floor is busy and the radio listens. In a running call PTT is FLOOR_REQUEST, answered with FLOOR_GRANT or FLOOR_REJECT (cause 1).

**Someone else talks (another radio or another mode):** urfd sends every affiliated radio that is not in the call a server CALL_REQUEST (its call id, call type 3, call attribute 0, the group number, caller number, audio description). The radio accepts on its own with CALL_ACCEPT and its audio description. FLOOR_INFORM tells who talks: type 0 with subscriber number and user name. For a transmission from another mode the name is the callsign with its source, as in the D-Star text, e.g. `N0CALL via DMR TG91`. A radio takes one call at a time and rejects a second server call with cause 39 (busy).

**Release:** after the hang time urfd sends CALL_RELEASE_REQUEST (cause 0) to each radio in the call. The radio finds a call by transaction id and role, else by call id: for a call the radio set up, only its own call id from its CALL_REQUEST matches, so urfd releases with that. The radio answers with CALL_RELEASE_RESPONSE and plays its call-end tone, as on any end it did not make itself (a setting on the radio).

### Audio description

Nested IEs: IP address (`0x001d`), port (`0x0021`), codec list `0x40` (AMBE), AMBE parameters `04 57` (payload type 87), codec priority 7. The radio offers AMBE+2 and AMR-WB; urfd takes AMBE only.

## Voice: RTP

- RTP version 2, payload type 87, three AMBE+2 frames of 9 bytes (20 ms each) per packet, so 60 ms per packet; the timestamp advances by 160 per frame. urfd does not set the marker bit; the radio does not need it.
- The frames are the 72 bit DMR AMBE+2 (3600x2450) frames with their FEC, the same as in a DMR burst, so voice passes to and from DMR, YSF and NXDN without transcoding.
- A radio behind a carrier NAT signals a private address. It keeps its NAT open with 16 byte RTP packets of payload type 127 to `RtpPort` and `RtpPort` + 1 and wants each echoed back; without the echo it repeats them, drops the call and logs in again after 120 s. urfd echoes them and sends voice to the public address and port they come from. RTCP reports on `RtpPort` + 1 are not used.
- A talker's transmission ends 1.5 s after its last RTP packet, an urfd transmission to the radios 1 s after its last frame.

## Text messages

MESSAGE_REQUEST with message type 244 or 245, the text in the message container, and a group number or a called number. The server answers MESSAGE_RESPONSE with cause 0 and the message reference, then forwards the request to the other logged-in radios of the group (group number in each radio's realm) or to the radio with that number. A text to the group of the echo module comes back to its sender. Picture, voice, video and document messages (types 240–243) are answered with cause 1; they need a file server urfd does not have.

## GPS: GTRP

GTRP is a separate TCP protocol of TLV messages, not PNAS.

**Port.** REGISTER_ACCEPT names the GIS server, and the radio connects to the port it is told **plus 2000**. urfd therefore announces `GisPort` − 2000: with the default 7406 the radio is told 5406 and connects to 7406. Open `GisPort` on the router; 5406 only travels inside the login. `GisPort` must be at least 3024.

**Message header**, 8 bytes:

```
10 <type u8> <transaction id u32> <body length u16>
```

A response echoes the request's transaction id. The body has elements of a tag byte, a length byte and the value; tag `0x23` (result) is one byte with no length.

| Type | Message |
|---|---|
| `0x10` | keep-alive, request and response |
| `0x14` / `0x15` | register request / response |
| `0x16` / `0x17` | deregister request / response |
| `0x18` / `0x19` | location request / response |
| `0x1e` | GPS configuration request (server → radio) |
| `0x20` | configuration response |
| `0x21` | location report |

| Tag | Element |
|---|---|
| `0x23` | result, 0 = success |
| `0xa3` | IPv4 address |
| `0xa6` | own number, BCD |
| `0xa7` | peer number, BCD |
| `0xa8` | configuration flag |
| `0xa9` | GPS position |
| `0xaa` | GPS extension: heading and speed (2 BCD bytes each, tenths), battery percent, RSSI (sign and magnitude) |
| `0xab` | GIS configuration: `<interval u16> 00 <on> 00 00` |

Numbers are BCD with the first digit in the low nibble and `F` as padding. The GPS position is: fix byte (0 = valid fix), UNIX time u32, `E` or `W`, longitude as 5 BCD bytes `dddmm.mmmm`, `N` or `S`, latitude as 4 BCD bytes `ddmm.mmmm`, altitude as 3 BCD bytes in tenths of a metre; the decimal points are implicit.

**Flow.** The radio sends a register request with its own number; urfd answers with result 0 and then a GPS configuration with the report interval (`GisInterval`, default 60 s), after which the radio sends location reports. It sends a keep-alive every 10 s; urfd closes a connection after 60 s without a message.

GTRP has no password. urfd takes a position only from the address of the radio logged in over PNAS with that number. Right after an urfd restart the GPS connection comes back before the PNAS login; its positions count once the radio is logged in again.

## Key file

The app encrypts with a zero IV and PKCS#7 padding (none for RC4), with one of five ciphers chosen by the option word's type: 1 AES-128-CBC, 2 AES-192-CBC, 3 AES-256-CBC, 4 DES-CBC, 5 3DES-CBC, 6 RC4. The keys are fixed in the app, the same in every copy: an 80 byte table in its protocol library, the DES/RC4 key (8 bytes; 3DES reads 24 bytes from here), then the AES-128 key (key id 1, 16 bytes), the AES-192 key (key id 2, 24 bytes) and the AES-256 key (key id 3, 32 bytes). Without them a server cannot read a single message, since the app has no plain fallback.

urfd-ng does not ship the keys. `[HyTalk] KeyFile` (default `hytalk.keys` next to `urfd.ini`) holds them as 80 hex bytes, separated by spaces or commas, `#` starts a comment. Where to find them:

| | |
|---|---|
| App | HyTalk V3.2.00.040 for the P50 Pro, package `com.hytalkpro.ocean` |
| Library | `lib/armeabi-v7a/libpoc_protocol.so` in the APK |
| Table | virtual address `0xff1ab` of the library, 80 bytes |

`tools/hytalk-keys.py` reads them from your own copy and writes the file:

```bash
adb shell pm path com.hytalkpro.ocean      # where the radio keeps the app
adb pull <that path> hytalk.apk
python3 tools/hytalk-keys.py hytalk.apk /usr/local/etc/urfd/hytalk.keys
```

It checks the table against a SHA-256 digest and writes nothing if it does not match (another app version may keep it elsewhere). The keys are needed only so that urfd can talk to the radios at all (interoperability); keep the file to your installation.

## Not supported

Private (individual) calls, voice, picture and video messages, emergency calls, dynamic regrouping, monitoring several groups at once (the app affiliates one group; secondary DMR TGs on the module are the alternative), end-to-end encryption (not allowed in amateur radio), remote kill and stun.
