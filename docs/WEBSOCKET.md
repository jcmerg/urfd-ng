# WebSocket protocol

urfd pushes its state to WebSocket clients, so the dashboard sees links, peers and talking stations as they change; since urfd 4.38 / dashboard 3.0 it is the dashboard's only source (there is no XML file any more), and the admin page sends its commands the same way. Based on the notifier by Jason Kendall (coolacid), PR #1.

## Setup

urfd is always built with it (needs `libwebsockets-dev`). The dashboard needs it on, as the template and `setup.sh` have it:

```ini
[WebSocket]
Enable = true
Port = 10102
# BindingAddress = 127.0.0.1   # Default: loopback only
# MaxClients = 64              # connections at the same time
# QueueLength = 256            # unsent events per client before it is dropped
# PingInterval = 30            # seconds of silence before a ping
# IdleTimeout = 90             # seconds without an answer before hanging up
```

Changes need a restart. The events are public (IP addresses only masked, as `[Dashboard]` says, see below); the admin commands need a login with the `[Admin] Password`. It stays on loopback by default: let the web server pass it through, e.g. nginx:

```nginx
location /ws {
    proxy_pass http://127.0.0.1:10102;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_read_timeout 120s;
}
```

`X-Forwarded-For` tells urfd the browser's address, so a wrong admin password pauses only that browser's logins (see Requests); without it every browser comes from 127.0.0.1 and shares one pause. urfd reads the header only from a peer on loopback, and only its last entry.

The Docker image has this block in `docker/nginx-dashboard.conf`, next to the static dashboard (`dashboard/`); there the browser's address comes from the `X-Real-IP` of a TLS proxy in front. A TLS proxy that passes `/ws` straight to port 10102 instead sets `X-Forwarded-For` itself, as above.

The browser connects with `new WebSocket("wss://" + location.host + "/ws")`; a subprotocol is optional (`urfd`).

## Connection lifecycle

- When a client connects, the server sends an `initial_connect` event first. This contains static reflector metadata and startup timing information so the browser can render the reflector description before the live state arrives.
- The server then sends a `state_snapshot` event describing the current dynamic reflector state. A client can ask for it again, see Requests.
- After that every change arrives as an event. A client that falls behind by more than `QueueLength` events is disconnected: it reconnects and gets a fresh snapshot instead of a wrong state.
- The connection stays alive with WebSocket pings, see below.

## Message format

Every message from the server is a JSON object with the following shape:

```json
{
  "event_type": "initial_connect",
  "timestamp": 1730000000,
  "payload": {
    "server_started": 1730000000,
    "current_time": 1730000000,
    "uptime_seconds": 0,
    "reflector": {
      "callsign": "URF123",
      "country": "US",
      "dashboard_url": "https://example.invalid"
    },
    "modules": [
      {
        "name": "A",
        "description": "Main reflector module",
        "echo": true,
        "linked_nodes": 1,
        "on_air": "N0CALL",
        "transcoded": false,
        "YSFDGID": 10
      }
    ],
    "protocols": [
      { "name": "YSF", "port": 42000 },
      { "name": "DCS", "port": 50001 }
    ]
  }
}
```

Notes:

- `event_type` identifies the event category.
- `timestamp` is the Unix time when the server emitted the event.
- `payload` is always a JSON object, even when the event is just a minimal status update.

## Server-to-client events

The notifier emits regular event objects for the reflector lifecycle and state changes.

### `initial_connect`

Sent as the first message after a client connects.

This contains static descriptor data for the reflector and the startup timing metadata:

- `version`: urfd's version, e.g. `4.38.0`
- `reflector`: callsign, and country, sponsor, dashboard URL and `email` (`[Names] SysopEmail`) when configured
- `dashboard`: what the dashboard shows, from `[Dashboard]`: `motd`, `last_heard_rows`, `links_rows`, `peers_rows` (rows), `filter`, `links_ip` / `peers_ip` (whether IP addresses are shown at all) and `admin` (`[Admin] Enable` and `[Dashboard] Admin`: the admin page can log in)
- `modules`: the configured module list, including description, echo flag, linked node count, on-air user (`on_air`, with `on_air_name`), transcoded flag, per-module protocol IDs, and `mappings`: the module's bridges, see `module_mappings`
- `protocols`: the enabled protocol list and their configured ports; a plugin's with its `description`
- `server_started`: Unix timestamp when the reflector process started
- `current_time`: Unix timestamp at the moment the initial packet was built
- `uptime_seconds`: elapsed time since startup

This packet is intentionally static metadata and startup information; it is not meant to be the live runtime snapshot.

### `state_snapshot`

Sent immediately after the `initial_connect` message.

This includes the current dynamic state of the reflector:

- `server_started`: Unix timestamp when the reflector process started
- `current_time`: Unix timestamp at the moment the snapshot was built
- `uptime_seconds`: elapsed time since start
- `transcoded`: the modules transcoded right now, e.g. `"AD"` (with `[Transcoder] Modules = auto` they follow tcd)
- `links`: the nodes linked to the reflector, as `client_added` has them: not the pseudo clients of the connectors and bridges (MMDVMClient, SvxReflector, DCS/DExtra/DPlus/YSF clients, USRP; those are in the module's `mappings`) and not the clients of interlinked peers (those are in `peers`); `client_added` and `client_removed` cover the same nodes
- `last_heard`: recent heard stations, newest first, as `user_heard` has them
- `open_channels`: active streams currently transmitting on a module, with the operator's `name`
- `peers`: linked peer reflectors with their clients, like `peer_added`

An event that happens while the snapshot is built can arrive after it although the snapshot already has it, e.g. a `client_added` for a client already in `links`. Nothing is lost that way; apply events by key (callsign, protocol, module) so a repeat changes nothing.

### `client_added`

Sent when a reflector client is added.

Example payload:

```json
{
  "callsign": "N0CALL  B",
  "protocol": "DCS",
  "reflector_module": "B",
  "modules": "B",
  "connect_time": 1730000000,
  "last_heard_time": 1730000000,
  "ip": "*.*.56.123"
}
```

- `callsign`: 8 characters and the module or suffix, as urfd keeps it (`N0CALL  B` is N0CALL-B)
- `modules`: every module the node is linked to; a DMR hotspot has `ts1` / `ts2` as well, the module of each timeslot
- `ip`: only with `[Dashboard] LinksIP` other than `HideIP` (the default), masked as it says: `ShowLast1ByteOfIP` `*.*.*.123`, `ShowLast2ByteOfIP` `*.*.56.123`, `ShowLast3ByteOfIP` `*.34.56.123`, `ShowFullIP`; for IPv6 the last 1, 2 or 3 groups; the character is `IPMask`

### `client_removed`

Sent when a reflector client is removed, with the payload of `client_added` (callsign and protocol identify it).

### `peer_added`

Sent when a peer node is added.

Example payload:

```json
{
  "callsign": "URF123  ",
  "protocol": "URF",
  "modules": "ABC",
  "connect_time": 1730000000,
  "last_heard_time": 1730000000,
  "ip": "*.*.56.123",
  "clients": [
    { "callsign": "URF123 A", "protocol": "URF", "reflector_module": "A" }
  ]
}
```

`ip` as for `client_added`, from `[Dashboard] PeersIP`.

### `peer_removed`

Sent when a peer node is removed, with the payload of `peer_added` (its clients already gone).

### `station_started`

Sent when a stream opens on a module: `callsign` is the talking station, `suffix` the one a D-Star radio sends, `name` the operator's name from the DMR/NXDN ID database, `via_node` the client it came through, `via_peer` the reflector it came from (this one when local), `on_module` the module, `last_heard_time` the start. `suffix`, `name` and the position (`locator`, `latitude`, `longitude`: the middle of the locator's field) only when known.

Example payload:

```json
{
  "callsign": "N0CALL  ",
  "suffix": "ID51",
  "name": "Tom",
  "via_node": "N0CALL  B",
  "via_peer": "URF123  ",
  "on_module": "A",
  "protocol": "DExtra",
  "last_heard_time": 1730000000,
  "locator": "JO40fd",
  "latitude": 50.14583,
  "longitude": 8.375
}
```

### `station_stopped`

Sent when that stream closes, with the payload of its `station_started`. There is one stream per module, so at most one station per module is active.

### `user_heard`

A station at the head of the last heard list, with the fields of `station_started`; also when the position of a station already in the list arrives. The list keeps one entry per station, node, module and peer: drop an older entry with the same `callsign`, `suffix`, `via_node`, `on_module` and `via_peer`, and keep the list sorted by `last_heard_time`.

### `user_removed`

An entry left the last heard list: the talker was renamed during the stream (the new callsign follows as `user_heard`). Fields and key as `user_heard`.

### `last_heard_cleared`

The admin page cleared the last heard list; empty payload.

### `module_mappings`

The bridges of every module, sent when one changed: a TG mapping added, removed or its timer refreshed, a connector to another reflector up or down. urfd checks every 10 seconds, so a change arrives within that. The same list is in `initial_connect` as `mappings` of each module.

```json
{
  "modules": {
    "F": [
      { "protocol": "MMDVMClient", "type": "TG", "id": 26363, "timeslot": 2 },
      { "protocol": "MMDVMClient", "type": "TG", "id": 2625, "timeslot": 2, "expires": 1730000900 },
      { "protocol": "SVX", "type": "TG", "id": 26363 },
      { "protocol": "DCSClient", "type": "Module", "id": "C", "remote_name": "dcs001.xreflector.net:30051", "connected": true }
    ],
    "S": []
  }
}
```

- `protocol`: `YSF`, `NXDN`, `P25` (`type` `AutoLink`), `USRP` (`Bridge`), `MMDVM`, `MMDVMClient`, `SvxReflector`, `SVX` (`TG`), `DCSClient`, `DExtraClient`, `DPlusClient` (`Module`), `YSFClient` (`DG-ID`); only enabled protocols
- `id`: the TG, the reflector ID, the remote module letter or the DG-ID, when there is one
- `timeslot`: DMR timeslot, for `MMDVM` and `MMDVMClient`
- `expires`: Unix time a dynamic TG ends unless it is used again; static ones have none
- `remote_name`: the remote reflector or server, for connectors `host:port`
- `connected`: for connectors, whether the link is up

## Ping / pong behavior

libwebsockets pings a client that sent nothing for `PingInterval` seconds and hangs up when no pong came back within `IdleTimeout` seconds. Browsers answer pings on their own; this is the standard WebSocket control frame, not an application message.

## Requests

A client sends a JSON object with `cmd` and any `id` of its own; the answer is an event `reply` with that `id` and the result as `payload`, only to this client:

```json
{ "id": 7, "cmd": "auth", "password": "..." }
{ "event_type": "reply", "id": 7, "timestamp": 1730000000, "payload": { "status": "ok", "token": "3f2a..." } }
```

- `snapshot`: a new `state_snapshot` (the last heard times of links and peers, a node changing its module and transcoding change without an event; the dashboard asks every 10 s). At most one every 2 seconds per connection, else a `reply` with an error.
- `auth`: logs this connection in to the admin commands, with `password` (the `[Admin] Password`) or a `token` from an earlier login, e.g. after a reload of the page. A wrong password pauses logins from that browser's address for 2 seconds, doubling with every further one up to 5 minutes (the address comes from `X-Forwarded-For` when urfd's peer is on loopback, i.e. nginx; a TLS proxy in front passes `X-Real-IP` to it, or all browsers share one pause); a token is valid until an hour after its last command. Answers `admin page disabled` without `[Admin] Enable = true` and `[Dashboard] Admin = true`.
- `logout`: ends the login and makes its token invalid.
- every command of the admin socket, with the same fields and answers (`status`, `tg_list`, `tg_add`, `log`, `mmdvm_user_add`, `plugin`, ...; see `reflector/AdminSocket.cpp`), without a `token`: the connection's login counts. Without one the answer is `authentication required`, with `"code": "auth"`. The commands run one after another in a thread of their own, so a slow one never holds up the events; at most 64 wait, 8 of them from one connection, else the answer is `busy, try again`.

A request is at most 64 KB; a longer one closes the connection. The TCP admin socket (`[Admin] Port`) stays for scripts and takes the same commands with a `token` from its own `auth`.

## Notes

- The server sends `initial_connect` first so the browser or UI can render the reflector identity, module list, and enabled protocols immediately. The subsequent `state_snapshot` then shows the live runtime state.
- IP addresses go out only masked, as `[Dashboard]` allows; the full address never leaves urfd.
