<script src="./js/jquery-1.12.4.min.js"></script>
<script>
var adminApiUrl = './api/admin.php';
var isAuthenticated = false;

// a command of a plugin, see the "plugin" action of api/admin.php
function pluginPost(name, command, args, callback) {
    adminPost({action: 'plugin', name: name, command: command, args: args || {}}, callback);
}

// everything shown here may come from outside (hotspot config, node info,
// host names), so it goes into the page escaped
function esc(v) {
    return String(v === undefined || v === null ? '' : v)
        .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
// the plugins of urfd by their protocol name in lower case, from the status
var pluginNames = {};
function pluginProtoName(p) { return pluginNames[p]; }
// a value as a quoted JavaScript argument inside an onclick="..." attribute
function jsArg(v) {
    return esc(JSON.stringify(String(v === undefined || v === null ? '' : v)));
}

function adminPost(data, callback) {
    $.ajax({
        url: adminApiUrl,
        type: 'POST',
        contentType: 'application/json',
        headers: {'X-Requested-With': 'XMLHttpRequest'},
        data: JSON.stringify(data),
        success: function(resp) {
            if (typeof resp === 'string') resp = JSON.parse(resp);
            callback(resp);
        },
        error: function(xhr) {
            var resp = {status: 'error', message: 'Request failed'};
            try { resp = JSON.parse(xhr.responseText); } catch(e) {}
            if (xhr.status === 401) {
                isAuthenticated = false;
                showLogin();
            }
            callback(resp);
        }
    });
}

function showLogin() {
    $('#admin-content').hide();
    $('#admin-login').show();
    $('#admin-status').html('');
}

var adminRefreshTimer = null;

function showAdmin() {
    $('#admin-login').hide();
    $('#admin-content').show();
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
    if (adminRefreshTimer) clearInterval(adminRefreshTimer);
    adminRefreshTimer = setInterval(function() {
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

function doLogin() {
    var pw = $('#admin-password').val();
    adminPost({action: 'login', password: pw}, function(resp) {
        if (resp.status === 'ok') {
            isAuthenticated = true;
            showAdmin();
        } else {
            $('#login-error').text(resp.message || 'The password was not accepted. Use the [Admin] Password from urfd.ini.').show();
        }
    });
}

function doLogout() {
    if (adminRefreshTimer) { clearInterval(adminRefreshTimer); adminRefreshTimer = null; }
    adminPost({action: 'logout'}, function() {
        isAuthenticated = false;
        showLogin();
    });
}

function refreshTGList() {
    adminPost({action: 'tg_list'}, function(resp) {
        var tbody = $('#tg-table-body');
        tbody.empty();
        if (resp.status === 'ok' && resp.mappings) {
            resp.mappings.forEach(function(m) {
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
                    actions += '<button class="btn small danger" onclick="removeTG(' + jsArg(proto) + ',' + parseInt(m.tg, 10) + ')">Remove</button> ';
                }
                if (m.protocol === 'mmdvmclient' && !m['static']) {
                    actions += '<button class="btn small" onclick="doKerchunk(' + parseInt(m.tg, 10) + ',' + parseInt(m.client || 1, 10) + ')" title="Send kerchunk to the master">Kerchunk</button>';
                }
                var dirStr = m['primary'] ? 'TX/RX' : 'RX';
                tbody.append(
                    '<tr>' +
                    '<td>' + (m.client_name ? esc(m.client_name) + ' <span class="muted">' + esc(m.network || '') + '</span>'
                        : esc({mmdvmclient:'MMDVMClient',svxreflector:'SvxReflector',svx:'SVX'}[m.protocol] || pluginProtoName(m.protocol) || m.protocol)) + '</td>' +
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
            tbody.append('<tr class=\"empty\"><td colspan=\"8\">No TG mappings configured</td></tr>');
        }
    });
}

function addTG() {
    var data = {
        action: 'tg_add',
        protocol: $('#add-protocol').val(),
        tg: parseInt($('#add-tg').val()),
        module: $('#add-module').val(),
        ttl: parseInt($('#add-ttl').val()) || 900
    };
    var ts = parseInt($('#add-ts').val());
    if (ts) data.ts = ts;

    adminPost(data, function(resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshTGList();
    });
}

function removeTG(protocol, tg) {
    adminPost({action: 'tg_remove', protocol: protocol, tg: tg}, function(resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshTGList();
    });
}

function doKerchunk(tg, client) {
    adminPost({action: 'kerchunk', tg: tg, client: client || 1}, function(resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshTGList();
    });
}

function doReconnect(protocol, client) {
    var data = {action: 'reconnect', protocol: protocol};
    if (client) data.client = client;
    adminPost(data, function(resp) {
        showAlert(resp);
    });
}

function refreshDcsMapList() {
    // aggregate all D-Star client mappings (DCS, DExtra, DPlus)
    var tbody = $('#dcs-map-table-body');
    tbody.empty();
    var allMappings = [];
    var pending = 3;
    function renderAll() {
        if (--pending > 0) return;
        allMappings.forEach(function(m) {
            var connLabel = m.connected ?
                '<span class="state-ok">connected</span>' :
                '<span class="state-bad">disconnected</span>';
            tbody.append(
                '<tr>' +
                '<td>' + esc(m.proto) + '</td>' +
                '<td>' + esc(m.host) + ':' + esc(m.port) + '</td>' +
                '<td>' + esc(m.remote_module) + '</td>' +
                '<td>' + esc(m.local_module) + '</td>' +
                '<td>' + connLabel + '</td>' +
                '<td><button class="btn small danger" onclick="removeDstarMap(' + jsArg(m.proto_key) + ',' + jsArg(m.local_module) + ')">Remove</button></td>' +
                '</tr>'
            );
        });
        if (allMappings.length === 0) {
            tbody.append('<tr class=\"empty\"><td colspan=\"6\">No D-Star client mappings configured</td></tr>');
        }
    }
    $.each([
        {action: 'dcs_map_list', proto: 'DCS', key: 'dcs'},
        {action: 'dextra_map_list', proto: 'DExtra', key: 'dextra'},
        {action: 'dplus_map_list', proto: 'DPlus', key: 'dplus'}
    ], function(_, p) {
        adminPost({action: p.action}, function(resp) {
            if (resp.status === 'ok' && resp.mappings) {
                resp.mappings.forEach(function(m) {
                    m.proto = p.proto; m.proto_key = p.key;
                    allMappings.push(m);
                });
            }
            renderAll();
        });
    });
}

function addDcsMap() {
    var proto = $('#dcs-proto').val();
    var portDefaults = {dcs: 30051, dextra: 30001, dplus: 20001};
    var data = {
        action: proto + '_map_add',
        host: $('#dcs-host').val(),
        port: parseInt($('#dcs-port').val()) || portDefaults[proto] || 30051,
        remote_module: $('#dcs-remote-mod').val(),
        local_module: $('#dcs-local-mod').val()
    };
    adminPost(data, function(resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshDcsMapList();
    });
}

function removeDstarMap(protoKey, localMod) {
    adminPost({action: protoKey + '_map_remove', local_module: localMod}, function(resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshDcsMapList();
    });
}

function refreshYsfMapList() {
    adminPost({action: 'ysf_map_list'}, function(resp) {
        var tbody = $('#ysf-map-table-body');
        tbody.empty();
        if (resp.status === 'ok' && resp.mappings) {
            resp.mappings.forEach(function(m) {
                var connLabel = m.connected ?
                    '<span class="state-ok">connected</span>' :
                    '<span class="state-bad">disconnected</span>';
                var dgidLabel = m.dgid > 0 ? m.dgid : '-';
                tbody.append(
                    '<tr>' +
                    '<td>' + esc(m.host) + ':' + esc(m.port) + '</td>' +
                    '<td>' + esc(m.local_module) + '</td>' +
                    '<td>' + esc(dgidLabel) + '</td>' +
                    '<td>' + connLabel + '</td>' +
                    '<td><button class="btn small danger" onclick="removeYsfMap(' + jsArg(m.local_module) + ')">Remove</button></td>' +
                    '</tr>'
                );
            });
        }
        if (!resp.mappings || resp.mappings.length === 0) {
            tbody.append('<tr class=\"empty\"><td colspan=\"5\">No YSF mappings configured</td></tr>');
        }
    });
}

function addYsfMap() {
    var data = {
        action: 'ysf_map_add',
        host: $('#ysf-host').val(),
        port: parseInt($('#ysf-port').val()) || 42000,
        local_module: $('#ysf-local-mod').val(),
        dgid: parseInt($('#ysf-dgid').val()) || 0
    };
    adminPost(data, function(resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshYsfMapList();
    });
}

function removeYsfMap(localMod) {
    adminPost({action: 'ysf_map_remove', local_module: localMod}, function(resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshYsfMapList();
    });
}

function refreshSvxsUserList() {
    adminPost({action: 'svxs_user_list'}, function(resp) {
        var tbody = $('#svxs-user-table-body');
        tbody.empty();
        if (resp.status === 'ok' && resp.users) {
            resp.users.forEach(function(u) {
                tbody.append(
                    '<tr>' +
                    '<td>' + esc(u) + dbTag(resp.sources && resp.sources[u]) + '</td>' +
                    '<td><button class="btn small danger" onclick="removeSvxsUser(' + jsArg(u) + ')">Remove</button></td>' +
                    '</tr>'
                );
            });
        }
        if (!resp.users || resp.users.length === 0) {
            tbody.append('<tr class=\"empty\"><td colspan=\"2\">No SVX users configured</td></tr>');
        }
    });
}

function addSvxsUser() {
    var data = {
        action: 'svxs_user_add',
        callsign: $('#svxs-callsign').val(),
        password: $('#svxs-password').val()
    };
    adminPost(data, function(resp) {
        showAlert(resp);
        if (resp.status === 'ok') {
            $('#svxs-callsign').val('');
            $('#svxs-password').val('');
            refreshSvxsUserList();
        }
    });
}

function removeSvxsUser(callsign) {
    adminPost({action: 'svxs_user_remove', callsign: callsign}, function(resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshSvxsUserList();
    });
}


function refreshMmdvmUserList() {
    adminPost({action: 'mmdvm_user_list'}, function(resp) {
        var tbody = $('#mmdvm-user-table-body');
        tbody.empty();
        if (resp.status === 'ok' && resp.users) {
            resp.users.forEach(function(u) {
                tbody.append(
                    '<tr>' +
                    '<td>' + esc(u.dmrid) + dbTag(u.source) + '</td>' +
                    '<td>' + esc(u.callsign) + '</td>' +
                    '<td><button class="btn small danger" onclick="removeMmdvmUser(' + parseInt(u.dmrid, 10) + ')">Remove</button></td>' +
                    '</tr>'
                );
            });
        }
        if (!resp.users || resp.users.length === 0) {
            tbody.append('<tr class=\"empty\"><td colspan=\"3\">No MMDVM users configured</td></tr>');
        }
    });
}

function addMmdvmUser() {
    var id = $('#mmdvm-user-id').val().trim();
    var pw = $('#mmdvm-user-pw').val().trim();
    if (!id) return;
    var data = {action: 'mmdvm_user_add', password: pw};
    if (/^\d+$/.test(id))
        data.dmrid = parseInt(id);
    else
        data.callsign = id;
    adminPost(data, function(resp) {
        showAlert(resp, '#mmdvm-user-alerts');
        if (resp.status === 'ok') {
            $('#mmdvm-user-id').val('');
            $('#mmdvm-user-pw').val('');
            refreshMmdvmUserList();
        }
    });
}

function removeMmdvmUser(dmrid) {
    adminPost({action: 'mmdvm_user_remove', dmrid: dmrid}, function(resp) {
        showAlert(resp, '#mmdvm-user-alerts');
        if (resp.status === 'ok') refreshMmdvmUserList();
    });
}

function refreshMmdvmPeerList() {
    adminPost({action: 'mmdvm_peer_list'}, function(resp) {
        var tbody = $('#mmdvm-peers-table-body');
        tbody.empty();
        if (resp.status === 'ok' && resp.peers && resp.peers.length > 0) {
            resp.peers.forEach(function(p) {
                var info  = p.peer_info || {};
                var dmrid = info.dmrid || '';
                var loc   = info.location || '';
                var sw    = [info.packageId, info.softwareId].filter(Boolean).join(' / ');
                var rx    = info.rxFreq ? (info.rxFreq / 1e6).toFixed(4) : '';
                var tx    = info.txFreq ? (info.txFreq / 1e6).toFixed(4) : '';
                var freq  = (rx && tx) ? rx + ' / ' + tx : (rx || tx || '');
                var since = p.connected_since ? new Date(p.connected_since * 1000).toLocaleString() : '';
                var cs    = (p.callsign || '').trim();
                tbody.append(
                    '<tr>' +
                    '<td>' + esc(cs) + '</td>' +
                    '<td>' + esc(dmrid) + '</td>' +
                    '<td>' + esc(freq) + '</td>' +
                    '<td>' + esc(loc) + '</td>' +
                    '<td>' + esc(sw) + '</td>' +
                    '<td>' + esc(p.module && p.module !== ' ' ? p.module : '') + '</td>' +
                    '<td>' + esc(since) + '</td>' +
                    '</tr>'
                );
            });
        } else {
            tbody.append('<tr class=\"empty\"><td colspan=\"7\">No MMDVM nodes connected</td></tr>');
        }
    });
}

function refreshSvxsPeerList() {
    adminPost({action: 'svxs_peer_list'}, function(resp) {
        var tbody = $('#svxs-peers-table-body');
        tbody.empty();
        if (resp.status === 'ok' && resp.peers) {
            resp.peers.forEach(function(p) {
                var tgs   = (p.subscribed_tgs && p.subscribed_tgs.length) ? p.subscribed_tgs.join(', ') : '-';
                if (p.monitored_tgs && p.monitored_tgs.length) tgs += ' (monitor ' + p.monitored_tgs.join(', ') + ')';
                var sw    = (p.node_info && p.node_info.software) ? p.node_info.software : '';
                var udp   = p.udp_discovered ? 'Yes' : 'No';
                var since = p.connected_since ? new Date(p.connected_since * 1000).toLocaleString() : '';
                tbody.append(
                    '<tr>' +
                    '<td>' + esc((p.callsign || '').trim()) + '</td>' +
                    '<td>' + esc(tgs) + '</td>' +
                    '<td>' + esc(sw) + '</td>' +
                    '<td>' + esc(udp) + '</td>' +
                    '<td>' + esc(since) + '</td>' +
                    '</tr>'
                );
            });
        }
        if (!resp.peers || resp.peers.length === 0) {
            tbody.append('<tr class=\"empty\"><td colspan=\"5\">No SVX nodes connected</td></tr>');
        }
    });
}

function doToggleBlock(action, a, b) {
    if (!a) a = $('#block-proto-a').val();
    if (!b) b = $('#block-proto-b').val();
    if (!a || !b || a === b) { showAlert({status: 'error', message: 'Pick two different protocols to block or unblock.'}); return; }
    adminPost({action: action, a: a, b: b}, function(resp) {
        showAlert(resp);
        refreshStatus();
    });
}

function doBlockReset() {
    adminPost({action: 'block_reset'}, function(resp) {
        showAlert(resp);
        refreshStatus();
    });
}

function doClearUsers() {
    adminPost({action: 'clear_users'}, function(resp) {
        showAlert(resp);
    });
}

function refreshLog() {
    adminPost({action: 'log', lines: 50}, function(resp) {
        if (resp.status === 'ok' && resp.lines) {
            var pre = $('#log-output');
            pre.text(resp.lines.join('\n'));
            pre.scrollTop(pre[0].scrollHeight);
        }
    });
}

function refreshStatus() {
    adminPost({action: 'status'}, function(resp) {
        if (resp.status === 'ok') {
            $('#status-reflector').text(resp.reflector || '?');
            $('#status-version').text(resp.version || '?');
            // Populate module dropdown from available modules
            if (resp.modules) {
                var sel = $('#add-module');
                var current = sel.val();
                sel.empty();
                for (var i = 0; i < resp.modules.length; i++) {
                    var m = resp.modules[i];
                    sel.append('<option value="' + esc(m) + '">' + esc(m) + '</option>');
                }
                if (current) sel.val(current);

                // also populate DCS and YSF local module dropdowns
                $.each(['#dcs-local-mod', '#ysf-local-mod'], function(_, sel) {
                    var $s = $(sel), cur = $s.val();
                    $s.empty();
                    for (var i = 0; i < resp.modules.length; i++)
                        $s.append('<option value="' + esc(resp.modules[i]) + '">' + esc(resp.modules[i]) + '</option>');
                    if (cur) $s.val(cur);
                });
            }
            // Show/hide protocol options based on what's active
            var protoSel = $('#add-protocol');
            protoSel.empty();
            // one entry per MMDVMClient connection when there are several
            var clis = resp.mmdvm_clients || [];
            if (clis.length > 1) {
                clis.forEach(function(c) {
                    var v = 'mmdvmclient' + (c.client > 1 ? parseInt(c.client, 10) : '');
                    protoSel.append('<option value="' + esc(v) + '">' + esc(c.name) + ' (' + esc(c.network) + ')</option>');
                });
            } else if (resp.mmdvm_active) protoSel.append('<option value="mmdvmclient">MMDVMClient</option>');
            if (resp.svx_active) protoSel.append('<option value="svxreflector">SvxReflector</option>');
            if (resp.svxs_active) protoSel.append('<option value="svx">SVX</option>');
            if (!resp.mmdvm_active && !resp.svx_active && !resp.svxs_active) {
                protoSel.append('<option value="">No TG protocol active</option>');
            }
            protoSel.trigger('change');
            // Reconnect buttons
            var cliBtns = $('#mmdvm-reconnect-list');
            cliBtns.empty();
            if (clis.length > 1) {
                $('#btn-reconnect-mmdvm').hide();
                clis.forEach(function(c) {
                    cliBtns.append('<button class="btn" onclick="doReconnect(\'mmdvmclient\',' + parseInt(c.client, 10) + ')" title="' + esc(c.network) + (c.connected ? '' : ', not connected') + '">Reconnect ' + esc(c.name) + '</button> ');
                });
            } else if (resp.mmdvm_active) $('#btn-reconnect-mmdvm').show(); else $('#btn-reconnect-mmdvm').hide();
            if (resp.svx_active) $('#btn-reconnect-svx').show(); else $('#btn-reconnect-svx').hide();
            if (resp.dcsclient_active) $('#btn-reconnect-dcsclient').show(); else $('#btn-reconnect-dcsclient').hide();
            if (resp.dextraclient_active) $('#btn-reconnect-dextraclient').show(); else $('#btn-reconnect-dextraclient').hide();
            if (resp.dplusclient_active) $('#btn-reconnect-dplusclient').show(); else $('#btn-reconnect-dplusclient').hide();
            if (resp.dcsclient_active || resp.dextraclient_active || resp.dplusclient_active) $('#dcs-mapping-section').show(); else $('#dcs-mapping-section').hide();
            if (resp.ysfclient_active) { $('#btn-reconnect-ysfclient').show(); $('#ysf-mapping-section').show(); } else { $('#btn-reconnect-ysfclient').hide(); $('#ysf-mapping-section').hide(); }
            if (resp.svxs_active) { $('#svxs-user-section').show(); $('#svxs-peers-section').show(); refreshSvxsPeerList(); } else { $('#svxs-user-section').hide(); $('#svxs-peers-section').hide(); }
            pluginNames = {};
            (resp.plugins || []).forEach(function(p) { pluginNames[String(p.name).toLowerCase()] = p.name; });
            renderPluginSections(resp.plugins);
            if (resp.mmdvm_server_active) { $('#mmdvm-user-section').show(); $('#mmdvm-peers-section').show(); refreshMmdvmUserList(); refreshMmdvmPeerList(); } else { $('#mmdvm-user-section').hide(); $('#mmdvm-peers-section').hide(); }

            syncIndex();

            // Block rules — collect bidirectional pairs
            var blockedPairs = {};  // "A|B" (sorted) -> true
            if (resp.blocks && resp.blocks.length > 0) {
                resp.blocks.forEach(function(b) {
                    if (b.from === b.to) return;
                    var key = [b.from, b.to].sort().join('|');
                    blockedPairs[key] = true;
                });
                var html = '';
                for (var k in blockedPairs) {
                    var parts = k.split('|');
                    html += '<button type="button" class="btn small danger" '
                        + 'onclick="doToggleBlock(\'unblock\',' + jsArg(parts[0]) + ',' + jsArg(parts[1]) + ')" '
                        + 'title="Unblock">'
                        + esc(parts[0]) + ' &#8596; ' + esc(parts[1]) + ' &#10005;</button>';
                }
                $('#block-rules').html(html);
            } else {
                $('#block-rules').html('<span class="muted">No protocol pair is blocked.</span>');
            }

            // Populate protocol dropdowns from active protocols
            if (resp.active_protocols) {
                var selA = $('#block-proto-a'), selB = $('#block-proto-b');
                var curA = selA.val(), curB = selB.val();
                selA.empty(); selB.empty();
                resp.active_protocols.sort().forEach(function(p) {
                    selA.append('<option value="' + esc(p) + '">' + esc(p) + '</option>');
                    selB.append('<option value="' + esc(p) + '">' + esc(p) + '</option>');
                });
                if (curA) selA.val(curA);
                if (curB) selB.val(curB);
            }
        }
    });
}

function refreshTCStats() {
    adminPost({action: 'tc_stats'}, function(resp) {
        var tbody = $('#tc-table-body');
        tbody.empty();
        if (resp.status === 'ok' && resp.modules) {
            var codecNames = {0:'-', 1:'D-Star', 2:'DMR', 3:'Codec2/1600', 4:'Codec2/3200', 5:'P25', 6:'USRP/PCM', 7:'SVX/OPUS'};
            resp.modules.forEach(function(m) {
                var connLabel = m.connected ?
                    '<span class="state-ok">connected</span>' :
                    '<span class="state-bad">disconnected</span>';
                var streamLabel = m.streaming ?
                    '<span class="txtag">TX</span> ' + esc(m.user || '?') :
                    '<span class="muted">idle</span>';
                var codec = codecNames[m.codec_in] || '-';
                var rtt = '-';
                var packets = m.total_packets || 0;
                var mismatches = m.mismatch_count || 0;
                if (!m.connected) {
                    codec = '-';
                    packets = '-';
                    mismatches = '-';
                } else {
                    if (m.rt_count > 0 && m.rt_min_ms >= 0) {
                        rtt = m.rt_min_ms + ' / ' + m.rt_avg_ms + ' / ' + m.rt_max_ms + ' ms';
                    }
                }
                tbody.append(
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
        if (!resp.modules || resp.modules.length === 0) {
            tbody.append('<tr class=\"empty\"><td colspan=\"7\">No transcoded modules</td></tr>');
        }
    });
}

// users from the credential database get a tag, those from urfd.ini none
// the sections of urfd's plugins, drawn from the description each plugin gives
// in the status (plugins[].ui, see docs/PLUGINS.md); nothing here knows a plugin
var pluginUi = {};      // section element id -> {plugin, section}
var pluginUiKey = '';   // what is drawn, so it is drawn again only after a change

function pluginSlug(s) { return String(s).toLowerCase().replace(/[^a-z0-9]+/g, '-'); }

function renderPluginSections(plugins) {
    var active = (plugins || []).filter(function(p) { return p.active && Array.isArray(p.ui) && p.ui.length; });
    var key = JSON.stringify(active.map(function(p) { return [p.name, p.ui]; }));
    if (key === pluginUiKey) return;
    pluginUiKey = key;
    pluginUi = {};
    var nav = $('#plugin-nav').empty(), box = $('#plugin-sections').empty();
    active.forEach(function(p) {
        p.ui.forEach(function(sec) {
            var id = 'plugin-' + pluginSlug(p.name) + '-' + pluginSlug(sec.id || '');
            var title = sec.title || p.name;
            var cols = Array.isArray(sec.columns) ? sec.columns : [];
            var ncol = cols.length + (sec.remove ? 1 : 0);
            pluginUi[id] = {plugin: p.name, section: sec};
            nav.append('<a href="#' + id + '">' + esc(title) + '</a>');
            var html = '<section class="admin-sec" id="' + id + '">' +
                '<header><h2>' + esc(title) + '</h2>' + (sec.list ? '<button class="btn small quiet" data-plugin-refresh="' + id + '">Refresh</button>' : '') + '</header>';
            if (sec.form && Array.isArray(sec.form.fields)) {
                html += '<form class="form-row" data-plugin-form="' + id + '">';
                sec.form.fields.forEach(function(f) {
                    html += '<label>' + esc(f.label || f.name) +
                        ' <input type="' + (f.type === 'password' ? 'password' : 'text') + '" class="field w-' + (f.width === 'm' ? 'm' : 's') + '" name="' + esc(f.name) + '"' +
                        (f.placeholder ? ' placeholder="' + esc(f.placeholder) + '"' : '') + (f.required ? ' required' : '') + '></label>';
                });
                html += '<button type="submit" class="btn primary">' + esc(sec.form.submit || 'Add') + '</button></form>';
            }
            if (sec.list) {
                html += '<div class="scroll"><table class="t' + (ncol <= 3 ? ' narrow' : '') + '"><thead><tr>' +
                    cols.map(function(c) { return '<th>' + esc(c.label || c.key) + '</th>'; }).join('') + (sec.remove ? '<th></th>' : '') +
                    '</tr></thead><tbody><tr class="empty"><td colspan="' + ncol + '">Loading</td></tr></tbody></table></div>';
            }
            box.append(html + '</section>');
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
    pluginPost(ui.plugin, sec.list.command, {}, function(resp) {
        var tbody = $('#' + id + ' tbody');
        tbody.empty();
        var rows = (resp.status === 'ok' && Array.isArray(resp[sec.list.rows])) ? resp[sec.list.rows] : [];
        rows.forEach(function(row) {
            var tr = '<tr>' + cols.map(function(c) { return pluginCell(c, row); }).join('');
            if (sec.remove)
                tr += '<td><button class="btn small danger" data-plugin-remove="' + id + '" data-value="' + esc(row[sec.remove.key]) + '">Remove</button></td>';
            tbody.append(tr + '</tr>');
        });
        if (!rows.length)
            tbody.append('<tr class="empty"><td colspan="' + (cols.length + (sec.remove ? 1 : 0)) + '">' + esc(sec.list.empty || 'Nothing to show') + '</td></tr>');
    });
}

function refreshPluginSections() {
    Object.keys(pluginUi).forEach(refreshPluginSection);
}

$(document).on('click', '[data-plugin-refresh]', function() {
    refreshPluginSection($(this).attr('data-plugin-refresh'));
});
$(document).on('submit', 'form[data-plugin-form]', function(e) {
    e.preventDefault();
    var form = this, id = $(form).attr('data-plugin-form'), ui = pluginUi[id];
    if (!ui) return;
    var args = {};
    $(form).find('input[name]').each(function() { args[this.name] = $(this).val(); });
    pluginPost(ui.plugin, ui.section.form.command, args, function(resp) {
        showAlert(resp);
        if (resp.status === 'ok') { form.reset(); refreshPluginSection(id); }
    });
});
$(document).on('click', '[data-plugin-remove]', function() {
    var id = $(this).attr('data-plugin-remove'), ui = pluginUi[id];
    if (!ui) return;
    var args = {};
    args[ui.section.remove.arg || ui.section.remove.key] = $(this).attr('data-value');
    pluginPost(ui.plugin, ui.section.remove.command, args, function(resp) {
        showAlert(resp);
        if (resp.status === 'ok') refreshPluginSection(id);
    });
});

function dbTag(source) {
    return source === 'db' ? ' <span class="muted">(database)</span>' : '';
}

function refreshCredentials() {
    adminPost({action: 'credentials_status'}, function(resp) {
        var tbody = $('#cred-table-body');
        tbody.empty();
        if (resp.status !== 'ok') {
            tbody.append('<tr class="empty"><td colspan="2">' + esc(resp.message || 'No answer') + '</td></tr>');
            return;
        }
        if (resp.backend === 'ini') {
            tbody.append('<tr class="empty"><td colspan="2">No database: the logins come from urfd.ini only ([Credentials] Backend = ini)</td></tr>');
            $('#btn-cred-flush').hide();
            return;
        }
        var rows = [
            ['Backend', esc(resp.backend)],
            ['Database', resp.connected ? '<span class="state-ok">connected</span>' : '<span class="state-bad">not connected</span>'],
            ['Logins in the cache', esc(resp.cached)],
            ['Cache hits / misses', esc(resp.hits) + ' / ' + esc(resp.misses)],
            ['Database errors', esc(resp.errors)]
        ];
        rows.forEach(function(r) { tbody.append('<tr><th scope="row">' + r[0] + '</th><td>' + r[1] + '</td></tr>'); });
        $('#btn-cred-flush').show();
    });
}

function doCredentialsFlush() {
    adminPost({action: 'credentials_flush'}, function(resp) {
        showAlert(resp, '#cred-alerts');
        refreshCredentials();
    });
}

// the index lists only the sections the running protocols use
function syncIndex() {
    $('.admin-index a[href^="#"]').each(function() {
        var sec = $($(this).attr('href'));
        $(this).toggle(sec.length > 0 && sec.css('display') !== 'none');
    });
}

function showAlert(resp, target) {
    var cls = resp.status === 'ok' ? 'ok' : 'bad';
    var msg = resp.message || resp.status;
    var html = '<div class="alert ' + cls + '" role="status"><span>' + esc(msg) + '</span>' +
        '<button type="button" class="close" aria-label="Dismiss" onclick="$(this).parent().remove()">&times;</button></div>';
    $(target || '#admin-alerts').html(html);
}

function formatSeconds(s) {
    if (s < 0) return '<span class="muted">none</span>';
    var m = Math.floor(s / 60);
    var sec = s % 60;
    return m + ' min ' + sec + ' s';
}

$(document).ready(function() {
    adminPost({action: 'check_auth'}, function(resp) {
        if (resp.authenticated) {
            isAuthenticated = true;
            showAdmin();
            refreshStatus();
        } else {
            showLogin();
        }
    });

    // Show/hide timeslot based on protocol selection
    $(document).on('change', '#add-protocol', function() {
        if (String($(this).val()).indexOf('mmdvmclient') === 0) {
            $('#ts-group').show();
        } else {
            $('#ts-group').hide();
        }
    });
});
</script>

<!-- Login -->
<div id="admin-login" class="login" style="display:none;">
    <div>
        <h1>Admin</h1>
        <p class="lede">Sign in with the [Admin] Password from urfd.ini. The dashboard keeps no copy of it.</p>
    </div>
    <div id="login-error" class="alert bad" style="display:none;"></div>
    <form class="form-row" onsubmit="doLogin(); return false;">
        <label style="flex:1 1 12rem">Password
            <input type="password" class="field" id="admin-password" autocomplete="current-password" required>
        </label>
        <button type="submit" class="btn primary">Sign in</button>
    </form>
</div>

<!-- Admin -->
<div id="admin-content" style="display:none;">
    <div class="tbl-head" style="margin-bottom:1.5rem">
        <div>
            <h1>Admin</h1>
            <p class="lede"><span id="status-reflector">-</span>, urfd <span id="status-version">-</span>. Lists refresh every 10 seconds.</p>
        </div>
        <button class="btn quiet" onclick="doLogout()">Sign out</button>
    </div>
    <div id="admin-alerts" style="margin-bottom:1rem"></div>

    <div class="admin">
        <nav class="admin-index" aria-label="Admin sections">
            <a href="#sec-tg">Talkgroups</a>
            <a href="#dcs-mapping-section">D-Star links</a>
            <a href="#ysf-mapping-section">YSF links</a>
            <a href="#svxs-user-section">SVX users</a>
            <a href="#svxs-peers-section">SVX nodes</a>
            <span id="plugin-nav" style="display:contents"></span>
            <a href="#mmdvm-user-section">MMDVM users</a>
            <a href="#mmdvm-peers-section">MMDVM nodes</a>
            <a href="#sec-cred">Credentials</a>
            <a href="#sec-tc">Transcoder</a>
            <a href="#sec-ctl">Protocol controls</a>
            <a href="#sec-log">Log</a>
        </nav>

        <div class="admin-body">

            <section class="admin-sec" id="sec-tg">
                <header><h2>Talkgroups</h2><button class="btn small quiet" onclick="refreshTGList()">Refresh</button></header>
                <form class="form-row" onsubmit="addTG(); return false;">
                    <label>Protocol
                        <select class="field w-m" id="add-protocol">
                            <option value="mmdvmclient">MMDVMClient</option>
                            <option value="svxreflector">SvxReflector</option>
                        </select>
                    </label>
                    <label>Talkgroup
                        <input type="number" class="field w-s" id="add-tg" placeholder="26207" required>
                    </label>
                    <label>Module
                        <select class="field w-xs" id="add-module" required><option value="">-</option></select>
                    </label>
                    <label id="ts-group">Timeslot
                        <select class="field w-xs" id="add-ts"><option value="2">TS2</option><option value="1">TS1</option></select>
                    </label>
                    <label>Timeout in s
                        <input type="number" class="field w-s" id="add-ttl" value="900">
                    </label>
                    <button type="submit" class="btn primary">Add talkgroup</button>
                </form>
                <div class="scroll">
                    <table class="t">
                        <thead><tr><th>Protocol</th><th>Talkgroup</th><th>Module</th><th>Timeslot</th><th>Type</th><th>Direction</th><th>Ends in</th><th></th></tr></thead>
                        <tbody id="tg-table-body"><tr class="empty"><td colspan="8">Loading</td></tr></tbody>
                    </table>
                </div>
            </section>

            <section class="admin-sec" id="dcs-mapping-section" style="display:none;">
                <header><h2>D-Star links</h2><button class="btn small quiet" onclick="refreshDcsMapList()">Refresh</button></header>
                <form class="form-row" onsubmit="addDcsMap(); return false;">
                    <label>Protocol
                        <select class="field w-s" id="dcs-proto" onchange="var p={dcs:30051,dextra:30001,dplus:20001};$('#dcs-port').val(p[this.value]||30051);">
                            <option value="dcs">DCS</option>
                            <option value="dextra">DExtra</option>
                            <option value="dplus">DPlus</option>
                        </select>
                    </label>
                    <label>Host
                        <input type="text" class="field w-l" id="dcs-host" placeholder="dcs001.xreflector.net" required>
                    </label>
                    <label>Port
                        <input type="number" class="field w-s" id="dcs-port" value="30051">
                    </label>
                    <label>Remote module
                        <select class="field w-xs" id="dcs-remote-mod"></select>
                    </label>
                    <label>Local module
                        <select class="field w-xs" id="dcs-local-mod"><option value="">-</option></select>
                    </label>
                    <button type="submit" class="btn primary">Connect</button>
                </form>
                <div class="scroll">
                    <table class="t">
                        <thead><tr><th>Protocol</th><th>Reflector</th><th>Remote module</th><th>Local module</th><th>Status</th><th></th></tr></thead>
                        <tbody id="dcs-map-table-body"><tr class="empty"><td colspan="6">Loading</td></tr></tbody>
                    </table>
                </div>
            </section>

            <section class="admin-sec" id="ysf-mapping-section" style="display:none;">
                <header><h2>YSF links</h2><button class="btn small quiet" onclick="refreshYsfMapList()">Refresh</button></header>
                <form class="form-row" onsubmit="addYsfMap(); return false;">
                    <label>Host
                        <input type="text" class="field w-l" id="ysf-host" placeholder="ysf.reflector.net" required>
                    </label>
                    <label>Port
                        <input type="number" class="field w-s" id="ysf-port" value="42000">
                    </label>
                    <label>Local module
                        <select class="field w-xs" id="ysf-local-mod"><option value="">-</option></select>
                    </label>
                    <label>DG-ID
                        <input type="number" class="field w-xs" id="ysf-dgid" value="0" min="0" max="99">
                    </label>
                    <button type="submit" class="btn primary">Connect</button>
                </form>
                <div class="scroll">
                    <table class="t">
                        <thead><tr><th>Reflector</th><th>Local module</th><th>DG-ID</th><th>Status</th><th></th></tr></thead>
                        <tbody id="ysf-map-table-body"><tr class="empty"><td colspan="5">Loading</td></tr></tbody>
                    </table>
                </div>
            </section>

            <section class="admin-sec" id="svxs-user-section" style="display:none;">
                <header><h2>SVX users</h2><button class="btn small quiet" onclick="refreshSvxsUserList()">Refresh</button></header>
                <form class="form-row" onsubmit="addSvxsUser(); return false;">
                    <label>Callsign
                        <input type="text" class="field w-s" id="svxs-callsign" placeholder="N0CALL" required>
                    </label>
                    <label>Password
                        <input type="text" class="field w-m" id="svxs-password" required>
                    </label>
                    <button type="submit" class="btn primary">Add user</button>
                </form>
                <div class="scroll">
                    <table class="t narrow">
                        <thead><tr><th>Callsign</th><th></th></tr></thead>
                        <tbody id="svxs-user-table-body"><tr class="empty"><td colspan="2">Loading</td></tr></tbody>
                    </table>
                </div>
            </section>

            <section class="admin-sec" id="svxs-peers-section" style="display:none;">
                <header><h2>SVX nodes</h2><button class="btn small quiet" onclick="refreshSvxsPeerList()">Refresh</button></header>
                <div class="scroll">
                    <table class="t">
                        <thead><tr><th>Callsign</th><th>Talkgroups</th><th>Software</th><th>UDP</th><th>Connected since</th></tr></thead>
                        <tbody id="svxs-peers-table-body"><tr class="empty"><td colspan="5">Loading</td></tr></tbody>
                    </table>
                </div>
            </section>

            <div id="plugin-sections"></div>

            <section class="admin-sec" id="mmdvm-user-section" style="display:none;">
                <header><h2>MMDVM users</h2><button class="btn small quiet" onclick="refreshMmdvmUserList()">Refresh</button></header>
                <div id="mmdvm-user-alerts"></div>
                <form class="form-row" onsubmit="addMmdvmUser(); return false;">
                    <label>Callsign or DMR ID
                        <input type="text" class="field w-m" id="mmdvm-user-id" placeholder="N0CALL or 2634000" required>
                    </label>
                    <label>Password, optional
                        <input type="text" class="field w-m" id="mmdvm-user-pw">
                    </label>
                    <button type="submit" class="btn primary">Add user</button>
                </form>
                <div class="scroll">
                    <table class="t narrow">
                        <thead><tr><th>DMR ID</th><th>Callsign</th><th></th></tr></thead>
                        <tbody id="mmdvm-user-table-body"><tr class="empty"><td colspan="3">Loading</td></tr></tbody>
                    </table>
                </div>
            </section>

            <section class="admin-sec" id="mmdvm-peers-section" style="display:none;">
                <header><h2>MMDVM nodes</h2><button class="btn small quiet" onclick="refreshMmdvmPeerList()">Refresh</button></header>
                <div class="scroll">
                    <table class="t">
                        <thead><tr><th>Callsign</th><th>DMR ID</th><th>RX / TX in MHz</th><th>Location</th><th>Modem and software</th><th>Module</th><th>Connected since</th></tr></thead>
                        <tbody id="mmdvm-peers-table-body"><tr class="empty"><td colspan="7">Loading</td></tr></tbody>
                    </table>
                </div>
            </section>

            <section class="admin-sec" id="sec-cred">
                <header><h2>Credential database</h2><button class="btn small quiet" onclick="refreshCredentials()">Refresh</button></header>
                <div id="cred-alerts"></div>
                <p class="hint">Logins of MMDVM and SVX users (and those of plugins) who are not in urfd.ini. After a change made directly in the database, flush the cache so it counts at the next login; changes made on this page count at once.</p>
                <div class="scroll">
                    <table class="t narrow">
                        <tbody id="cred-table-body"><tr class="empty"><td colspan="2">Loading</td></tr></tbody>
                    </table>
                </div>
                <div style="margin-top:0.75rem"><button class="btn" id="btn-cred-flush" onclick="doCredentialsFlush()" style="display:none;">Flush cache</button></div>
            </section>

            <section class="admin-sec" id="sec-tc">
                <header><h2>Transcoder</h2><button class="btn small quiet" onclick="refreshTCStats()">Refresh</button></header>
                <div class="scroll">
                    <table class="t">
                        <thead><tr><th>Module</th><th>Connection</th><th>Stream</th><th>Codec</th><th>Packets</th><th>Mismatches</th><th>Round trip min / avg / max</th></tr></thead>
                        <tbody id="tc-table-body"><tr class="empty"><td colspan="7">Loading</td></tr></tbody>
                    </table>
                </div>
            </section>

            <section class="admin-sec" id="sec-ctl">
                <header><h2>Protocol controls</h2></header>
                <div class="form-row" style="background:none;border:0;padding:0">
                    <button class="btn" id="btn-reconnect-mmdvm" onclick="doReconnect('mmdvmclient')">Reconnect MMDVMClient</button>
                    <span id="mmdvm-reconnect-list"></span>
                    <button class="btn" id="btn-reconnect-svx" onclick="doReconnect('svxreflector')">Reconnect SvxReflector</button>
                    <button class="btn" id="btn-reconnect-dcsclient" onclick="doReconnect('dcsclient')" style="display:none;">Reconnect DCS client</button>
                    <button class="btn" id="btn-reconnect-dextraclient" onclick="doReconnect('dextraclient')" style="display:none;">Reconnect DExtra client</button>
                    <button class="btn" id="btn-reconnect-dplusclient" onclick="doReconnect('dplusclient')" style="display:none;">Reconnect DPlus client</button>
                    <button class="btn" id="btn-reconnect-ysfclient" onclick="doReconnect('ysfclient')" style="display:none;">Reconnect YSF client</button>
                </div>
                <h3>Blocked protocol pairs</h3>
                <p class="hint">Traffic does not cross between a blocked pair, in either direction.</p>
                <div class="blocks" id="block-rules"></div>
                <div class="form-row">
                    <label>Protocol
                        <select id="block-proto-a" class="field w-m"></select>
                    </label>
                    <span aria-hidden="true" style="padding-bottom:0.35rem">&#8596;</span>
                    <label>Protocol
                        <select id="block-proto-b" class="field w-m"></select>
                    </label>
                    <button class="btn danger" onclick="doToggleBlock('block')">Block</button>
                    <button class="btn" onclick="doToggleBlock('unblock')">Unblock</button>
                    <button class="btn quiet" onclick="doBlockReset()">Back to urfd.ini</button>
                </div>
                <h3>Last heard</h3>
                <div><button class="btn danger" onclick="doClearUsers()">Clear last heard list</button></div>
            </section>

            <section class="admin-sec" id="sec-log">
                <header><h2>Log</h2><button class="btn small quiet" onclick="refreshLog()">Refresh</button></header>
                <pre id="log-output" class="log-out"></pre>
            </section>
        </div>
    </div>
</div>
<script>
// remote module letters A to Z
(function() {
    var s = document.getElementById('dcs-remote-mod');
    for (var i = 65; i <= 90; i++) { var o = document.createElement('option'); o.value = o.textContent = String.fromCharCode(i); s.appendChild(o); }
})();
</script>

