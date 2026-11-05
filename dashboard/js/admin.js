/*
 *  URF dashboard: the admin page. It logs in over the dashboard's WebSocket
 *  with the [Admin] Password from urfd.ini and sends the admin socket's
 *  commands the same way; the login is a token kept for this browser tab.
 */
(function () {
'use strict';

var URF = window.URF, h = URF.h;
var q = function (id) { return document.getElementById(id); };
var TOKEN_KEY = 'urf-admin-token';
var mounted = false, refreshTimer = null;

function getToken() { try { return sessionStorage.getItem(TOKEN_KEY) || ''; } catch (e) { return ''; } }
function setToken(t) { try { if (t) sessionStorage.setItem(TOKEN_KEY, t); else sessionStorage.removeItem(TOKEN_KEY); } catch (e) {} }

// everything shown here may come from outside (hotspot config, node info,
// host names), so it goes into the page escaped
var esc = h;
// a value as a quoted JavaScript argument inside an onclick="..." attribute
function jsArg(v) { return esc(JSON.stringify(String(v === undefined || v === null ? '' : v))); }

// a command for urfd; a login that ran out shows the login again
function adminPost(cmd, args, callback) {
    URF.request(cmd, args).then(function (resp) {
        if (resp.message === 'authentication required') { setToken(''); showLogin(); }
        if (callback) callback(resp);
    });
}

// a command of a plugin: {"cmd": "plugin", "name": ..., "command": ..., args}
function pluginPost(name, command, args, callback) {
    var a = Object.assign({}, args || {});
    delete a.cmd; delete a.token; delete a.id; delete a.name; delete a.command;
    a.name = name; a.command = command;
    adminPost('plugin', a, callback);
}

// the plugins of urfd by their protocol name in lower case, from the status
var pluginNames = {};
function pluginProtoName(p) { return pluginNames[p]; }

function display(id, on) { var e = q(id); if (e) e.style.display = on ? '' : 'none'; }
function val(id) { var e = q(id); return e ? e.value : ''; }
function setVal(id, v) { var e = q(id); if (e) e.value = v; }
function tbody(id) { var e = q(id); if (e) e.innerHTML = ''; return e; }
function append(e, html) { if (e) e.insertAdjacentHTML('beforeend', html); }

// a select's options, keeping what was chosen
function fillSelect(id, items) {
    var s = q(id); if (!s) return;
    var cur = s.value;
    s.innerHTML = items.map(function (it) { return '<option value="' + esc(it[0]) + '">' + esc(it[1]) + '</option>'; }).join('');
    if (cur && items.some(function (it) { return String(it[0]) === cur; })) s.value = cur;
}

function showLogin() {
    stopRefresh();
    display('admin-content', false);
    display('admin-login', true);
}

function stopRefresh() { if (refreshTimer) { clearInterval(refreshTimer); refreshTimer = null; } }

function showAdmin() {
    display('admin-login', false);
    display('admin-content', true);
    refreshStatus();
    refreshTGList();
    refreshDcsMapList();
    refreshYsfMapList();
    refreshSvxsUserList();
    refreshMmdvmUserList();
    refreshTCStats();
    refreshCredentials();
    refreshLog();
    // Auto-refresh every 10s
    stopRefresh();
    refreshTimer = setInterval(function () {
        if (document.hidden) return;
        refreshTGList();
        refreshDcsMapList();
        refreshYsfMapList();
        refreshSvxsUserList();
        refreshMmdvmUserList();
        refreshMmdvmPeerList();
        refreshSvxsPeerList();
        refreshPluginSections();
        refreshTCStats();
        refreshCredentials();
        refreshLog();
    }, 10000);
}

// the login of this tab again, after a reload or a new connection
function resume() {
    var t = getToken();
    if (!t) { showLogin(); return; }
    URF.request('auth', { token: t }).then(function (resp) {
        if (!mounted) return;
        if (resp.status === 'ok') showAdmin();
        else {
            if (resp.message === 'authentication required') setToken('');
            showLogin();
            if (resp.message && resp.message !== 'authentication required') {
                q('login-error').textContent = resp.message;
                display('login-error', true);
            }
        }
    });
}

function doLogin() {
    var pw = val('admin-password');
    URF.request('auth', { password: pw }).then(function (resp) {
        if (resp.status === 'ok' && resp.token) {
            setToken(resp.token);
            setVal('admin-password', '');
            display('login-error', false);
            showAdmin();
        } else {
            q('login-error').textContent = resp.message || 'The password was not accepted. Use the [Admin] Password from urfd.ini.';
            display('login-error', true);
        }
    });
}

function doLogout() {
    stopRefresh();
    URF.request('logout').then(function () {
        setToken('');
        showLogin();
    });
}

function refreshTGList() {
    adminPost('tg_list', {}, function (resp) {
        var body = tbody('tg-table-body');
        if (resp.status === 'ok' && resp.mappings) {
            resp.mappings.forEach(function (m) {
                var typeLabel;
                if (m['static'] && m['primary']) typeLabel = 'static';
                else if (m['static'] && !m['primary']) typeLabel = '<span class="muted">static, secondary</span>';
                else if (m['primary']) typeLabel = '<span class="state-dyn">dynamic</span>';
                else typeLabel = '<span class="state-dyn">dynamic, secondary</span>';

                var timeoutStr = m['static'] ? '<span class="muted">none</span>' : formatSeconds(m.remaining);
                var actions = '';
                // an MMDVMClient mapping belongs to one connection: "mmdvmclient2" names it
                var proto = m.protocol;
                if (m.protocol === 'mmdvmclient' && m.client > 1) proto = 'mmdvmclient' + parseInt(m.client, 10);
                if (!m['static']) {
                    actions += '<button class="btn small danger" onclick="URFAdmin.removeTG(' + jsArg(proto) + ',' + parseInt(m.tg, 10) + ')">Remove</button> ';
                }
                if (m.protocol === 'mmdvmclient' && !m['static']) {
                    actions += '<button class="btn small" onclick="URFAdmin.doKerchunk(' + parseInt(m.tg, 10) + ',' + parseInt(m.client || 1, 10) + ')" title="Send kerchunk to the master">Kerchunk</button>';
                }
                var dirStr = m['primary'] ? 'TX/RX' : 'RX';
                append(body,
                    '<tr>' +
                    '<td>' + (m.client_name ? esc(m.client_name) + ' <span class="muted">' + esc(m.network || '') + '</span>'
                        : esc({ mmdvmclient: 'MMDVMClient', svxreflector: 'SvxReflector', svx: 'SVX' }[m.protocol] || pluginProtoName(m.protocol) || m.protocol)) + '</td>' +
                    '<td>' + esc(m.tg) + '</td>' +
                    '<td>' + esc(m.module) + '</td>' +
                    '<td>' + esc(m.ts || '-') + '</td>' +
                    '<td>' + typeLabel + '</td>' +
                    '<td>' + dirStr + '</td>' +
                    '<td>' + timeoutStr + '</td>' +
                    '<td>' + actions + '</td>' +
                    '</tr>'
                );
            });
        }
        if (resp.mappings && resp.mappings.length === 0) {
            append(body, '<tr class="empty"><td colspan="8">No TG mappings configured</td></tr>');
        }
    });
}

function addTG() {
    var data = {
        protocol: val('add-protocol'),
        tg: parseInt(val('add-tg'), 10),
        module: val('add-module'),
        ttl: parseInt(val('add-ttl'), 10) || 900
    };
    var ts = parseInt(val('add-ts'), 10);
    if (ts) data.ts = ts;
    adminPost('tg_add', data, function (resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshTGList();
    });
}

function removeTG(protocol, tg) {
    adminPost('tg_remove', { protocol: protocol, tg: tg }, function (resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshTGList();
    });
}

function doKerchunk(tg, client) {
    adminPost('kerchunk', { tg: tg, client: client || 1 }, function (resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshTGList();
    });
}

function doReconnect(protocol, client) {
    var data = { protocol: protocol };
    if (client) data.client = client;
    adminPost('reconnect', data, function (resp) { showAlert(resp); });
}

function refreshDcsMapList() {
    // aggregate all D-Star client mappings (DCS, DExtra, DPlus)
    var allMappings = [], pending = 3;
    function renderAll() {
        if (--pending > 0) return;
        var body = tbody('dcs-map-table-body');
        allMappings.forEach(function (m) {
            var connLabel = m.connected ? '<span class="state-ok">connected</span>' : '<span class="state-bad">disconnected</span>';
            append(body,
                '<tr>' +
                '<td>' + esc(m.proto) + '</td>' +
                '<td>' + esc(m.host) + ':' + esc(m.port) + '</td>' +
                '<td>' + esc(m.remote_module) + '</td>' +
                '<td>' + esc(m.local_module) + '</td>' +
                '<td>' + connLabel + '</td>' +
                '<td><button class="btn small danger" onclick="URFAdmin.removeDstarMap(' + jsArg(m.proto_key) + ',' + jsArg(m.local_module) + ')">Remove</button></td>' +
                '</tr>'
            );
        });
        if (allMappings.length === 0) append(body, '<tr class="empty"><td colspan="6">No D-Star client mappings configured</td></tr>');
    }
    [
        { cmd: 'dcs_map_list', proto: 'DCS', key: 'dcs' },
        { cmd: 'dextra_map_list', proto: 'DExtra', key: 'dextra' },
        { cmd: 'dplus_map_list', proto: 'DPlus', key: 'dplus' }
    ].forEach(function (p) {
        adminPost(p.cmd, {}, function (resp) {
            if (resp.status === 'ok' && resp.mappings) {
                resp.mappings.forEach(function (m) { m.proto = p.proto; m.proto_key = p.key; allMappings.push(m); });
            }
            renderAll();
        });
    });
}

var portDefaults = { dcs: 30051, dextra: 30001, dplus: 20001 };

function addDcsMap() {
    var proto = val('dcs-proto');
    var data = {
        host: val('dcs-host'),
        port: parseInt(val('dcs-port'), 10) || portDefaults[proto] || 30051,
        remote_module: val('dcs-remote-mod'),
        local_module: val('dcs-local-mod')
    };
    adminPost(proto + '_map_add', data, function (resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshDcsMapList();
    });
}

function removeDstarMap(protoKey, localMod) {
    if (['dcs', 'dextra', 'dplus'].indexOf(protoKey) < 0) return;
    adminPost(protoKey + '_map_remove', { local_module: localMod }, function (resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshDcsMapList();
    });
}

function refreshYsfMapList() {
    adminPost('ysf_map_list', {}, function (resp) {
        var body = tbody('ysf-map-table-body');
        if (resp.status === 'ok' && resp.mappings) {
            resp.mappings.forEach(function (m) {
                var connLabel = m.connected ? '<span class="state-ok">connected</span>' : '<span class="state-bad">disconnected</span>';
                append(body,
                    '<tr>' +
                    '<td>' + esc(m.host) + ':' + esc(m.port) + '</td>' +
                    '<td>' + esc(m.local_module) + '</td>' +
                    '<td>' + esc(m.dgid > 0 ? m.dgid : '-') + '</td>' +
                    '<td>' + connLabel + '</td>' +
                    '<td><button class="btn small danger" onclick="URFAdmin.removeYsfMap(' + jsArg(m.local_module) + ')">Remove</button></td>' +
                    '</tr>'
                );
            });
        }
        if (!resp.mappings || resp.mappings.length === 0) append(body, '<tr class="empty"><td colspan="5">No YSF mappings configured</td></tr>');
    });
}

function addYsfMap() {
    var data = {
        host: val('ysf-host'),
        port: parseInt(val('ysf-port'), 10) || 42000,
        local_module: val('ysf-local-mod'),
        dgid: parseInt(val('ysf-dgid'), 10) || 0
    };
    adminPost('ysf_map_add', data, function (resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshYsfMapList();
    });
}

function removeYsfMap(localMod) {
    adminPost('ysf_map_remove', { local_module: localMod }, function (resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshYsfMapList();
    });
}

function refreshSvxsUserList() {
    adminPost('svxs_user_list', {}, function (resp) {
        var body = tbody('svxs-user-table-body');
        if (resp.status === 'ok' && resp.users) {
            resp.users.forEach(function (u) {
                append(body,
                    '<tr>' +
                    '<td>' + esc(u) + dbTag(resp.sources && resp.sources[u]) + '</td>' +
                    '<td><button class="btn small danger" onclick="URFAdmin.removeSvxsUser(' + jsArg(u) + ')">Remove</button></td>' +
                    '</tr>'
                );
            });
        }
        if (!resp.users || resp.users.length === 0) append(body, '<tr class="empty"><td colspan="2">No SVX users configured</td></tr>');
    });
}

function addSvxsUser() {
    adminPost('svxs_user_add', { callsign: val('svxs-callsign'), password: val('svxs-password') }, function (resp) {
        showAlert(resp);
        if (resp.status === 'ok') {
            setVal('svxs-callsign', '');
            setVal('svxs-password', '');
            refreshSvxsUserList();
        }
    });
}

function removeSvxsUser(callsign) {
    adminPost('svxs_user_remove', { callsign: callsign }, function (resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshSvxsUserList();
    });
}

function refreshMmdvmUserList() {
    adminPost('mmdvm_user_list', {}, function (resp) {
        var body = tbody('mmdvm-user-table-body');
        if (resp.status === 'ok' && resp.users) {
            resp.users.forEach(function (u) {
                append(body,
                    '<tr>' +
                    '<td>' + esc(u.dmrid) + dbTag(u.source) + '</td>' +
                    '<td>' + esc(u.callsign) + '</td>' +
                    '<td><button class="btn small danger" onclick="URFAdmin.removeMmdvmUser(' + parseInt(u.dmrid, 10) + ')">Remove</button></td>' +
                    '</tr>'
                );
            });
        }
        if (!resp.users || resp.users.length === 0) append(body, '<tr class="empty"><td colspan="3">No MMDVM users configured</td></tr>');
    });
}

function addMmdvmUser() {
    var id = val('mmdvm-user-id').trim();
    var pw = val('mmdvm-user-pw').trim();
    if (!id) return;
    var data = { password: pw };
    if (/^\d+$/.test(id)) data.dmrid = parseInt(id, 10);
    else data.callsign = id;
    adminPost('mmdvm_user_add', data, function (resp) {
        showAlert(resp, 'mmdvm-user-alerts');
        if (resp.status === 'ok') {
            setVal('mmdvm-user-id', '');
            setVal('mmdvm-user-pw', '');
            refreshMmdvmUserList();
        }
    });
}

function removeMmdvmUser(dmrid) {
    adminPost('mmdvm_user_remove', { dmrid: dmrid }, function (resp) {
        showAlert(resp, 'mmdvm-user-alerts');
        if (resp.status === 'ok') refreshMmdvmUserList();
    });
}

function refreshMmdvmPeerList() {
    adminPost('mmdvm_peer_list', {}, function (resp) {
        var body = tbody('mmdvm-peers-table-body');
        if (resp.status === 'ok' && resp.peers && resp.peers.length > 0) {
            resp.peers.forEach(function (p) {
                var info = p.peer_info || {};
                var rx = info.rxFreq ? (info.rxFreq / 1e6).toFixed(4) : '';
                var tx = info.txFreq ? (info.txFreq / 1e6).toFixed(4) : '';
                var freq = (rx && tx) ? rx + ' / ' + tx : (rx || tx || '');
                var since = p.connected_since ? new Date(p.connected_since * 1000).toLocaleString() : '';
                append(body,
                    '<tr>' +
                    '<td>' + esc((p.callsign || '').trim()) + '</td>' +
                    '<td>' + esc(info.dmrid || '') + '</td>' +
                    '<td>' + esc(freq) + '</td>' +
                    '<td>' + esc(info.location || '') + '</td>' +
                    '<td>' + esc([info.packageId, info.softwareId].filter(Boolean).join(' / ')) + '</td>' +
                    '<td>' + esc(p.module && p.module !== ' ' ? p.module : '') + '</td>' +
                    '<td>' + esc(since) + '</td>' +
                    '</tr>'
                );
            });
        } else {
            append(body, '<tr class="empty"><td colspan="7">No MMDVM nodes connected</td></tr>');
        }
    });
}

function refreshSvxsPeerList() {
    adminPost('svxs_peer_list', {}, function (resp) {
        var body = tbody('svxs-peers-table-body');
        if (resp.status === 'ok' && resp.peers) {
            resp.peers.forEach(function (p) {
                var tgs = (p.subscribed_tgs && p.subscribed_tgs.length) ? p.subscribed_tgs.join(', ') : '-';
                if (p.monitored_tgs && p.monitored_tgs.length) tgs += ' (monitor ' + p.monitored_tgs.join(', ') + ')';
                var since = p.connected_since ? new Date(p.connected_since * 1000).toLocaleString() : '';
                append(body,
                    '<tr>' +
                    '<td>' + esc((p.callsign || '').trim()) + '</td>' +
                    '<td>' + esc(tgs) + '</td>' +
                    '<td>' + esc((p.node_info && p.node_info.software) ? p.node_info.software : '') + '</td>' +
                    '<td>' + esc(p.udp_discovered ? 'Yes' : 'No') + '</td>' +
                    '<td>' + esc(since) + '</td>' +
                    '</tr>'
                );
            });
        }
        if (!resp.peers || resp.peers.length === 0) append(body, '<tr class="empty"><td colspan="5">No SVX nodes connected</td></tr>');
    });
}

function doToggleBlock(action, a, b) {
    if (!a) a = val('block-proto-a');
    if (!b) b = val('block-proto-b');
    if (!a || !b || a === b) { showAlert({ status: 'error', message: 'Pick two different protocols to block or unblock.' }); return; }
    if (action !== 'block' && action !== 'unblock') return;
    adminPost(action, { a: a, b: b }, function (resp) {
        showAlert(resp);
        refreshStatus();
    });
}

function doBlockReset() {
    adminPost('block_reset', {}, function (resp) {
        showAlert(resp);
        refreshStatus();
    });
}

function doClearUsers() {
    adminPost('clear_users', {}, function (resp) { showAlert(resp); });
}

function refreshLog() {
    adminPost('log', { lines: 50 }, function (resp) {
        if (resp.status === 'ok' && resp.lines) {
            var pre = q('log-output');
            pre.textContent = resp.lines.join('\n');
            pre.scrollTop = pre.scrollHeight;
        }
    });
}

function refreshStatus() {
    adminPost('status', {}, function (resp) {
        if (resp.status !== 'ok') return;
        q('status-reflector').textContent = resp.reflector || '?';
        var v = URF.state.info && URF.state.info.version;
        q('status-version').textContent = v || resp.version || '?';
        // Populate module dropdowns from available modules
        if (resp.modules) {
            // a string of letters ("ADMSZ"), or a list of them
            var mods = Array.prototype.slice.call(resp.modules).map(function (m) { return [m, m]; });
            fillSelect('add-module', mods);
            fillSelect('dcs-local-mod', mods);
            fillSelect('ysf-local-mod', mods);
        }
        // Show/hide protocol options based on what's active
        var protos = [];
        // one entry per MMDVMClient connection when there are several
        var clis = resp.mmdvm_clients || [];
        if (clis.length > 1) {
            clis.forEach(function (c) { protos.push(['mmdvmclient' + (c.client > 1 ? parseInt(c.client, 10) : ''), c.name + ' (' + c.network + ')']); });
        } else if (resp.mmdvm_active) protos.push(['mmdvmclient', 'MMDVMClient']);
        if (resp.svx_active) protos.push(['svxreflector', 'SvxReflector']);
        if (resp.svxs_active) protos.push(['svx', 'SVX']);
        if (!resp.mmdvm_active && !resp.svx_active && !resp.svxs_active) protos.push(['', 'No TG protocol active']);
        fillSelect('add-protocol', protos);
        syncTimeslot();
        // Reconnect buttons
        var cliBtns = q('mmdvm-reconnect-list');
        cliBtns.innerHTML = '';
        if (clis.length > 1) {
            display('btn-reconnect-mmdvm', false);
            clis.forEach(function (c) {
                append(cliBtns, '<button class="btn" onclick="URFAdmin.doReconnect(\'mmdvmclient\',' + parseInt(c.client, 10) + ')" title="' + esc(c.network) + (c.connected ? '' : ', not connected') + '">Reconnect ' + esc(c.name) + '</button> ');
            });
        } else display('btn-reconnect-mmdvm', !!resp.mmdvm_active);
        display('btn-reconnect-svx', !!resp.svx_active);
        display('btn-reconnect-dcsclient', !!resp.dcsclient_active);
        display('btn-reconnect-dextraclient', !!resp.dextraclient_active);
        display('btn-reconnect-dplusclient', !!resp.dplusclient_active);
        display('dcs-mapping-section', resp.dcsclient_active || resp.dextraclient_active || resp.dplusclient_active);
        display('btn-reconnect-ysfclient', !!resp.ysfclient_active);
        display('ysf-mapping-section', !!resp.ysfclient_active);
        display('svxs-user-section', !!resp.svxs_active);
        display('svxs-peers-section', !!resp.svxs_active);
        if (resp.svxs_active) refreshSvxsPeerList();
        pluginNames = {};
        (resp.plugins || []).forEach(function (p) { pluginNames[String(p.name).toLowerCase()] = p.name; });
        renderPluginSections(resp.plugins);
        display('mmdvm-user-section', !!resp.mmdvm_server_active);
        display('mmdvm-peers-section', !!resp.mmdvm_server_active);
        if (resp.mmdvm_server_active) { refreshMmdvmUserList(); refreshMmdvmPeerList(); }

        syncIndex();

        // Block rules — collect bidirectional pairs
        var blockedPairs = {};  // "A|B" (sorted) -> true
        if (resp.blocks && resp.blocks.length > 0) {
            resp.blocks.forEach(function (b) {
                if (b.from === b.to) return;
                blockedPairs[[b.from, b.to].sort().join('|')] = true;
            });
            var html = '';
            Object.keys(blockedPairs).forEach(function (k) {
                var parts = k.split('|');
                html += '<button type="button" class="btn small danger" onclick="URFAdmin.doToggleBlock(\'unblock\',' + jsArg(parts[0]) + ',' + jsArg(parts[1]) + ')" title="Unblock">'
                    + esc(parts[0]) + ' &#8596; ' + esc(parts[1]) + ' &#10005;</button>';
            });
            q('block-rules').innerHTML = html;
        } else {
            q('block-rules').innerHTML = '<span class="muted">No protocol pair is blocked.</span>';
        }

        // Populate protocol dropdowns from active protocols
        if (resp.active_protocols) {
            var list = resp.active_protocols.slice().sort().map(function (p) { return [p, p]; });
            fillSelect('block-proto-a', list);
            fillSelect('block-proto-b', list);
        }
    });
}

function refreshTCStats() {
    adminPost('tc_stats', {}, function (resp) {
        var body = tbody('tc-table-body');
        if (resp.status === 'ok' && resp.modules) {
            var codecNames = { 0: '-', 1: 'D-Star', 2: 'DMR', 3: 'Codec2/1600', 4: 'Codec2/3200', 5: 'P25', 6: 'USRP/PCM', 7: 'SVX/OPUS' };
            resp.modules.forEach(function (m) {
                var connLabel = m.connected ? '<span class="state-ok">connected</span>' : '<span class="state-bad">disconnected</span>';
                var streamLabel = m.streaming ? '<span class="txtag">TX</span> ' + esc(m.user || '?') : '<span class="muted">idle</span>';
                var codec = codecNames[m.codec_in] || '-';
                var rtt = '-';
                var packets = m.total_packets || 0;
                var mismatches = m.mismatch_count || 0;
                if (!m.connected) {
                    codec = '-'; packets = '-'; mismatches = '-';
                } else if (m.rt_count > 0 && m.rt_min_ms >= 0) {
                    rtt = m.rt_min_ms + ' / ' + m.rt_avg_ms + ' / ' + m.rt_max_ms + ' ms';
                }
                append(body,
                    '<tr>' +
                    '<td>' + esc(m.module) + '</td>' +
                    '<td>' + connLabel + '</td>' +
                    '<td>' + streamLabel + '</td>' +
                    '<td>' + esc(codec) + '</td>' +
                    '<td>' + esc(packets) + '</td>' +
                    '<td>' + esc(mismatches) + '</td>' +
                    '<td>' + esc(rtt) + '</td>' +
                    '</tr>'
                );
            });
        }
        if (!resp.modules || resp.modules.length === 0) append(body, '<tr class="empty"><td colspan="7">No transcoded modules</td></tr>');
    });
}

// the sections of urfd's plugins, drawn from the description each plugin gives
// in the status (plugins[].ui, see docs/PLUGINS.md); nothing here knows a plugin
var pluginUi = {};      // section element id -> {plugin, section}
var pluginUiKey = '';   // what is drawn, so it is drawn again only after a change

function pluginSlug(s) { return String(s).toLowerCase().replace(/[^a-z0-9]+/g, '-'); }

function renderPluginSections(plugins) {
    var active = (plugins || []).filter(function (p) { return p.active && Array.isArray(p.ui) && p.ui.length; });
    var key = JSON.stringify(active.map(function (p) { return [p.name, p.ui]; }));
    if (key === pluginUiKey) return;
    pluginUiKey = key;
    pluginUi = {};
    var nav = q('plugin-nav'), box = q('plugin-sections');
    nav.innerHTML = ''; box.innerHTML = '';
    active.forEach(function (p) {
        p.ui.forEach(function (sec) {
            var id = 'plugin-' + pluginSlug(p.name) + '-' + pluginSlug(sec.id || '');
            var title = sec.title || p.name;
            var cols = Array.isArray(sec.columns) ? sec.columns : [];
            var ncol = cols.length + (sec.remove ? 1 : 0);
            pluginUi[id] = { plugin: p.name, section: sec };
            append(nav, '<a href="#' + id + '">' + esc(title) + '</a>');
            var html = '<section class="admin-sec" id="' + id + '">' +
                '<header><h2>' + esc(title) + '</h2>' + (sec.list ? '<button class="btn small quiet" data-plugin-refresh="' + id + '">Refresh</button>' : '') + '</header>';
            if (sec.form && Array.isArray(sec.form.fields)) {
                html += '<form class="form-row" data-plugin-form="' + id + '">';
                sec.form.fields.forEach(function (f) {
                    html += '<label>' + esc(f.label || f.name) +
                        ' <input type="' + (f.type === 'password' ? 'password' : 'text') + '" class="field w-' + (f.width === 'm' ? 'm' : 's') + '" name="' + esc(f.name) + '"' +
                        (f.placeholder ? ' placeholder="' + esc(f.placeholder) + '"' : '') + (f.required ? ' required' : '') + '></label>';
                });
                html += '<button type="submit" class="btn primary">' + esc(sec.form.submit || 'Add') + '</button></form>';
            }
            if (sec.list) {
                html += '<div class="scroll"><table class="t' + (ncol <= 3 ? ' narrow' : '') + '"><thead><tr>' +
                    cols.map(function (c) { return '<th>' + esc(c.label || c.key) + '</th>'; }).join('') + (sec.remove ? '<th></th>' : '') +
                    '</tr></thead><tbody><tr class="empty"><td colspan="' + ncol + '">Loading</td></tr></tbody></table></div>';
            }
            append(box, html + '</section>');
        });
    });
    refreshPluginSections();
    syncIndex();
}

function pluginCell(c, row) {
    var v = row[c.key];
    if (c.format === 'list') v = (Array.isArray(v) && v.length) ? v.join(', ') : '';
    else if (c.format === 'time') v = v ? new Date(v * 1000).toLocaleString() : '';
    else if (c.format === 'bool') v = v ? 'yes' : 'no';
    var html = (v === undefined || v === null || v === '') ? '-' : esc(v);
    if (c.source) html += dbTag(row[c.source]);
    return '<td>' + html + '</td>';
}

function refreshPluginSection(id) {
    var ui = pluginUi[id];
    if (!ui || !ui.section.list) return;
    var sec = ui.section, cols = Array.isArray(sec.columns) ? sec.columns : [];
    pluginPost(ui.plugin, sec.list.command, {}, function (resp) {
        var sect = q(id); if (!sect) return;
        var body = sect.querySelector('tbody');
        body.innerHTML = '';
        var rows = (resp.status === 'ok' && Array.isArray(resp[sec.list.rows])) ? resp[sec.list.rows] : [];
        rows.forEach(function (row) {
            var tr = '<tr>' + cols.map(function (c) { return pluginCell(c, row); }).join('');
            if (sec.remove)
                tr += '<td><button class="btn small danger" data-plugin-remove="' + id + '" data-value="' + esc(row[sec.remove.key]) + '">Remove</button></td>';
            append(body, tr + '</tr>');
        });
        if (!rows.length)
            append(body, '<tr class="empty"><td colspan="' + (cols.length + (sec.remove ? 1 : 0)) + '">' + esc(sec.list.empty || 'Nothing to show') + '</td></tr>');
    });
}

function refreshPluginSections() { Object.keys(pluginUi).forEach(refreshPluginSection); }

function dbTag(source) {
    // users from the credential database get a tag, those from urfd.ini none
    return source === 'db' ? ' <span class="muted">(database)</span>' : '';
}

function refreshCredentials() {
    adminPost('credentials_status', {}, function (resp) {
        var body = tbody('cred-table-body');
        if (resp.status !== 'ok') {
            append(body, '<tr class="empty"><td colspan="2">' + esc(resp.message || 'No answer') + '</td></tr>');
            return;
        }
        if (resp.backend === 'ini') {
            append(body, '<tr class="empty"><td colspan="2">No database: the logins come from urfd.ini only ([Credentials] Backend = ini)</td></tr>');
            display('btn-cred-flush', false);
            return;
        }
        [
            ['Backend', esc(resp.backend)],
            ['Database', resp.connected ? '<span class="state-ok">connected</span>' : '<span class="state-bad">not connected</span>'],
            ['Logins in the cache', esc(resp.cached)],
            ['Cache hits / misses', esc(resp.hits) + ' / ' + esc(resp.misses)],
            ['Database errors', esc(resp.errors)]
        ].forEach(function (r) { append(body, '<tr><th scope="row">' + r[0] + '</th><td>' + r[1] + '</td></tr>'); });
        display('btn-cred-flush', true);
    });
}

function doCredentialsFlush() {
    adminPost('credentials_flush', {}, function (resp) {
        showAlert(resp, 'cred-alerts');
        refreshCredentials();
    });
}

// the index lists only the sections the running protocols use
function syncIndex() {
    document.querySelectorAll('.admin-index a[href^="#"]').forEach(function (a) {
        var sec = document.getElementById(a.getAttribute('href').slice(1));
        a.style.display = (sec && sec.style.display !== 'none') ? '' : 'none';
    });
}

function showAlert(resp, target) {
    var box = q(target || 'admin-alerts');
    if (!box) return;
    var cls = resp.status === 'ok' ? 'ok' : 'bad';
    box.innerHTML = '<div class="alert ' + cls + '" role="status"><span>' + esc(resp.message || resp.status) + '</span>' +
        '<button type="button" class="close" aria-label="Dismiss" onclick="this.parentNode.remove()">&times;</button></div>';
}

function formatSeconds(s) {
    if (s < 0) return '<span class="muted">none</span>';
    return Math.floor(s / 60) + ' min ' + (s % 60) + ' s';
}

// the timeslot belongs to MMDVMClient talkgroups only
function syncTimeslot() { display('ts-group', String(val('add-protocol')).indexOf('mmdvmclient') === 0); }

var MARKUP = '' +
'<div id="admin-login" class="login" style="display:none;">' +
'    <div><h1>Admin</h1><p class="lede">Sign in with the [Admin] Password from urfd.ini. The dashboard keeps no copy of it.</p></div>' +
'    <div id="login-error" class="alert bad" style="display:none;"></div>' +
'    <form class="form-row" id="login-form">' +
'        <label style="flex:1 1 12rem">Password <input type="password" class="field" id="admin-password" autocomplete="current-password" required></label>' +
'        <button type="submit" class="btn primary">Sign in</button>' +
'    </form>' +
'</div>' +
'<div id="admin-content" style="display:none;">' +
'    <div class="tbl-head" style="margin-bottom:1.5rem">' +
'        <div><h1>Admin</h1><p class="lede"><span id="status-reflector">-</span>, urfd <span id="status-version">-</span>. Lists refresh every 10 seconds.</p></div>' +
'        <button class="btn quiet" onclick="URFAdmin.doLogout()">Sign out</button>' +
'    </div>' +
'    <div id="admin-alerts" style="margin-bottom:1rem"></div>' +
'    <div class="admin">' +
'        <nav class="admin-index" aria-label="Admin sections">' +
'            <a href="#sec-tg">Talkgroups</a>' +
'            <a href="#dcs-mapping-section">D-Star links</a>' +
'            <a href="#ysf-mapping-section">YSF links</a>' +
'            <a href="#svxs-user-section">SVX users</a>' +
'            <a href="#svxs-peers-section">SVX nodes</a>' +
'            <span id="plugin-nav" style="display:contents"></span>' +
'            <a href="#mmdvm-user-section">MMDVM users</a>' +
'            <a href="#mmdvm-peers-section">MMDVM nodes</a>' +
'            <a href="#sec-cred">Credentials</a>' +
'            <a href="#sec-tc">Transcoder</a>' +
'            <a href="#sec-ctl">Protocol controls</a>' +
'            <a href="#sec-log">Log</a>' +
'        </nav>' +
'        <div class="admin-body">' +
'            <section class="admin-sec" id="sec-tg">' +
'                <header><h2>Talkgroups</h2><button class="btn small quiet" onclick="URFAdmin.refreshTGList()">Refresh</button></header>' +
'                <form class="form-row" data-submit="addTG">' +
'                    <label>Protocol <select class="field w-m" id="add-protocol"><option value="mmdvmclient">MMDVMClient</option><option value="svxreflector">SvxReflector</option></select></label>' +
'                    <label>Talkgroup <input type="number" class="field w-s" id="add-tg" placeholder="26207" required></label>' +
'                    <label>Module <select class="field w-xs" id="add-module" required><option value="">-</option></select></label>' +
'                    <label id="ts-group">Timeslot <select class="field w-xs" id="add-ts"><option value="2">TS2</option><option value="1">TS1</option></select></label>' +
'                    <label>Timeout in s <input type="number" class="field w-s" id="add-ttl" value="900"></label>' +
'                    <button type="submit" class="btn primary">Add talkgroup</button>' +
'                </form>' +
'                <div class="scroll"><table class="t"><thead><tr><th>Protocol</th><th>Talkgroup</th><th>Module</th><th>Timeslot</th><th>Type</th><th>Direction</th><th>Ends in</th><th></th></tr></thead>' +
'                    <tbody id="tg-table-body"><tr class="empty"><td colspan="8">Loading</td></tr></tbody></table></div>' +
'            </section>' +
'            <section class="admin-sec" id="dcs-mapping-section" style="display:none;">' +
'                <header><h2>D-Star links</h2><button class="btn small quiet" onclick="URFAdmin.refreshDcsMapList()">Refresh</button></header>' +
'                <form class="form-row" data-submit="addDcsMap">' +
'                    <label>Protocol <select class="field w-s" id="dcs-proto"><option value="dcs">DCS</option><option value="dextra">DExtra</option><option value="dplus">DPlus</option></select></label>' +
'                    <label>Host <input type="text" class="field w-l" id="dcs-host" placeholder="dcs001.xreflector.net" required></label>' +
'                    <label>Port <input type="number" class="field w-s" id="dcs-port" value="30051"></label>' +
'                    <label>Remote module <select class="field w-xs" id="dcs-remote-mod"></select></label>' +
'                    <label>Local module <select class="field w-xs" id="dcs-local-mod"><option value="">-</option></select></label>' +
'                    <button type="submit" class="btn primary">Connect</button>' +
'                </form>' +
'                <div class="scroll"><table class="t"><thead><tr><th>Protocol</th><th>Reflector</th><th>Remote module</th><th>Local module</th><th>Status</th><th></th></tr></thead>' +
'                    <tbody id="dcs-map-table-body"><tr class="empty"><td colspan="6">Loading</td></tr></tbody></table></div>' +
'            </section>' +
'            <section class="admin-sec" id="ysf-mapping-section" style="display:none;">' +
'                <header><h2>YSF links</h2><button class="btn small quiet" onclick="URFAdmin.refreshYsfMapList()">Refresh</button></header>' +
'                <form class="form-row" data-submit="addYsfMap">' +
'                    <label>Host <input type="text" class="field w-l" id="ysf-host" placeholder="ysf.reflector.net" required></label>' +
'                    <label>Port <input type="number" class="field w-s" id="ysf-port" value="42000"></label>' +
'                    <label>Local module <select class="field w-xs" id="ysf-local-mod"><option value="">-</option></select></label>' +
'                    <label>DG-ID <input type="number" class="field w-xs" id="ysf-dgid" value="0" min="0" max="99"></label>' +
'                    <button type="submit" class="btn primary">Connect</button>' +
'                </form>' +
'                <div class="scroll"><table class="t"><thead><tr><th>Reflector</th><th>Local module</th><th>DG-ID</th><th>Status</th><th></th></tr></thead>' +
'                    <tbody id="ysf-map-table-body"><tr class="empty"><td colspan="5">Loading</td></tr></tbody></table></div>' +
'            </section>' +
'            <section class="admin-sec" id="svxs-user-section" style="display:none;">' +
'                <header><h2>SVX users</h2><button class="btn small quiet" onclick="URFAdmin.refreshSvxsUserList()">Refresh</button></header>' +
'                <form class="form-row" data-submit="addSvxsUser">' +
'                    <label>Callsign <input type="text" class="field w-s" id="svxs-callsign" placeholder="N0CALL" required></label>' +
'                    <label>Password <input type="text" class="field w-m" id="svxs-password" required></label>' +
'                    <button type="submit" class="btn primary">Add user</button>' +
'                </form>' +
'                <div class="scroll"><table class="t narrow"><thead><tr><th>Callsign</th><th></th></tr></thead>' +
'                    <tbody id="svxs-user-table-body"><tr class="empty"><td colspan="2">Loading</td></tr></tbody></table></div>' +
'            </section>' +
'            <section class="admin-sec" id="svxs-peers-section" style="display:none;">' +
'                <header><h2>SVX nodes</h2><button class="btn small quiet" onclick="URFAdmin.refreshSvxsPeerList()">Refresh</button></header>' +
'                <div class="scroll"><table class="t"><thead><tr><th>Callsign</th><th>Talkgroups</th><th>Software</th><th>UDP</th><th>Connected since</th></tr></thead>' +
'                    <tbody id="svxs-peers-table-body"><tr class="empty"><td colspan="5">Loading</td></tr></tbody></table></div>' +
'            </section>' +
'            <div id="plugin-sections"></div>' +
'            <section class="admin-sec" id="mmdvm-user-section" style="display:none;">' +
'                <header><h2>MMDVM users</h2><button class="btn small quiet" onclick="URFAdmin.refreshMmdvmUserList()">Refresh</button></header>' +
'                <div id="mmdvm-user-alerts"></div>' +
'                <form class="form-row" data-submit="addMmdvmUser">' +
'                    <label>Callsign or DMR ID <input type="text" class="field w-m" id="mmdvm-user-id" placeholder="N0CALL or 2634000" required></label>' +
'                    <label>Password, optional <input type="text" class="field w-m" id="mmdvm-user-pw"></label>' +
'                    <button type="submit" class="btn primary">Add user</button>' +
'                </form>' +
'                <div class="scroll"><table class="t narrow"><thead><tr><th>DMR ID</th><th>Callsign</th><th></th></tr></thead>' +
'                    <tbody id="mmdvm-user-table-body"><tr class="empty"><td colspan="3">Loading</td></tr></tbody></table></div>' +
'            </section>' +
'            <section class="admin-sec" id="mmdvm-peers-section" style="display:none;">' +
'                <header><h2>MMDVM nodes</h2><button class="btn small quiet" onclick="URFAdmin.refreshMmdvmPeerList()">Refresh</button></header>' +
'                <div class="scroll"><table class="t"><thead><tr><th>Callsign</th><th>DMR ID</th><th>RX / TX in MHz</th><th>Location</th><th>Modem and software</th><th>Module</th><th>Connected since</th></tr></thead>' +
'                    <tbody id="mmdvm-peers-table-body"><tr class="empty"><td colspan="7">Loading</td></tr></tbody></table></div>' +
'            </section>' +
'            <section class="admin-sec" id="sec-cred">' +
'                <header><h2>Credential database</h2><button class="btn small quiet" onclick="URFAdmin.refreshCredentials()">Refresh</button></header>' +
'                <div id="cred-alerts"></div>' +
'                <p class="hint">Logins of MMDVM and SVX users (and those of plugins) who are not in urfd.ini. After a change made directly in the database, flush the cache so it counts at the next login; changes made on this page count at once.</p>' +
'                <div class="scroll"><table class="t narrow"><tbody id="cred-table-body"><tr class="empty"><td colspan="2">Loading</td></tr></tbody></table></div>' +
'                <div style="margin-top:0.75rem"><button class="btn" id="btn-cred-flush" onclick="URFAdmin.doCredentialsFlush()" style="display:none;">Flush cache</button></div>' +
'            </section>' +
'            <section class="admin-sec" id="sec-tc">' +
'                <header><h2>Transcoder</h2><button class="btn small quiet" onclick="URFAdmin.refreshTCStats()">Refresh</button></header>' +
'                <div class="scroll"><table class="t"><thead><tr><th>Module</th><th>Connection</th><th>Stream</th><th>Codec</th><th>Packets</th><th>Mismatches</th><th>Round trip min / avg / max</th></tr></thead>' +
'                    <tbody id="tc-table-body"><tr class="empty"><td colspan="7">Loading</td></tr></tbody></table></div>' +
'            </section>' +
'            <section class="admin-sec" id="sec-ctl">' +
'                <header><h2>Protocol controls</h2></header>' +
'                <div class="form-row" style="background:none;border:0;padding:0">' +
'                    <button class="btn" id="btn-reconnect-mmdvm" onclick="URFAdmin.doReconnect(\'mmdvmclient\')">Reconnect MMDVMClient</button>' +
'                    <span id="mmdvm-reconnect-list"></span>' +
'                    <button class="btn" id="btn-reconnect-svx" onclick="URFAdmin.doReconnect(\'svxreflector\')">Reconnect SvxReflector</button>' +
'                    <button class="btn" id="btn-reconnect-dcsclient" onclick="URFAdmin.doReconnect(\'dcsclient\')" style="display:none;">Reconnect DCS client</button>' +
'                    <button class="btn" id="btn-reconnect-dextraclient" onclick="URFAdmin.doReconnect(\'dextraclient\')" style="display:none;">Reconnect DExtra client</button>' +
'                    <button class="btn" id="btn-reconnect-dplusclient" onclick="URFAdmin.doReconnect(\'dplusclient\')" style="display:none;">Reconnect DPlus client</button>' +
'                    <button class="btn" id="btn-reconnect-ysfclient" onclick="URFAdmin.doReconnect(\'ysfclient\')" style="display:none;">Reconnect YSF client</button>' +
'                </div>' +
'                <h3>Blocked protocol pairs</h3>' +
'                <p class="hint">Traffic does not cross between a blocked pair, in either direction.</p>' +
'                <div class="blocks" id="block-rules"></div>' +
'                <div class="form-row">' +
'                    <label>Protocol <select id="block-proto-a" class="field w-m"></select></label>' +
'                    <span aria-hidden="true" style="padding-bottom:0.35rem">&#8596;</span>' +
'                    <label>Protocol <select id="block-proto-b" class="field w-m"></select></label>' +
'                    <button class="btn danger" onclick="URFAdmin.doToggleBlock(\'block\')">Block</button>' +
'                    <button class="btn" onclick="URFAdmin.doToggleBlock(\'unblock\')">Unblock</button>' +
'                    <button class="btn quiet" onclick="URFAdmin.doBlockReset()">Back to urfd.ini</button>' +
'                </div>' +
'                <h3>Last heard</h3>' +
'                <div><button class="btn danger" onclick="URFAdmin.doClearUsers()">Clear last heard list</button></div>' +
'            </section>' +
'            <section class="admin-sec" id="sec-log">' +
'                <header><h2>Log</h2><button class="btn small quiet" onclick="URFAdmin.refreshLog()">Refresh</button></header>' +
'                <pre id="log-output" class="log-out"></pre>' +
'            </section>' +
'        </div>' +
'    </div>' +
'</div>';

// what the buttons in the page call
window.URFAdmin = {
    removeTG: removeTG, doKerchunk: doKerchunk, doReconnect: doReconnect, removeDstarMap: removeDstarMap,
    removeYsfMap: removeYsfMap, removeSvxsUser: removeSvxsUser, removeMmdvmUser: removeMmdvmUser,
    doToggleBlock: doToggleBlock, doBlockReset: doBlockReset, doClearUsers: doClearUsers, doLogout: doLogout,
    doCredentialsFlush: doCredentialsFlush, refreshTGList: refreshTGList, refreshDcsMapList: refreshDcsMapList,
    refreshYsfMapList: refreshYsfMapList, refreshSvxsUserList: refreshSvxsUserList, refreshSvxsPeerList: refreshSvxsPeerList,
    refreshMmdvmUserList: refreshMmdvmUserList, refreshMmdvmPeerList: refreshMmdvmPeerList, refreshCredentials: refreshCredentials,
    refreshTCStats: refreshTCStats, refreshLog: refreshLog
};
var forms = { addTG: addTG, addDcsMap: addDcsMap, addYsfMap: addYsfMap, addSvxsUser: addSvxsUser, addMmdvmUser: addMmdvmUser };

URF.pages.admin = {
    refresh: false,
    mount: function (el) {
        mounted = true;
        el.innerHTML = MARKUP;
        // remote module letters A to Z
        var s = q('dcs-remote-mod');
        for (var i = 65; i <= 90; i++) { var o = document.createElement('option'); o.value = o.textContent = String.fromCharCode(i); s.appendChild(o); }
        q('login-form').addEventListener('submit', function (e) { e.preventDefault(); doLogin(); });
        el.addEventListener('submit', function (e) {
            var f = e.target;
            if (f.dataset.submit && forms[f.dataset.submit]) { e.preventDefault(); forms[f.dataset.submit](); return; }
            if (f.dataset.pluginForm) {
                e.preventDefault();
                var id = f.dataset.pluginForm, ui = pluginUi[id];
                if (!ui) return;
                var args = {};
                f.querySelectorAll('input[name]').forEach(function (i) { args[i.name] = i.value; });
                pluginPost(ui.plugin, ui.section.form.command, args, function (resp) {
                    showAlert(resp);
                    if (resp.status === 'ok') { f.reset(); refreshPluginSection(id); }
                });
            }
        });
        el.addEventListener('click', function (e) {
            var r = e.target.closest('[data-plugin-refresh]');
            if (r) { refreshPluginSection(r.dataset.pluginRefresh); return; }
            var d = e.target.closest('[data-plugin-remove]');
            if (d) {
                var id = d.dataset.pluginRemove, ui = pluginUi[id];
                if (!ui) return;
                var args = {};
                args[ui.section.remove.arg || ui.section.remove.key] = d.dataset.value;
                pluginPost(ui.plugin, ui.section.remove.command, args, function (resp) {
                    showAlert(resp);
                    if (resp.status === 'ok') refreshPluginSection(id);
                });
            }
        });
        el.addEventListener('change', function (e) {
            if (e.target.id === 'add-protocol') syncTimeslot();
            if (e.target.id === 'dcs-proto') setVal('dcs-port', portDefaults[e.target.value] || 30051);
        });
        pluginUi = {}; pluginUiKey = '';
        if (URF.state.connected) resume();
        else showLogin();
    },
    unmount: function () { mounted = false; stopRefresh(); },
    update: function () {}
};

// a new connection has no login yet: the token logs it in again
URF.onOpen.push(function () { if (mounted) resume(); });
URF.onClose.push(function () { stopRefresh(); });

})();
