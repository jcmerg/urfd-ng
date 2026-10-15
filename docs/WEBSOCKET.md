# WebSocket notifier protocol

urfd pushes its state to WebSocket clients, so a dashboard sees links, peers and talking stations as they change instead of reading the XML file every few seconds. Based on the notifier by Jason Kendall (coolacid), PR #1.

## Setup

urfd is always built with it (needs `libwebsockets-dev`). It is off until enabled in `urfd.ini`:

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

Changes need a restart. The listener has no password and only sends public data (no IP addresses), but it stays on loopback by default: let the web server pass it through, e.g. nginx:

```nginx
location /ws {
    proxy_pass http://127.0.0.1:10102;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_read_timeout 120s;
}
```

The Docker image has this block in `docker/nginx-dashboard.conf` and also serves coolacid's live dashboard from `dashboard-ws/static/` as `/live/`, next to the PHP dashboard; its `config.js` points it at `/ws` on the same host and loads Vue from the folder.

The browser connects with `new WebSocket("wss://" + location.host + "/ws")`; a subprotocol is optional (`urfd`).

## Connection lifecycle

- When a client connects, the server sends an `initial_connect` event first. This contains static reflector metadata and startup timing information so the browser can render the reflector description before the live state arrives.
- The server then sends a `state_snapshot` event describing the current dynamic reflector state.
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
        "on_air": "N7TAE",
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

- `reflector`: callsign, and country, sponsor and dashboard URL when configured
- `modules`: the configured module list, including description, echo flag, linked node count, on-air user (`on_air`, with `on_air_name`), transcoded flag, per-module protocol IDs, and `mappings`: the module's bridges, see `module_mappings`
- `protocols`: the enabled protocol list and their configured ports
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
- `links`: all currently linked clients/modules
- `last_heard`: recent heard stations
- `open_channels`: active streams currently transmitting on a module, with the operator's `name`
- `peers`: linked peer reflectors with their clients, like `peer_added`

An event that happens while the snapshot is built can arrive after it although the snapshot already has it, e.g. a `client_added` for a client already in `links`. Nothing is lost that way; apply events by key (callsign, protocol, module) so a repeat changes nothing.

### `client_added`

Sent when a reflector client is added.

Example payload:

```json
{
  "callsign": "DSTAR1",
  "protocol": "dcs",
  "reflector_module": "A",
  "connect_time": 1730000000
}
```

The websocket transport intentionally omits IP addresses from this payload.

### `client_removed`

Sent when a reflector client is removed.

Example payload:

```json
{
  "callsign": "DSTAR1",
  "protocol": "dcs",
  "reflector_module": "A",
  "last_heard_time": 1730000000
}
```

### `peer_added`

Sent when a peer node is added.

Example payload:

```json
{
  "callsign": "URFPEER",
  "protocol": "mmdvm",
  "clients": [
    {
      "callsign": "M1TEST",
      "protocol": "mmdvm",
      "reflector_module": "B"
    }
  ]
}
```

### `peer_removed`

Sent when a peer node is removed.

Example payload:

```json
{
  "callsign": "URFPEER",
  "protocol": "mmdvm"
}
```

### `station_started`

Sent when a stream opens on a module: `callsign` is the talking station, `name` the operator's name from the DMR/NXDN ID database (empty when unknown), `via_node` the client it came through, `on_module` the module.

Example payload:

```json
{
  "callsign": "N7TAE",
  "name": "Tom",
  "via_node": "URF123",
  "via_peer": "URF123",
  "on_module": "A",
  "protocol": "ysf",
  "last_heard_time": 1730000000,
  "locator": "FN42xx"
}
```

### `station_stopped`

Sent when that stream closes, with the payload of its `station_started`. There is one stream per module, so at most one station per module is active.

### `module_mappings`

The bridges of every module, sent when one changed: a TG mapping added, removed or its timer refreshed, a connector to another reflector up or down. urfd checks with every write of the XML file, so a change arrives within a few seconds (needs `[Files] XmlPath` or `JsonPath`, as every setup has). The same list is in `initial_connect` as `mappings` of each module.

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

## Notes

- The server does not accept messages from clients for this notifier path; it only pushes state updates to connected websocket clients.
- The server sends `initial_connect` first so the browser or UI can render the reflector identity, module list, and enabled protocols immediately. The subsequent `state_snapshot` then shows the live runtime state.
- The server intentionally omits client IP addresses from websocket payloads for privacy and UI simplicity.
