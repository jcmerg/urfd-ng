# WebSocket notifier protocol

urfd pushes its state to WebSocket clients, so a dashboard sees links, peers and talking stations as they change instead of reading the XML file every few seconds. Based on the notifier by Jason Kendall (coolacid), PR #1.

## Setup

urfd is built with it by default (`WEBSOCKET = true` in `urfd.mk`, needs `libwebsockets-dev`). It is off until enabled in `urfd.ini`:

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
- `modules`: the configured module list, including description, echo flag, linked node count, on-air user, transcoded flag, and per-module protocol IDs
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
- `open_channels`: active streams currently transmitting on a module

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

Sent when a stream opens on a module: `callsign` is the talking station, `via_node` the client it came through, `on_module` the module.

Example payload:

```json
{
  "callsign": "N7TAE",
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

## Ping / pong behavior

libwebsockets pings a client that sent nothing for `PingInterval` seconds and hangs up when no pong came back within `IdleTimeout` seconds. Browsers answer pings on their own; this is the standard WebSocket control frame, not an application message.

## Notes

- The server does not accept messages from clients for this notifier path; it only pushes state updates to connected websocket clients.
- The server sends `initial_connect` first so the browser or UI can render the reflector identity, module list, and enabled protocols immediately. The subsequent `state_snapshot` then shows the live runtime state.
- The server intentionally omits client IP addresses from websocket payloads for privacy and UI simplicity.
