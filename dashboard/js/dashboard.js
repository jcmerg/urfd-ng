/*
 *  URF dashboard, originally the XLX dashboard by the DVBrazil Team.
 *  A static page: the state comes live from urfd's WebSocket (/ws), the
 *  reflector list and QuadNet from nginx, which passes them through.
 */
(function () {
'use strict';

var DASHBOARD_VERSION = '3.1.11';
var REPO_URL = 'https://github.com/jcmerg/urfd-ng';

var $ = function (id) { return document.getElementById(id); };

// text for the page, escaped
function h(s) {
    return String(s === undefined || s === null ? '' : s)
        .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

// ---------------------------------------------------------------------------
// state, as urfd sends it

var S = {
    info: null,          // initial_connect: reflector, modules, protocols, dashboard options
    modules: {},         // letter -> module of initial_connect, mappings kept up to date
    links: [],           // linked nodes
    peers: [],           // peer reflectors
    lastHeard: [],       // newest first
    onAir: {},           // module -> the station transmitting there
    started: 0,          // when urfd started, its clock
    skew: 0,             // our clock minus urfd's, in seconds
    connected: false
};

function now() { return Math.floor(Date.now() / 1000 - S.skew); }

// "N0CALL  B" (8 characters and the module) -> N0CALL and B
function csParts(raw) {
    raw = String(raw || '').replace(/\u0000/g, ' ');
    return { base: raw.slice(0, 8).trim(), mod: raw.slice(8).trim() };
}

function reflectorName() {
    return S.info ? String(S.info.reflector.callsign || '').replace('XLX', 'URF') : '';
}

function options() {
    return (S.info && S.info.dashboard) || { last_heard_rows: 40, filter: true, links_rows: 100, peers_rows: 100 };
}

// "just now", "4 min ago", "2 h 5 min ago", "3 d ago"
function formatAgo(time) {
    var s = now() - (time | 0);
    if (s < 60) return 'just now';
    var m = Math.floor(s / 60);
    if (m < 60) return m + ' min ago';
    if (m < 1440) return Math.floor(m / 60) + ' h ' + (m % 60) + ' min ago';
    return Math.floor(m / 1440) + ' d ago';
}

// a duration: "45 min", "3 h 12 min", "4 d 2 h"
function formatSpan(seconds) {
    var m = Math.floor(Math.abs(seconds | 0) / 60), d = Math.floor(m / 1440), hr = Math.floor((m % 1440) / 60);
    if (d > 0) return d + ' d ' + hr + ' h';
    if (hr > 0) return hr + ' h ' + (m % 60) + ' min';
    return (m % 60) + ' min';
}

function formatDate(time) {
    var d = new Date(time * 1000), p = function (n) { return String(n).padStart(2, '0'); };
    return p(d.getDate()) + '.' + p(d.getMonth() + 1) + '.' + d.getFullYear() + ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
}

// the country of a callsign from its prefix: [ISO code, name]
var prefixes = null;
function country(cs) {
    if (!prefixes) {
        prefixes = {};
        (window.URF_COUNTRIES || []).forEach(function (c) { c[2].forEach(function (p) { prefixes[p] = c; }); });
    }
    cs = String(cs || '').trim();
    for (var n = 4; n >= 2; n--) {
        var c = prefixes[cs.slice(0, n)];
        if (c) return [c[1].toUpperCase(), c[0]];
    }
    return ['', ''];
}

function countryTag(cs) {
    var c = country(cs);
    return c[0] ? '<span class="cc" title="' + h(c[1]) + '">' + h(c[0]) + '</span>' : '';
}

// a station of the last heard list, ready to show
function station(u) {
    var my = csParts(u.callsign), node = csParts(u.via_node), peer = csParts(u.via_peer);
    // how it reached us: its node, and the peer when it came in from another reflector
    var via = node.base + (node.mod ? '-' + node.mod : '');
    var refl = String(S.info ? S.info.reflector.callsign : '');
    if (peer.base && peer.base !== refl && peer.base !== reflectorName()) via += (via ? ' / ' : '') + peer.base + (peer.mod ? ' ' + peer.mod : '');
    return {
        cs: my.base, suffix: u.suffix || '', name: u.name || '', module: u.on_module || '',
        protocol: u.protocol || '', via: via, time: u.last_heard_time | 0,
        locator: u.locator || '', lat: u.latitude, lon: u.longitude,
        audio: u.audio || null, verdict: u.audio ? audioVerdict(u.audio) : null,
        duration: u.duration_ms | 0,	// the stream's length once it ended (urfd 4.40.6), on every module
        key: [u.callsign, u.suffix || '', u.via_node, u.on_module, u.via_peer].join('|')
    };
}

function linkedNodes(mod) {
    return S.links.filter(function (l) { return String(l.modules || l.reflector_module).indexOf(mod) >= 0; });
}

// everyone on the air, the transmission that started last first
function onAirAll() {
    return Object.keys(S.onAir).map(function (m) { return S.onAir[m]; })
        .sort(function (a, b) { return b.time - a.time; });
}

// ---------------------------------------------------------------------------
// events

function setOnAirFromChannels(channels) {
    S.onAir = {};
    (channels || []).forEach(function (c) {
        var user = csParts(c.user).base, found = null;
        for (var i = 0; i < S.lastHeard.length; i++) {
            var st = S.lastHeard[i];
            if (st.module === c.module && st.cs === user) { found = st; break; }
        }
        S.onAir[c.module] = found || station({ callsign: c.user, name: c.name, on_module: c.module, protocol: c.protocol, via_node: c.callsign, last_heard_time: now() });
    });
}

function nodeKey(n) { return n.callsign + '|' + n.protocol; }

var handlers = {
    initial_connect: function (p) {
        S.info = p;
        S.skew = Date.now() / 1000 - (p.current_time || Date.now() / 1000);
        S.started = p.server_started || 0;
        S.modules = {};
        (p.modules || []).forEach(function (m) { S.modules[m.name] = m; });
        shell();
    },
    state_snapshot: function (p) {
        S.skew = Date.now() / 1000 - (p.current_time || Date.now() / 1000);
        S.links = p.links || [];
        S.peers = p.peers || [];
        S.lastHeard = (p.last_heard || []).map(station);
        setOnAirFromChannels(p.open_channels);
        // transcoding follows tcd while urfd runs
        if (typeof p.transcoded === 'string') Object.keys(S.modules).forEach(function (m) { S.modules[m].transcoded = p.transcoded.indexOf(m) >= 0; });
    },
    client_added: function (p) {
        S.links = S.links.filter(function (n) { return nodeKey(n) !== nodeKey(p); });
        S.links.push(p);
    },
    client_removed: function (p) {
        S.links = S.links.filter(function (n) { return nodeKey(n) !== nodeKey(p); });
    },
    peer_added: function (p) {
        S.peers = S.peers.filter(function (n) { return n.callsign !== p.callsign; });
        S.peers.push(p);
    },
    peer_removed: function (p) {
        S.peers = S.peers.filter(function (n) { return n.callsign !== p.callsign; });
    },
    station_started: function (p) { S.onAir[p.on_module] = station(p); },
    station_stopped: function (p) { delete S.onAir[p.on_module]; },
    user_heard: function (p) {
        var st = station(p);
        S.lastHeard = S.lastHeard.filter(function (u) { return u.key !== st.key; });
        var i = 0;
        while (i < S.lastHeard.length && S.lastHeard[i].time > st.time) i++;
        S.lastHeard.splice(i, 0, st);
        // as long as urfd keeps it, so a reload shows the same list
        var max = options().last_heard_rows || 40;
        if (S.lastHeard.length > max) S.lastHeard.length = max;
        // the station on the air now has its position as well
        var air = S.onAir[st.module];
        if (air && air.cs === st.cs && st.locator) { air.locator = st.locator; air.lat = st.lat; air.lon = st.lon; }
    },
    // an entry that changed its callsign (a talker renamed during the stream)
    user_removed: function (p) {
        var key = station(p).key;
        S.lastHeard = S.lastHeard.filter(function (u) { return u.key !== key; });
    },
    last_heard_cleared: function () { S.lastHeard = []; },
    // the transcoder's report of a transmission, after it: its entry gets the audio
    user_report: function (p) {
        var st = station(p);
        for (var i = 0; i < S.lastHeard.length; i++) if (S.lastHeard[i].key === st.key) { S.lastHeard[i].audio = st.audio; S.lastHeard[i].verdict = st.verdict; S.lastHeard[i].duration = st.duration; break; }
    },
    module_mappings: function (p) {
        Object.keys(p.modules || {}).forEach(function (m) { if (S.modules[m]) S.modules[m].mappings = p.modules[m]; });
    }
};

// ---------------------------------------------------------------------------
// the WebSocket: events, and requests with an answer

var ws = null, retry = 1000, nextId = 1, pending = {};
var URF = window.URF = { request: request, onOpen: [], onClose: [], h: h, state: S, formatSpan: formatSpan, now: now, pages: {} };

// a tab left open keeps the scripts of its load: after an update it reconnects to
// the new urfd with the old page. At every connect the page asks the server for
// the version of this file and loads itself again when it differs, once per
// version (sessionStorage), so a cache in between cannot make it loop
function checkVersion() {
    fetch('js/dashboard.js', { cache: 'no-store' }).then(function (r) { return r.ok ? r.text() : ''; }).then(function (text) {
        var m = /DASHBOARD_VERSION = '([^']+)'/.exec(text);
        if (!m || m[1] === DASHBOARD_VERSION) return;
        var done = null;
        try { done = sessionStorage.getItem('urf-reloaded-for'); sessionStorage.setItem('urf-reloaded-for', m[1]); } catch (e) {}
        if (done !== m[1]) location.reload();
    }).catch(function () {});
}
URF.onOpen.push(checkVersion);

function connect() {
    ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
    ws.onopen = function () {
        S.connected = true; retry = 1000;
        URF.onOpen.forEach(function (fn) { fn(); });
    };
    ws.onmessage = function (e) {
        var msg;
        try { msg = JSON.parse(e.data); } catch (x) { return; }
        if (msg.event_type === 'reply') {
            var p = pending[msg.id];
            if (p) { delete pending[msg.id]; clearTimeout(p.timer); p.resolve(msg.payload || {}); }
            return;
        }
        var fn = handlers[msg.event_type];
        if (fn) { fn(msg.payload || {}); schedule(); }
    };
    ws.onclose = function () {
        S.connected = false;
        Object.keys(pending).forEach(function (id) { clearTimeout(pending[id].timer); pending[id].resolve({ status: 'error', message: 'The connection to the reflector was lost' }); });
        pending = {};
        URF.onClose.forEach(function (fn) { fn(); });
        schedule();
        setTimeout(connect, retry);
        retry = Math.min(retry * 2, 15000);
    };
}

// a request to urfd, answered with {status, message, ...}
function request(cmd, args) {
    return new Promise(function (resolve) {
        if (!ws || ws.readyState !== 1) { resolve({ status: 'error', message: 'Not connected to the reflector' }); return; }
        var id = nextId++, msg = Object.assign({}, args || {}, { cmd: cmd, id: id });
        pending[id] = { resolve: resolve, timer: setTimeout(function () {
            delete pending[id]; resolve({ status: 'error', message: 'No answer from the reflector yet; it may still carry the command out, look again in a moment' });
        }, 20000) };
        ws.send(JSON.stringify(msg));
    });
}

// ---------------------------------------------------------------------------
// the shell: top bar, MOTD, on-air line, footer

function shell() {
    var name = reflectorName(), d = options();
    $('brand-name').textContent = name || 'URF';
    document.title = (name ? name + ' ' : '') + 'Universal Reflector';
    // the release this reflector runs, or the repository while the version is not known yet
    var ver = S.info && S.info.version ? String(S.info.version) : '';
    var url = ver ? REPO_URL + '/releases/tag/v' + encodeURIComponent(ver) : REPO_URL;
    $('version').innerHTML = (ver ? '<a href="' + h(url) + '" target="_blank" rel="noopener">urfd-ng ' + h(ver) + '</a>, ' : '') + 'dashboard ' + DASHBOARD_VERSION;
    // missing in an index.html of before 3.1.6 a cache may still hand out
    var link = $('source-link'), text = $('source-text');
    if (link) link.href = url;
    if (text) text.textContent = 'urfd-ng' + (ver ? ' ' + ver : '') + ' on GitHub';
    var motd = String(d.motd || '').trim();
    $('motd').hidden = !motd;
    $('motd-text').textContent = motd;
    // the address is put together by script, against harvesters
    var email = S.info && S.info.reflector.email, parts = String(email || '').split('@');
    $('sysop').innerHTML = parts.length === 2 ? 'Sysop: <a href="mai' + 'lto:' + h(parts[0]) + '&#64;' + h(parts[1]) + '">' + h(parts[0]) + '&#64;' + h(parts[1]) + '</a>' : '';
    $('admin-link').hidden = !d.admin;
}

function shellUpdate() {
    $('nav-links').textContent = S.links.length;
    $('nav-peers').textContent = S.peers.length;
    var up = $('uptime');
    if (!S.connected) { up.textContent = 'Reflector not reachable, retrying'; up.classList.add('state-bad'); }
    else { up.textContent = S.started ? 'Up ' + formatSpan(now() - S.started) : ''; up.classList.remove('state-bad'); }
}

// the "also on" box for the other modules on the air: three letters at most, the rest as a count
function alsoOnAir(mods) {
    if (!mods.length) return '';
    var shown = mods.slice(0, 3), more = mods.length - shown.length;
    return '<span class="lcd-also" title="Also on the air: module ' + h(mods.join(', ')) + '">also on '
        + h(shown.join(', ')) + (more > 0 ? ' +' + more : '') + '</span>';
}

function otherOnAir(mod) {
    return Object.keys(S.onAir).filter(function (m) { return m !== mod; }).sort();
}

// the slim LCD line above every page but Last heard and Admin
function strip(show) {
    var el = $('strip');
    if (!show) { el.hidden = true; return; }
    el.hidden = false;
    var air = onAirAll()[0];
    if (air) {
        el.className = 'strip';
        el.setAttribute('aria-live', 'polite');
        el.innerHTML = '<div class="strip-in"><span class="ann">TX</span><b>' + h(air.cs) + '</b>' + (air.name ? '<span>' + h(air.name) + '</span>' : '')
            + '<span>Module ' + h(air.module) + '</span>' + (air.protocol ? '<span>' + h(air.protocol) + '</span>' : '')
            + (air.via ? '<span>via ' + h(air.via) + '</span>' : '') + alsoOnAir(otherOnAir(air.module))
            + '<span class="t" data-since="' + air.time + '"></span></div>';
        return;
    }
    el.className = 'strip idle';
    var st = visibleHeard()[0];
    el.innerHTML = st
        ? '<div class="strip-in"><span>Last heard</span><b>' + h(st.cs) + '</b>' + (st.name ? '<span>' + h(st.name) + '</span>' : '')
            + '<span>Module ' + h(st.module) + '</span>' + (st.protocol ? '<span>' + h(st.protocol) + '</span>' : '')
            + '<span class="t">' + formatAgo(st.time) + '</span></div>'
        : '<div class="strip-in"><span>' + (onlyShort('') ? 'Short calls hidden' : 'Nobody heard yet') + '</span></div>';
}

// running transmit timers, against urfd's clock
function tick() {
    document.querySelectorAll('[data-since]').forEach(function (el) {
        var s = Math.max(0, now() - parseInt(el.dataset.since, 10));
        el.textContent = Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
    });
}

function waiting() {
    return '<div class="note"><p>' + (S.connected ? 'Loading the reflector state' : 'Connecting to the reflector') + '</p></div>';
}

// ---------------------------------------------------------------------------
// the audio of a transmission, as the transcoder reported it (transcoded modules
// only): how loud it came in, how much of it clipped, what the AGC did and how
// many AMBE frames the FEC could not save. Green fine, orange to look at, red
// bad; red only as the colour of the meter and the word, the filled red tag
// stays the one for transmitting

function dB(v) { return (v > 0 ? '+' : v < 0 ? '\u2212' : '') + Math.abs(v).toFixed(1) + ' dB'; }

// a transmission too short to rate its level: under 1.5 s, or under 1 s of speech
// (a kerchunk, a quick "ok"); a few frames would make the verdict chance. One
// without any speech is "No speech" only from 3 s on: a kerchunk may hold the
// key for 2 s, a longer silent one is a stuck PTT or a dead microphone
var SHORT_MS = 1500, SHORT_SPEECH_MS = 1000, NO_SPEECH_MS = 3000, MIN_FEC_FRAMES = 50;
// "Hide short calls": under HIDE_MS (as WPSD), and a longer one with under HIDE_SPEECH_MS
// of speech (a kerchunk with the key held). A "73" or "danke" of HIDE_MS or more has
// speech and stays, though "Short" (too short to rate its level)
var HIDE_MS = 1000, HIDE_SPEECH_MS = 300;
function speechMs(a) { return a.duration_ms * (a.speech_percent || 0) / 100; }
// what "Hide short calls" leaves out: an ended transmission shorter than HIDE_MS by
// urfd's length (header to last packet), on every module and before and after tcd's
// report alike, so an entry does not vanish when the report comes (tcd's length, voice
// frames only, for an urfd before 4.40.6 that sends none). With tcd's report also a
// longer one with next to no speech, unless the FEC or the network muted it; never
// "No speech" (a stuck PTT or a dead microphone is worth seeing). Not the same as the
// "Short" verdict, which only says the level was not rated
function hideable(st) {
    var a = st.audio;
    var len = st.duration > 0 ? st.duration : a ? a.duration_ms : 0;
    if (len > 0 && len < HIDE_MS) return true;
    if (!a || !st.verdict || st.verdict.w === 'No speech') return false;
    var muted = a.network_lost > 0 || (a.fec && a.fec.uncorrectable > 0);
    return speechMs(a) < HIDE_SPEECH_MS && !muted;
}
// the entries the log has room for
function logList() { return S.lastHeard.slice(0, options().last_heard_rows || 40); }
// the last heard entries a visitor sees: those of the log, without the short calls when they are hidden
function visibleHeard() {
    var list = logList();
    return hideShort ? list.filter(function (st) { return !hideable(st); }) : list;
}
// short calls hidden where nothing else was heard (all modules for key '')
function onlyShort(key) {
    return hideShort && logList().some(function (st) { return (!key || st.module === key) && hideable(st); });
}

// the input level for the log and the LCD: none without speech (it would read -100 dB)
// and none for a short transmission (it would be rated by a few frames)
function audioLevel(a, v) { return a.speech_percent && !v.short ? dB(a.level_in_db) : ''; }

// sev: 0 fine, 1 to look at, 2 bad, for the word; level: that of the level alone, for
// the meter; short: no level verdict, grey. Bit errors and packet loss count from
// MIN_FEC_FRAMES on, so one bad frame of a kerchunk is not "33 %". The level bands
// follow the codec tests of 3GPP (-26 dBov nominal, -16 to -36 dBov as normal): Quiet
// below that, Too quiet from 20 dB below nominal, where the AGC comes to its limit
function audioVerdict(a) {
    var rated = a.fec && a.fec.frames >= MIN_FEC_FRAMES;
    var lost = rated ? a.fec.uncorrectable / a.fec.frames * 100 : 0;
    var level = 0, w = 'Good', short = false;
    var tooShort = a.duration_ms < SHORT_MS || speechMs(a) < SHORT_SPEECH_MS;	// "Short", and with no speech too
    if (!a.speech_percent && a.duration_ms >= NO_SPEECH_MS) { level = 1; w = 'No speech'; }	// a carrier without modulation, or all of it muted
    else if (tooShort) { short = true; w = 'Short'; }
    else if (a.clipped_percent >= 5 || a.level_in_db > -8) { level = 2; w = 'Overdriven'; }
    else if (a.level_in_db < -46) { level = 2; w = 'Too quiet'; }
    else if (a.clipped_percent >= 1 || a.level_in_db > -12) { level = 1; w = 'Loud'; }
    else if (a.level_in_db < -36) { level = 1; w = 'Quiet'; }
    var fec = lost >= 5 ? 2 : lost >= 2 ? 1 : 0;
    var net = a.network_lost && rated ? a.network_lost / a.fec.frames * 100 : 0;
    var loss = net >= 5 ? 2 : net >= 2 ? 1 : 0;
    if (fec > level && fec >= loss) w = 'Bit errors';
    else if (loss > level) w = 'Packet loss';
    var sev = Math.max(level, fec, loss);
    return { w: w, sev: sev, level: level, warn: sev > 0, short: short && sev === 0 };
}

var SEV = ['', ' warn', ' bad'];

function audioDetails(a) {
    var lines = ['Input ' + dB(a.level_in_db) + 'FS, peak ' + dB(a.peak_in_db) + 'FS, ' + a.clipped_percent + ' % clipped'];
    if (a.agc_db) lines.push('AGC ' + dB(a.agc_db.avg) + ' (' + dB(a.agc_db.min) + ' to ' + dB(a.agc_db.max) + '), out ' + dB(a.level_out_db) + 'FS');
    else lines.push('Out ' + dB(a.level_out_db) + 'FS, no AGC');
    if (a.fec) lines.push('FEC: ' + a.fec.frames + ' frames, ' + a.fec.corrected + ' repaired, ' + a.fec.uncorrectable + ' beyond repair');
    if (a.decoder) lines.push('Decoder: ' + a.decoder.repeated + ' repeated, ' + a.decoder.muted + ' muted');
    if (a.network_lost) lines.push('Network: ' + a.network_lost + ' frames lost on the way, filled in');
    lines.push((a.duration_ms / 1000).toFixed(1) + ' s, ' + (speechMs(a) / 1000).toFixed(1) + ' s of it speech');
    if (audioVerdict(a).short) lines.unshift('Too short to rate the level');
    return lines.join('\n');
}

// the log's cell: a small meter of the input level, the verdict and the AGC. The meter
// starts at -50 dBFS, where a frame counts as speech, so a level with speech always
// shows (a "Too quiet" one below -46 dBFS too), at least a sliver of it
function meterPos(db) { return Math.max(0, Math.min(100, (db + 50) / 50 * 100)); }

function audioCell(a, v) {
    if (!a) return '';
    var fill = a.speech_percent ? Math.max(4, meterPos(a.level_in_db)) : 0, peak = meterPos(a.peak_in_db);
    // two causes: frames the FEC could not repair (bad), frames lost on the way (lost)
    var bad = a.fec ? a.fec.uncorrectable : 0, lost = a.network_lost || 0, muted = a.decoder ? a.decoder.muted : 0;
    // a short one is not rated: the empty track keeps the column in line, the values are in the tooltip
    if (v.short)
        return '<span class="lvl short" title="' + h(audioDetails(a)) + '"><span class="lvl-bar" aria-hidden="true"></span><span class="lvl-w">' + v.w + '</span></span>';
    return '<span class="lvl' + SEV[v.sev] + ' lv' + v.level + '" title="' + h(audioDetails(a)) + '">'
        + '<span class="lvl-bar" aria-hidden="true"><i style="width:' + fill.toFixed(0) + '%"></i><b style="left:' + peak.toFixed(0) + '%"></b></span>'
        + '<span class="lvl-w">' + v.w + '</span>'
        + '<span class="lvl-n">' + [audioLevel(a, v), a.agc_db && Math.abs(a.agc_db.avg) >= 0.5 ? 'AGC ' + dB(a.agc_db.avg) : '',
            bad ? bad + ' bad' : '', lost ? lost + ' lost' : '', muted ? muted + ' muted' : ''].filter(Boolean).join(', ') + '</span></span>';
}

// ---------------------------------------------------------------------------
// Last heard: the LCD with who is on the air, the module keypad and the log

var filter = { m: null, cs: '' }, lastCall = null, lastCallT = 0;
try { var saved = JSON.parse(sessionStorage.getItem('urf-filter') || 'null'); if (saved) filter = saved; } catch (e) {}
// the visitor's choice to leave out short transmissions (kerchunks), kept in the browser;
// only a transmission tcd reported on can be told short (a transcoded module)
var hideShort = false;
try { hideShort = localStorage.getItem('urf-hide-short') === '1'; } catch (e) {}
// switched in another tab of this dashboard
window.addEventListener('storage', function (e) {
    if (e.key !== 'urf-hide-short') return;
    hideShort = e.newValue === '1';
    lastCall = null;
    if (S.info) render();
});

function glob(pat, s) {
    if (!pat) return true;
    if (pat.indexOf('*') < 0) pat = '*' + pat + '*';
    return new RegExp('^' + pat.split('*').map(function (p) { return p.replace(/[.+?^${}()|[\]\\]/g, '\\$&'); }).join('.*') + '$', 'i').test(s);
}

function lcdPanel(key, air, last, hidden) {
    var st = air || last, mod = st ? st.module : key;
    var cls = 'lcd' + (air ? '' : ' idle') + (st ? '' : ' empty');
    var out = '<div class="' + cls + '" data-lcd="' + h(key) + '" data-call="' + h(st ? mod + st.cs : mod) + '" data-t="' + (st ? st.time : 0) + '"' + (hidden ? ' hidden' : '') + '><div class="lcd-top">';
    if (air) out += '<span class="ann">TX</span><span>Transmitting</span>';
    else if (last) out += '<span>Last heard ' + formatAgo(last.time) + '</span>';
    else if (onlyShort(key)) out += '<span>Short calls hidden</span>';
    else out += '<span>' + (key ? 'Nothing heard on module ' + h(key) + ' yet' : 'Nobody heard yet') + '</span>';
    out += alsoOnAir(otherOnAir(air ? air.module : key));
    if (air) out += '<span class="lcd-timer" data-since="' + air.time + '"></span>';
    out += '</div>';
    if (st) {
        out += '<p class="lcd-call">' + h(st.cs) + (st.suffix ? '<small>/' + h(st.suffix) + '</small>' : '')
            + (st.name ? '<span class="lcd-name">' + h(st.name) + '</span>' : '') + '</p>'
            + '<div class="lcd-ch"><span>Module</span><b>' + h(mod) + '</b></div><dl class="lcd-fields">'
            + '<div><dt>Mode</dt><dd>' + (st.protocol ? h(st.protocol) : 'unknown') + '</dd></div>'
            + '<div><dt>Via</dt><dd>' + h(st.via) + '</dd></div>'
            + (st.locator ? '<div><dt>Locator</dt><dd>' + h(st.locator) + '</dd></div>' : '')
            + (!air && st.audio ? '<div title="' + h(audioDetails(st.audio)) + '"><dt>Audio</dt><dd>' + st.verdict.w + (audioLevel(st.audio, st.verdict) ? ', ' + audioLevel(st.audio, st.verdict) : '') + '</dd></div>'
                + (st.audio.agc_db ? '<div><dt>AGC</dt><dd>' + dB(st.audio.agc_db.avg) + '</dd></div>' : '') : '') + '</dl>';
    } else {
        out += '<p class="lcd-call">' + (onlyShort(key) ? 'Only short calls' : key ? 'Quiet so far' : 'Waiting for the first call') + '</p>'
            + (key ? '<div class="lcd-ch"><span>Module</span><b>' + h(key) + '</b></div>' : '');
    }
    return out + '</div>';
}

URF.pages.users = {
    refresh: true,
    mount: function (el) {
        var f = options().filter;
        el.innerHTML = '<div class="page"><section class="deck" aria-label="On the air"><div class="bezel" id="lcds" aria-live="polite"></div>'
            + '<div><div class="keys-head"><h2>Modules</h2>' + (f ? '<p class="hint">Tap one to follow it in the display and the log</p>' : '') + '</div><div class="keys" id="keys"></div></div></section>'
            + '<section aria-labelledby="log-title"><div class="tbl-head"><h2 id="log-title">Last heard</h2>'
            + '<div class="filters">' + (f ? '<label for="f-call">Callsign</label><input class="field" id="f-call" type="search" placeholder="DL*, *JC" autocomplete="off" spellcheck="false"><button class="chip" id="f-clear" type="button" hidden>Clear filters</button>' : '')
            + '<button class="chip" id="f-short" type="button" aria-pressed="false" title="Leave out ended transmissions under ' + HIDE_MS / 1000 + ' s (kerchunks), and longer ones with under ' + HIDE_SPEECH_MS / 1000 + ' s of speech unless bit errors or packet loss muted them (without a report from tcd by the length alone); never one with no speech at all from 3 s on">Hide short calls</button></div>'
            + '</div><div class="scroll"><table class="t" id="log"><thead><tr><th class="state"><span class="sr">State</span></th><th>Callsign</th><th>Module</th><th>Mode</th><th>Via</th><th class="opt">Position</th><th class="opt">Audio</th><th class="r">Heard</th></tr></thead><tbody id="log-body"></tbody></table></div></section></div>';
        if (!f) filter = { m: null, cs: '' };
        el.addEventListener('click', function (e) {
            var k = e.target.closest('#keys button.key');
            if (k) { filter.m = (filter.m === k.dataset.m) ? null : k.dataset.m; applyFilter(); return; }
            if (e.target.closest('#f-clear')) { filter = { m: null, cs: '' }; $('f-call').value = ''; applyFilter(); }
            if (e.target.closest('#f-short')) {
                hideShort = !hideShort;
                try { localStorage.setItem('urf-hide-short', hideShort ? '1' : '0'); } catch (e2) {}
                lastCall = null;	// the display changes, no new call: no flick
                render();
            }
        });
        el.addEventListener('input', function (e) { if (e.target.id === 'f-call') { filter.cs = e.target.value.trim(); applyFilter(); } });
    },
    update: function () {
        var f = options().filter, mods = Object.keys(S.modules).sort();
        var air = onAirAll();
        // the last heard as the log shows it: without the short calls it hides
        var heard = visibleHeard();
        var lcd = lcdPanel('', air[0] || null, heard[0] || null, false);
        if (f) mods.forEach(function (m) {
            var last = null;
            for (var i = 0; i < heard.length; i++) if (heard[i].module === m) { last = heard[i]; break; }
            lcd += lcdPanel(m, S.onAir[m] || null, last, true);
        });
        $('lcds').innerHTML = lcd;

        // up to 8 modules get 4 columns of tall keys; more get flatter keys, spread evenly over
        // at most 5 columns (9 = 5 + 4, 10 = 5 x 2, 12 = 4 x 3), every key with its module's name
        var many = mods.length > 8, cols = many ? Math.ceil(mods.length / Math.ceil(mods.length / 5)) : 4;
        var keys = $('keys'), out = '', n = 0;
        keys.className = 'keys' + (many ? ' many' : '');
        keys.style.gridTemplateColumns = many ? 'repeat(' + cols + ', minmax(0, 1fr))' : '';
        mods.forEach(function (m) {
            var info = S.modules[m], nodes = linkedNodes(m).length, tx = !!S.onAir[m], name = info.description || '';
            var cls = 'key' + (nodes > 0 ? ' busy' : '') + (tx ? ' tx' : '');
            var label = 'Module ' + m + (name ? ', ' + name : '') + ', ' + nodes + (nodes === 1 ? ' node' : ' nodes') + (tx ? ', transmitting' : '');
            var inner = '<i class="led"></i><span class="l">' + h(m) + '</span><span class="n">' + nodes + '</span><span class="d">' + h(name) + '</span>';
            out += f ? '<button type="button" class="' + cls + '" data-m="' + h(m) + '" aria-pressed="false" aria-label="' + h(label) + '" title="' + h(label) + '">' + inner + '</button>'
                     : '<div class="' + cls + '" aria-label="' + h(label) + '" title="' + h(label) + '" role="img">' + inner + '</div>';
            n++;
        });
        while (n === 0 || n % cols) { out += '<span class="key blank" aria-hidden="true"></span>'; n++; }
        keys.innerHTML = out;

        var rows = '', live = {}, list = logList();
        list.forEach(function (st) {
            var on = S.onAir[st.module], isLive = !!on && on.cs === st.cs && !live[st.module];
            if (isLive) live[st.module] = true;
            rows += '<tr' + (isLive ? ' class="live"' : '') + ' data-m="' + h(st.module) + '" data-cs="' + h(st.cs) + '"'
                + (!isLive && hideable(st) ? ' data-short="1"' : '') + '>'
                + '<td class="state">' + (isLive ? '<span class="txtag">TX</span>' : '') + '</td>'
                + '<td class="cs"><a href="https://www.qrz.com/db/' + encodeURIComponent(st.cs) + '" target="_blank" rel="noopener">' + h(st.cs) + '</a>'
                + (st.suffix ? '<span class="sfx">/' + h(st.suffix) + '</span>' : '') + (st.name ? '<span class="op">' + h(st.name) + '</span>' : '') + countryTag(st.cs) + '</td>'
                + '<td class="m"><span class="mod' + (isLive ? ' on' : '') + '">' + h(st.module) + '</span></td>'
                + '<td class="p">' + h(st.protocol) + '</td><td class="v">' + h(st.via) + '</td>'
                + '<td class="meta">' + h((st.protocol + (st.via ? ' via ' + st.via : '')).trim())
                + (st.audio && !isLive ? ' \u00b7 <span class="lvl-m' + (st.verdict.short ? ' short' : SEV[st.verdict.sev]) + '">' + st.verdict.w + (audioLevel(st.audio, st.verdict) ? ', ' + audioLevel(st.audio, st.verdict) : '') + '</span>' : '') + '</td><td class="opt">';
            if (st.locator) {
                // position sent along with the transmission (M17 GNSS, D-Star DPRS, YSF GPS), rounded to its locator
                var lat = Number(st.lat), lon = Number(st.lon);
                rows += '<a href="https://www.openstreetmap.org/?mlat=' + lat + '&amp;mlon=' + lon + '#map=12/' + lat + '/' + lon + '" target="_blank" rel="noopener" title="Position sent with the transmission">' + h(st.locator) + '</a>';
            } else {
                rows += '<a class="muted" href="https://aprs.fi/' + encodeURIComponent(st.cs) + '" target="_blank" rel="noopener">aprs.fi</a>';
            }
            rows += '</td><td class="opt au">' + (isLive ? '' : audioCell(st.audio, st.verdict)) + '</td><td class="r when" title="' + h(formatDate(st.time)) + '">' + (isLive ? 'on the air' : formatAgo(st.time)) + '</td></tr>';
        });
        if (!list.length) rows += '<tr class="empty"><td colspan="8">' + (S.info ? 'Nobody heard since the reflector started. Calls show up here as soon as someone keys up.' : 'Connecting to the reflector') + '</td></tr>';
        rows += '<tr class="empty" id="log-none" hidden><td colspan="8"></td></tr>';
        $('log-body').innerHTML = rows;
        applyFilter();
    }
};

function applyFilter() {
    var input = $('f-call');
    if (input && input !== document.activeElement) input.value = filter.cs;
    document.querySelectorAll('#keys button.key').forEach(function (k) { k.setAttribute('aria-pressed', String(k.dataset.m === filter.m)); });
    var shown = 0, short = 0, rows = document.querySelectorAll('#log tbody tr[data-m]');
    rows.forEach(function (tr) {
        var match = (!filter.m || tr.dataset.m === filter.m) && glob(filter.cs, tr.dataset.cs);
        var ok = match && !(hideShort && tr.dataset.short === '1');
        if (match && !ok) short++;
        tr.hidden = !ok; if (ok) shown++;
    });
    var sh = $('f-short'); if (sh) sh.setAttribute('aria-pressed', String(hideShort));
    var none = $('log-none');
    if (none) {
        none.hidden = !(rows.length && !shown);
        var fl = filter.m || filter.cs;
        none.firstChild.textContent = !fl ? 'Only short calls in the log. Turn off "Hide short calls" to see them.'
            : short ? 'Only short calls match these filters. Turn off "Hide short calls" to see them.'
            : 'Nobody in the log matches these filters. Clear the filters to see all stations.';
    }
    var clear = $('f-clear'); if (clear) clear.hidden = !(filter.m || filter.cs);
    try { sessionStorage.setItem('urf-filter', JSON.stringify(filter)); } catch (e) {}

    // the LCD follows the selected module; a new call on it flickers once, like the display switching over
    var shownLcd = null;
    document.querySelectorAll('#lcds .lcd').forEach(function (l) {
        var on = l.dataset.lcd === (filter.m || '');
        l.hidden = !on; if (on) shownLcd = l;
    });
    if (!shownLcd) { shownLcd = document.querySelector('#lcds .lcd[data-lcd=""]'); if (shownLcd) shownLcd.hidden = false; }
    if (shownLcd) {
        var key = shownLcd.dataset.lcd + '|' + shownLcd.dataset.call, t = +shownLcd.dataset.t || 0;
        // only a newer call: one hidden as short gives the display back to an older one
        if (lastCall !== null && key !== lastCall && key.split('|')[0] === lastCall.split('|')[0] && t >= lastCallT) { shownLcd.classList.remove('flick'); void shownLcd.offsetWidth; shownLcd.classList.add('flick'); }
        lastCall = key; lastCallT = t;
    }
    tick();
}

// ---------------------------------------------------------------------------
// Modules: what each module carries, who is linked to it and how other networks reach it

function missing() { return '<span class="muted">none</span>'; }

function mappingLabel(map) {
    var parts = [map.protocol], id = (map.id === undefined || map.id === null) ? '' : String(map.id);
    if (map.type === 'TG') parts.push('TG ' + id);
    if (map.type === 'RAN') parts.push('RAN ' + id);
    if (map.type === 'AutoLink') parts.push('autolink');
    if (map.type === 'Bridge') parts.push('bridge');
    if (id !== '' && map.type !== 'TG' && map.type !== 'RAN') parts.push('#' + id);
    if (map.timeslot) parts.push('TS' + map.timeslot);
    if (map.remote_name) parts.push(map.remote_name);
    var dyn = map.expires !== undefined;
    var title = dyn ? ' title="Added at runtime, ends in ' + h(formatSpan(Math.max(0, map.expires - now()))) + '"' : '';
    return '<span class="tag' + (dyn ? ' dyn' : '') + '"' + title + '>' + h(parts.join(' ')) + '</span>';
}

// client protocols connect out to other reflectors, nobody connects to them here
var hiddenProtos = ['MMDVMClient', 'DCSClient', 'DExtraClient', 'DPlusClient', 'YSFClient', 'SvxReflector'];
var protoTypes = {
    DExtra: 'D-Star', DPlus: 'D-Star', DCS: 'D-Star', G3: 'D-Star (Icom G3)',
    MMDVM: 'DMR hotspots and repeaters', DMRPlus: 'DMR+ masters', XLXPeer: 'XLX and BrandMeister peering',
    M17: 'M17', YSF: 'YSF and Wires-X', P25: 'P25', NXDN: 'NXDN',
    URF: 'URF peer reflectors', USRP: 'AllStar (USRP)', SVX: 'SvxLink nodes'
};

URF.pages.modules = {
    refresh: true,
    mount: function (el) {
        el.innerHTML = '<div class="page"><div><h1>Modules</h1><p class="lede">What each module carries, who is linked to it and how the other networks reach it. Dashed bridges were added at runtime and run out on their own.</p></div><div id="mod-body"></div></div>';
    },
    update: function () {
        if (!S.info) { $('mod-body').innerHTML = waiting(); return; }
        var protos = S.info.protocols || [], names = protos.map(function (p) { return p.name; });
        var mods = Object.keys(S.modules).sort();
        var showDmrPlus = names.indexOf('DMRPlus') >= 0, showYsf = names.indexOf('YSF') >= 0, showRan = names.indexOf('NXDN') >= 0;
        var showDst = showRan && mods.some(function (m) { return S.modules[m].NXDNDstId; });
        var cols = 5 + showDmrPlus + showYsf + showRan + showDst;
        var open = {};
        document.querySelectorAll('#mod-body details[open]').forEach(function (d) { open[d.id] = true; });

        var out = '<div class="scroll"><table class="t"><thead><tr><th>Module</th><th>Name</th><th class="r">Nodes</th>'
            + (showDmrPlus ? '<th>DMR+ TG</th>' : '') + (showYsf ? '<th>YSF DG-ID</th>' : '') + (showRan ? '<th>NXDN RAN</th>' : '') + (showDst ? '<th>NXDN DST-ID</th>' : '')
            + '<th>Transcoded</th><th>Bridged to</th></tr></thead><tbody>';
        mods.forEach(function (m) {
            var info = S.modules[m], nodes = linkedNodes(m), air = S.onAir[m];
            var val = function (v) { return (v === undefined || v === null || v === '') ? missing() : h(v); };
            out += '<tr><td class="nw"><span class="mod' + (nodes.length ? ' on' : '') + '">' + h(m) + '</span>' + (air ? ' <span class="txtag" title="' + h(air.cs) + ' is transmitting">TX</span>' : '') + '</td>'
                + '<td>' + (info.description ? h(info.description) : missing()) + (info.echo ? ' <span class="muted">(echo test)</span>' : '') + '</td>'
                + '<td class="r">' + nodes.length + '</td>'
                + (showDmrPlus ? '<td>' + val(info.DMRplus) + '</td>' : '') + (showYsf ? '<td>' + val(info.YSFDGID) + '</td>' : '')
                + (showRan ? '<td>' + val(info.NXDNRAN) + '</td>' : '') + (showDst ? '<td>' + val(info.NXDNDstId) + '</td>' : '')
                + '<td>' + (info.transcoded ? 'Yes' : '<span class="muted">No</span>') + '</td>'
                + '<td>' + ((info.mappings || []).length ? info.mappings.map(mappingLabel).join('') : missing()) + '</td></tr>';
            if (nodes.length) {
                out += '<tr><td></td><td colspan="' + (cols - 1) + '" style="padding-top:0"><details class="nodes" id="nodes-' + h(m) + '"' + (open['nodes-' + m] ? ' open' : '') + '><summary>Show linked stations</summary><ul>';
                nodes.forEach(function (n) {
                    var c = csParts(n.callsign);
                    out += '<li>' + h(c.base + (c.mod ? '-' + c.mod : '')) + '<span>' + h(n.protocol) + '</span></li>';
                });
                out += '</ul></details></td></tr>';
            }
        });
        if (!mods.length) out += '<tr class="empty"><td colspan="' + cols + '">The reflector reports no modules.</td></tr>';
        out += '</tbody></table></div>';

        // MMDVMClient2 ... MMDVMClient9 and MMDVMClient.<name> are client connections as well
        var shown = protos.filter(function (p) { return hiddenProtos.indexOf(p.name) < 0 && !/^MMDVMClient([2-9]|\..+)$/.test(p.name); });
        if (shown.length) {
            out += '<section><div class="tbl-head"><h2>Protocols</h2><p class="hint">Ports a hotspot, repeater or gateway connects to</p></div><div class="scroll"><table class="t narrow"><thead><tr><th>Protocol</th><th class="r">Port</th><th>Used by</th></tr></thead><tbody>';
            shown.forEach(function (p) {
                out += '<tr><td>' + h(p.name) + '</td><td class="r">' + h(p.port === undefined ? '' : p.port) + '</td><td class="muted">' + h(protoTypes[p.name] || p.description || 'Other') + '</td></tr>';
            });
            out += '</tbody></table></div></section>';
        }
        $('mod-body').innerHTML = out;
    }
};

// ---------------------------------------------------------------------------
// Links: repeaters, hotspots and gateways linked to the reflector

// the suffix is a band only in D-Star (the repeater's port letter); elsewhere it
// is the module or a connection's name
function nodeBand(n, c) {
    if (c.base.slice(0, 3) === 'REF') return 'REF link';
    if (c.base.slice(0, 3) === 'XRF') return 'XRF link';
    if (['DCS', 'DExtra', 'DPlus', 'Icom G3', 'DCSClient', 'DExtraClient', 'DPlusClient'].indexOf(n.protocol) < 0) return '';
    return { A: '23cm', B: '70cm', C: '2m', D: 'Dongle', G: 'Gateway' }[c.mod] || '';
}

function nodeModules(n) {
    var ts1 = n.ts1 || '', ts2 = n.ts2 || '';
    if (ts1 && ts2 && ts1 !== ts2) return '<span class="mod on">' + h(ts1) + '</span> <span class="muted">TS1</span> <span class="mod on">' + h(ts2) + '</span> <span class="muted">TS2</span>';
    if (ts1) return '<span class="mod on">' + h(ts1) + '</span> <span class="muted">TS1</span>';
    if (ts2) return '<span class="mod on">' + h(ts2) + '</span> <span class="muted">TS2</span>';
    return '<span class="mod on">' + h(String(n.reflector_module || '').trim()) + '</span>';
}

URF.pages.repeaters = {
    refresh: true,
    mount: function (el) {
        el.innerHTML = '<div class="page"><div><h1>Links</h1><p class="lede">Repeaters, hotspots and gateways linked to <span class="refl-name"></span> right now.</p></div><div class="scroll" id="links-body"></div></div>';
    },
    update: function () {
        document.querySelectorAll('.refl-name').forEach(function (e) { e.textContent = reflectorName(); });
        var showIP = !!options().links_ip;
        var out = '<table class="t"><thead><tr><th>Station</th><th class="opt">Band</th><th>Mode</th><th>Module</th><th class="r">Last heard</th><th class="r opt">Linked for</th>' + (showIP ? '<th class="opt">IP</th>' : '') + '</tr></thead><tbody>';
        var list = S.links.slice(0, options().links_rows || 100);
        list.forEach(function (n) {
            var c = csParts(n.callsign);
            // like the last heard list: the callsign leads to QRZ, without the suffix
            out += '<tr><td class="cs"><a href="https://www.qrz.com/db/' + encodeURIComponent(c.base) + '" target="_blank" rel="noopener">' + h(c.base) + '</a>'
                + (c.mod ? '<span class="sfx">-' + h(c.mod) + '</span>' : '') + countryTag(c.base) + '</td>'
                + '<td class="opt">' + h(nodeBand(n, c)) + '</td><td>' + h(n.protocol) + '</td><td class="nw">' + nodeModules(n) + '</td>'
                + '<td class="r" title="' + h(formatDate(n.last_heard_time)) + '">' + formatAgo(n.last_heard_time) + '</td>'
                + '<td class="r opt">' + formatSpan(now() - (n.connect_time | 0)) + '</td>'
                + (showIP ? '<td class="opt ip">' + h(n.ip || '') + '</td>' : '') + '</tr>';
        });
        if (!list.length) out += '<tr class="empty"><td colspan="' + (showIP ? 7 : 6) + '">' + (S.info ? 'No repeater or hotspot is linked right now. Linked stations appear here as soon as they connect.' : 'Connecting to the reflector') + '</td></tr>';
        $('links-body').innerHTML = out + '</tbody></table>';
    }
};

// ---------------------------------------------------------------------------
// Peers: other reflectors that share modules with this one

var directory = { list: null, when: 0, loading: null };

// the XLX directory through nginx (cached there): [{name, country, comment, lastcontact, dashboardurl}], null when unreachable
function loadDirectory() {
    if (directory.list && Date.now() - directory.when < 300000) return Promise.resolve(directory.list);
    if (directory.loading) return directory.loading;
    directory.loading = fetch('./xlxapi/reflectorlist', { credentials: 'same-origin' })
        .then(function (r) { return r.ok ? r.text() : ''; })
        .then(function (text) {
            var list = parseDirectory(text);
            if (list) { directory.list = list; directory.when = Date.now(); }
            return directory.list;
        })
        .catch(function () { return directory.list; })
        .then(function (list) { directory.loading = null; return list; });
    return directory.loading;
}

// every reflector in the world writes these, and the directory hands out < > & raw
// or as entities: parsed by pattern, not as XML, which one stray & would break
function decodeText(s) {
    return new DOMParser().parseFromString('<!doctype html><body>' + s, 'text/html').body.textContent.trim();
}
function parseDirectory(text) {
    var blocks = String(text || '').match(/<reflector>[\s\S]*?<\/reflector>/g);
    if (!blocks || !blocks.length) return null;
    return blocks.map(function (b) {
        var get = function (tag) { var m = b.match(new RegExp('<' + tag + '>([\\s\\S]*?)</' + tag + '>')); return m ? decodeText(m[1]) : ''; };
        return { name: get('name'), country: get('country'), comment: get('comment'), lastcontact: parseInt(get('lastcontact'), 10) || 0, dashboardurl: get('dashboardurl') };
    });
}

function dashboardLink(name, url) {
    return /^https?:\/\//i.test(url)
        ? '<td class="cs"><a href="' + h(url) + '" target="_blank" rel="noopener noreferrer" title="Open the dashboard of ' + h(name) + '">' + h(name) + '</a></td>'
        : '<td class="cs"><b>' + h(name) + '</b></td>';
}

URF.pages.peers = {
    refresh: true,
    mount: function (el) {
        el.innerHTML = '<div class="page"><div><h1>Peers</h1><p class="lede">Other reflectors that share modules with <span class="refl-name"></span>. A call on a shared module is heard on both.</p></div><div class="scroll" id="peers-body"></div></div>';
    },
    update: function () {
        document.querySelectorAll('.refl-name').forEach(function (e) { e.textContent = reflectorName(); });
        // dashboard links from the XLX directory; without it the names stay plain text
        var urls = {};
        if (S.peers.length) {
            if (directory.list) directory.list.forEach(function (r) { urls[r.name] = r.dashboardurl; });
            else loadDirectory().then(function (l) { if (l) schedule(); });
        }
        var showIP = !!options().peers_ip;
        var out = '<table class="t"><thead><tr><th>Reflector</th><th>Mode</th><th>Shared modules</th><th class="r">Last heard</th><th class="r opt">Linked for</th>' + (showIP ? '<th class="opt">IP</th>' : '') + '</tr></thead><tbody>';
        var list = S.peers.slice(0, options().peers_rows || 100);
        list.forEach(function (p) {
            var name = csParts(p.callsign).base;
            var mods = String(p.modules || '').toUpperCase().replace(/[^A-Z]/g, '').split('').filter(Boolean).map(function (m) {
                return '<span class="mod' + (linkedNodes(m).length ? ' on' : '') + '">' + h(m) + '</span>';
            });
            out += '<tr>' + dashboardLink(name, urls[name] || '') + '<td>' + h(p.protocol) + '</td><td>' + (mods.length ? mods.join(' ') : '<span class="muted">none</span>') + '</td>'
                + '<td class="r" title="' + h(formatDate(p.last_heard_time)) + '">' + formatAgo(p.last_heard_time) + '</td>'
                + '<td class="r opt">' + formatSpan(now() - (p.connect_time | 0)) + '</td>'
                + (showIP ? '<td class="opt ip">' + h(p.ip || '') + '</td>' : '') + '</tr>';
        });
        if (!list.length) out += '<tr class="empty"><td colspan="' + (showIP ? 6 : 5) + '">' + (S.info ? 'No peer reflector is linked. Peers are set up in the interlink file of urfd.' : 'Connecting to the reflector') + '</td></tr>';
        $('peers-body').innerHTML = out + '</tbody></table>';
    }
};

// ---------------------------------------------------------------------------
// Reflector list: the XLX directory, searchable, 25 per page

URF.pages.reflectors = {
    refresh: false,
    mount: function (el) {
        el.innerHTML = '<div class="page"><div class="tbl-head"><div><h1>Reflector list</h1><p class="lede">Reflectors registered in the XLX directory. Open a dashboard to see who is on there.</p></div>'
            + '<div class="filters" id="refl-filters" hidden><label for="f-refl">Search</label><input class="field" id="f-refl" type="search" placeholder="Name, country, comment" autocomplete="off"><span class="hint" id="refl-count"></span></div></div>'
            + '<div id="refl-body"><div class="note"><p>Loading the XLX directory</p></div></div></div>';
        var page = 1, perPage = 25, rows = [];
        function render() {
            var q = $('f-refl').value.trim().toLowerCase();
            var hits = rows.filter(function (r) { return !q || r.text.indexOf(q) >= 0; });
            var pages = Math.max(1, Math.ceil(hits.length / perPage));
            if (page > pages) page = pages;
            var out = '<div class="scroll"><table class="t" id="refl"><thead><tr><th>Reflector</th><th>Country</th><th class="opt">Comment</th><th class="r">Directory</th></tr></thead><tbody>';
            hits.slice((page - 1) * perPage, page * perPage).forEach(function (r) { out += r.html; });
            if (!hits.length) out += '<tr class="empty"><td colspan="4">No reflector matches this search. Try a country or a number.</td></tr>';
            out += '</tbody></table></div><nav class="pager" id="refl-pager" aria-label="Pages of the reflector list"></nav>';
            $('refl-body').innerHTML = out;
            $('refl-count').textContent = hits.length + (hits.length === 1 ? ' reflector' : ' reflectors');
            var pager = $('refl-pager');
            if (pages < 2) return;
            function btn(label, p, cur, dis) {
                var b = document.createElement('button');
                b.type = 'button'; b.className = 'chip'; b.textContent = label; b.disabled = !!dis;
                if (cur) b.setAttribute('aria-current', 'page');
                b.addEventListener('click', function () { page = p; render(); });
                pager.appendChild(b);
            }
            btn('Previous', page - 1, false, page === 1);
            for (var p = 1; p <= pages; p++) {
                if (p === 1 || p === pages || Math.abs(p - page) <= 1) btn(String(p), p, p === page);
                else if (Math.abs(p - page) === 2) { var s = document.createElement('span'); s.className = 'hint'; s.textContent = '…'; pager.appendChild(s); }
            }
            btn('Next', page + 1, false, page === pages);
        }
        loadDirectory().then(function (list) {
            if (!$('refl-body')) return;
            if (!list) {
                $('refl-body').innerHTML = '<div class="note"><p>The XLX directory server cannot be reached right now, so this list cannot be shown. The reflector is not affected and keeps running.</p><p class="muted">The list comes back on its own once the directory answers again.</p></div>';
                return;
            }
            rows = list.map(function (r) {
                var online = r.lastcontact >= Date.now() / 1000 - 600;
                return {
                    text: (r.name + ' ' + r.country + ' ' + r.comment).toLowerCase(),
                    html: '<tr>' + dashboardLink(r.name, r.dashboardurl) + '<td>' + h(r.country) + '</td><td class="opt muted">' + h(r.comment) + '</td><td class="r">'
                        + (online ? '<span class="state-ok">online</span>' : '<span class="muted" title="' + h(formatDate(r.lastcontact)) + '">seen ' + formatAgo(r.lastcontact - S.skew) + '</span>') + '</td></tr>'
                };
            });
            $('refl-filters').hidden = false;
            $('f-refl').addEventListener('input', function () { page = 1; render(); });
            render();
        });
    },
    update: function () {}
};

// ---------------------------------------------------------------------------
// QuadNet live: D-Star routing traffic, parsed from the OpenQuad ICS data stream

URF.pages.livequadnet = {
    refresh: false,
    mount: function (el) {
        el.innerHTML = '<div class="page"><div class="tbl-head"><div><h1>QuadNet live</h1><p class="lede">D-Star routing traffic across the QuadNet servers, newest first.</p></div>'
            + '<div class="filters"><label for="quadnet-search">Search</label><input class="field" type="search" id="quadnet-search" placeholder="Callsign, name or reflector" autocomplete="off"><span class="hint" id="quadnet-status"></span></div></div>'
            + '<div class="scroll"><table class="t"><thead><tr><th>Time</th><th>Callsign</th><th class="opt">Radio</th><th>Target</th><th>Repeater</th><th class="opt">Name</th><th>Reflector</th><th class="r opt">Duration</th></tr></thead>'
            + '<tbody id="quadnet-body"><tr class="empty"><td colspan="8">Loading the QuadNet feed</td></tr></tbody></table></div></div>';
        var entries = [], timer = null, search = $('quadnet-search');
        // Preserve portable/mobile suffixes: _P -> /P, _M -> /M, _AM -> /AM etc.
        function cleanCall(s) { var m = s.match(/^(.+?)_(P|M|AM|MM|QRP)$/); return m ? m[1].replace(/_/g, '') + '/' + m[2] : s.replace(/_/g, ''); }
        // e.g. W2ECR__B -> W2ECR-B, DCS237_A -> DCS237-A
        function cleanModule(s) { return s.replace(/_/g, '').replace(/([A-Z0-9])([A-Z])$/, '$1-$2'); }
        function parseLine(line) {
            var m = line.match(/^(\d{4}-\d{2}-\d{2})\s+(\d{2}:\d{2}:\d{2})\s+([\d.]+)s:\s*(\d+)%:\s*([\d.]+)%\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(.*?)\s+(\S+)\s*$/);
            if (!m) return null;
            var cr = m[6].split('/');
            return { time: m[2], duration: m[3] + 's', call: cleanCall(cr[0] || ''), radio: (cr[1] || '').replace(/_/g, ''), target: cleanCall(m[7]),
                     repeater: cleanModule(m[8]), name: m[10].replace(/_/g, ' ').trim(), reflector: cleanModule(m[11]) };
        }
        function render() {
            var q = search.value.toLowerCase(), out = '', shown = 0;
            for (var i = entries.length - 1; i >= 0; i--) {
                var e = entries[i];
                if (q && [e.call, e.radio, e.target, e.repeater, e.name, e.reflector].join(' ').toLowerCase().indexOf(q) < 0) continue;
                out += '<tr><td class="nw">' + h(e.time) + '</td><td class="cs"><b>' + h(e.call) + '</b></td><td class="opt">' + h(e.radio) + '</td><td>' + h(e.target) + '</td>'
                    + '<td>' + h(e.repeater) + '</td><td class="opt muted">' + h(e.name) + '</td><td>' + h(e.reflector) + '</td><td class="r opt">' + h(e.duration) + '</td></tr>';
                shown++;
            }
            $('quadnet-body').innerHTML = out || '<tr class="empty"><td colspan="8">' + (q ? 'Nothing in the feed matches this search.' : 'The feed has no traffic yet.') + '</td></tr>';
            $('quadnet-status').textContent = shown + (shown === 1 ? ' entry' : ' entries');
        }
        function poll() {
            fetch('./quadnet/ics', { credentials: 'same-origin', cache: 'no-store' }).then(function (r) {
                if (!r.ok) throw new Error('feed');
                return r.text();
            }).then(function (data) {
                // the last 10 KB of the feed: the first line is cut
                var lines = data.split('\n').slice(1);
                entries = lines.map(parseLine).filter(Boolean);
                render();
            }).catch(function () {
                if ($('quadnet-status')) $('quadnet-status').textContent = 'Feed not reachable, retrying';
            }).then(function () {
                if ($('quadnet-body')) timer = setTimeout(poll, 3000);
            });
        }
        search.addEventListener('input', render);
        poll();
        this.unmount = function () { clearTimeout(timer); };
    },
    update: function () {}
};

// ---------------------------------------------------------------------------
// pages: ?show=<page>, as the old dashboard had them

var pageNames = ['users', 'modules', 'repeaters', 'peers', 'reflectors', 'livequadnet', 'admin'];
var current = null, currentName = '', refreshTimer = null, scheduled = false;

function route() {
    var show = new URLSearchParams(location.search).get('show') || 'users';
    if (show === 'modulesd') show = 'modules';   // the old module overview is part of Modules now
    if (pageNames.indexOf(show) < 0 || !URF.pages[show]) show = 'users';
    if (show === currentName) return;
    if (current && current.unmount) current.unmount();
    currentName = show;
    current = URF.pages[show];
    document.querySelectorAll('#nav a').forEach(function (a) {
        if (a.dataset.nav === show) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current');
    });
    var el = $('page');
    var fresh = el.cloneNode(false);   // no listeners of the page before
    el.parentNode.replaceChild(fresh, el);
    lastCall = null;
    current.mount(fresh);
    clearInterval(refreshTimer);
    // what has no events (last heard times of links and peers, a node changing its
    // module, transcoding): ask for the state now and then
    if (current.refresh) refreshTimer = setInterval(function () { if (S.connected && !document.hidden) request('snapshot'); }, 10000);
    render();
    window.scrollTo(0, 0);
}

function render() {
    scheduled = false;
    shellUpdate();
    strip(S.info && currentName !== 'users' && currentName !== 'admin');
    if (current) current.update();
    tick();
}

function schedule() {
    if (scheduled) return;
    scheduled = true;
    requestAnimationFrame(render);
}
URF.schedule = schedule;

document.addEventListener('click', function (e) {
    var a = e.target.closest('a[data-nav]');
    if (!a || e.ctrlKey || e.metaKey || e.shiftKey || e.button !== 0) return;
    e.preventDefault();
    if (a.getAttribute('href') !== location.pathname + location.search) history.pushState(null, '', a.getAttribute('href'));
    route();
});
window.addEventListener('popstate', route);

// theme toggle: system, light, dark
var modes = ['system', 'light', 'dark'], mode = 'system';
try { mode = localStorage.getItem('urf-theme') || 'system'; } catch (e) {}
function applyTheme() {
    if (mode === 'system') document.documentElement.removeAttribute('data-theme');
    else document.documentElement.setAttribute('data-theme', mode);
    var b = $('theme-btn');
    b.textContent = mode.charAt(0).toUpperCase() + mode.slice(1);
    b.setAttribute('aria-label', 'Theme: ' + mode);
    b.title = 'Theme: ' + mode + '. Click to switch';
}
$('theme-btn').addEventListener('click', function () {
    mode = modes[(modes.indexOf(mode) + 1) % modes.length];
    try { localStorage.setItem('urf-theme', mode); } catch (e) {}
    applyTheme();
});
applyTheme();

// the "ago" texts age by themselves; the timers tick every second
setInterval(tick, 1000);
setInterval(function () { if (!document.hidden) schedule(); }, 30000);

$('version').textContent = 'dashboard ' + DASHBOARD_VERSION;
document.addEventListener('DOMContentLoaded', function () { route(); connect(); });
if (document.readyState !== 'loading') { route(); connect(); }

})();
