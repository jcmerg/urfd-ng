# URFD live dashboard (WebSocket)

The live dashboard by Jason Kendall (coolacid): a static Vue app that shows the reflector as it changes, from urfd's WebSocket notifier. It runs next to the PHP dashboard, not instead of it.

## What is included

- `static/index.html`, `static/app.js`: the entry page and the app
- `static/components/*.js`: the pages (last heard, feed, modules, protocols, links, peers; the reflector list stays off, see below)
- `static/lib/`: the WebSocket client (`notifier.js`), Vue, and `config.js`, which reads the runtime settings
- `static/config.js`: the runtime settings: Vue from `lib/`, the WebSocket at `/ws` (without it, `lib/config.js` falls back to Vue from a CDN)
- `static/styles.css`: the styling

It reads the notifier's `initial_connect`, `state_snapshot` and event messages, see [docs/WEBSOCKET.md](../docs/WEBSOCKET.md).

## How it is served

There is no server of its own. The web server hands out `static/` as `/live/` and passes urfd's notifier (`127.0.0.1:10102`) through as `/ws` on the same host; `config.js` points the app at `/ws` and loads Vue from the folder. The Docker image does both (`docker/nginx-dashboard.conf`), and urfd needs `[WebSocket] Enable = true` (`setup.sh` sets it).

For an nginx of your own, the `/ws` block is in [docs/WEBSOCKET.md](../docs/WEBSOCKET.md); `/live/` is a plain static location on this folder:

```nginx
location /live/ {
    alias /var/www/urfd-dashboard-ws/;
    index index.html;
}
```

The reflector list and QuadNet pages of the original need a proxy server and are off here (`reflectorListUrl` and `quadnetLiveUrl` are `null` in `config.js`).
