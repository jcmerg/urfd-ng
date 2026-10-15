# WebSocket notifier protocol

This project exposes a lightweight WebSocket notifier for clients that want near-real-time reflector state without polling a JSON file.

## Connection lifecycle

- The server opens a libwebsockets context when the notifier backend is enabled.
- Each client connection is accepted on the configured bind address and port.
- When a client connects, the server sends an `initial_connect` event first. This contains static reflector metadata and startup timing information so the browser can render the reflector description before the live state arrives.
- The server then sends a `state_snapshot` event describing the current dynamic reflector state.
- The connection stays alive with periodic keepalive traffic. If a client is idle too long, the server closes it.

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

- `reflector`: configured identity details from the XML output, such as callsign, country, sponsor, and dashboard URL when present
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

Sent when a station begins transmitting or becomes active.

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

Sent when a station becomes inactive.

The payload matches the same shape as `station_started`, but describes the active station being cleared.

### `uptime_update`

Sent from the main process at startup and whenever the process emits an uptime event.

Example payload:

```json
{
  "server_started": 1730000000,
  "current_time": 1730000000,
  "uptime_seconds": 0
}
```

## Ping / pong behavior

The server uses libwebsockets keepalive timers to monitor client responsiveness.

- Each client connection gets a timer configured to the configured ping interval.
- When the timer expires, the server checks whether the connection has been idle long enough.
- If the client has been quiet for too long, the server closes the socket as idle.
- Otherwise, it sends a WebSocket ping frame with payload `urfd`.
- The peer is expected to respond with a corresponding pong frame.
- If the ping fails or the client does not respond in a timely way, the server closes the connection.

This is not a user-space application-level ping/pong protocol; it is the standard WebSocket control frame mechanism implemented by libwebsockets.

## Notes

- The server does not accept messages from clients for this notifier path; it only pushes state updates to connected websocket clients.
- The server sends `initial_connect` first so the browser or UI can render the reflector identity, module list, and enabled protocols immediately. The subsequent `state_snapshot` then shows the live runtime state.
- The server intentionally omits client IP addresses from websocket payloads for privacy and UI simplicity.
