// Runtime settings when nginx serves this folder as /live/ next to the PHP
// dashboard (no Flask): Vue from this folder, the WebSocket through /ws on the
// same host, no reflector list or QuadNet proxy.
window.__URFD_DASHBOARD_CONFIG__ = {
  __configLoaded: true,
  vueUrl: './lib/vue.esm-browser.prod.js',
  wsUrl: (window.location.protocol === 'https:' ? 'wss://' : 'ws://') + window.location.host + '/ws',
  reflectorListUrl: null,
  quadnetLiveUrl: null,
};
