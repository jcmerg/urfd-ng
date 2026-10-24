# URFD WebSocket Dashboard

This directory contains the current Vue-based dashboard for the URFD WebSocket notifier. It replaces the legacy PHP/XML dashboard pages with a browser app that consumes the live notifier stream and renders the reflector state in a more modern UI.

The browser code is intentionally static and uses ES modules directly, while the Python/Flask app in this folder provides the runtime config and optional proxy endpoints for data sources that are not exposed by the notifier itself.

## What is included

- `server.py`: Flask app that serves the static frontend and exposes `/config.js`
- `static/index.html`: entry page for the app
- `static/app.js`: app bootstrap
- `static/components/*.js`: Vue page components for the dashboard
- `static/lib/notifier.js`: WebSocket client logic
- `static/styles.css`: dashboard styling
- `settings.ini.example`: sample runtime configuration

The dashboard reads live reflector data from the URFD notifier’s `initial_connect`, `state_snapshot`, and event messages.

## Current dashboard pages

The working dashboard currently includes:

- Last heard
- Modules
- Protocols
- Links
- Peers
- Reflector list (when `enable_reflector_list` is enabled)
- QuadNet live (when `enable_quadnet_live` is enabled)

## Running the dashboard

This Python server is intended for local development only. For a production-style process manager, run the app with Gunicorn.

```bash
cd dashboard-ws
python3 -m pip install -r requirements.txt
gunicorn --bind 0.0.0.0:8000 server:app
```

Then open:

```text
http://localhost:8000/
```

The app listens on port `8000` by default and uses the configured `ws_url` from `settings.ini` or a matching environment variable.

For local debugging only, you may still run the Flask development server directly:

```bash
cd dashboard-ws
python3 server.py
```

## Configuration

Copy the example settings file before editing it:

```bash
cd dashboard-ws
cp settings.ini.example settings.ini
```

The sample config looks like this:

```ini
[default]
enable_reflector_list = false
enable_quadnet_live = false
vue_url = https://cdn.jsdelivr.net/npm/vue@3/dist/vue.esm-browser.prod.js
ws_url =
```

Options:

- `ws_url`: WebSocket URL to the URFD notifier, such as `ws://reflector.example:8080` or `wss://reflector.example:8080`
- `enable_reflector_list`: Enables the optional reflector directory page and `/api/reflector-list`
- `enable_quadnet_live`: Enables the optional QuadNet live feed and `/api/quadnet`
- `vue_url`: Override for the Vue runtime URL if needed
- `port`: Optional Flask port override (default `8000`)
- `debug`: Optional Flask debug mode flag

Dynaconf reads these settings from `settings.ini` and also supports environment overrides using the `URFD_DASHBOARD_` prefix. For example:

```bash
export URFD_DASHBOARD_ENABLE_REFLECTOR_LIST=true
export URFD_DASHBOARD_ENABLE_QUADNET_LIVE=true
export URFD_DASHBOARD_WS_URL=ws://localhost:8080
```

## Static mode

The frontend can also be served directly as a static site if you want to test it without the Python app:

```bash
cd dashboard-ws/static
python3 -m http.server 8000
```

When running in static mode, create a local `config.js` next to `index.html` if you need a custom runtime config. This is the place to override the WebSocket endpoint when the dashboard is served from a different host or behind a reverse proxy:

```javascript
window.__URFD_DASHBOARD_CONFIG__ = {
  __configLoaded: true,
  wsUrl: 'wss://reflector.example.com:8080',
  reflectorListUrl: null,
  quadnetLiveUrl: null,
};
```

If no config is present, the app falls back to the default production Vue URL and the same-host WebSocket endpoint.

## Proxy endpoints

The Flask app exposes optional proxy endpoints when the matching feature flag is enabled:

```text
/api/reflector-list
/api/quadnet
```

These are for external data sources that are not directly exposed by the notifier stream.

## Production notes

- No build step is required for the browser-side Vue code; it is loaded as ES modules.
- Keep the production Vue runtime URL in place unless you specifically need a different CDN or local override.
- This dashboard is a modern replacement for the legacy dashboard views and does not serve the older PHP dashboard assets.
- The legacy dashboard code under `dashboard/` is kept separate and is not part of the runtime served by this app.
