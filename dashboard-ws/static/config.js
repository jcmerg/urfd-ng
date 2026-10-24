// Runtime settings when nginx serves this folder as /live/ next to the PHP
// dashboard (no Flask): Vue from this folder, the WebSocket through /ws on the
// same host, no reflector list or QuadNet proxy.
window.__URFD_DASHBOARD_CONFIG__ = {
  __configLoaded: true,
  // absolute: the modules import it from their own folders
  vueUrl: new URL('lib/vue.esm-browser.prod.js', window.location.href).href,
  wsUrl: (window.location.protocol === 'https:' ? 'wss://' : 'ws://') + window.location.host + '/ws',
  reflectorListUrl: null,
  quadnetLiveUrl: null,
};
