<?php
/**
 * Admin API Bridge
 * Bridges HTTP POST requests to the urfd admin socket (TCP JSON protocol).
 * Handles session-based authentication with the admin socket.
 *
 * Usage:
 *   POST /api/admin.php
 *   Content-Type: application/json
 *   Body: {"action": "login", "password": "xxx"}
 *         {"action": "tg_list"}
 *         {"action": "tg_add", "protocol": "mmdvm", "tg": 26207, "module": "G", "ts": 2, "ttl": 900}
 *         {"action": "tg_remove", "protocol": "mmdvm", "tg": 26207}
 *         {"action": "status"}
 *         {"action": "reconnect", "protocol": "mmdvm"}
 */

require_once(__DIR__ . '/../pgs/functions.php');
AdminSessionStart();
header('Content-Type: application/json');
header('X-Content-Type-Options: nosniff');

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    echo json_encode(['status' => 'error', 'message' => 'POST only']);
    exit;
}

// Only the dashboard's own page may call this. Another site can send neither a
// JSON body nor the X-Requested-With header without a CORS preflight, and no
// preflight is answered here, so these two checks stop cross-site requests.
$contentType = strtolower(trim(explode(';', $_SERVER['CONTENT_TYPE'] ?? '')[0]));
if ($contentType !== 'application/json' || ($_SERVER['HTTP_X_REQUESTED_WITH'] ?? '') !== 'XMLHttpRequest') {
    http_response_code(403);
    echo json_encode(['status' => 'error', 'message' => 'Forbidden']);
    exit;
}

// Load config
require_once(__DIR__ . '/../pgs/config.inc.php');

if (!isset($Admin) || !$Admin['Enable']) {
    http_response_code(403);
    echo json_encode(['status' => 'error', 'message' => 'Admin interface disabled']);
    exit;
}

$input = json_decode(file_get_contents('php://input'), true);
if (!is_array($input) || !isset($input['action']) || !is_string($input['action'])) {
    http_response_code(400);
    echo json_encode(['status' => 'error', 'message' => 'Invalid request']);
    exit;
}

$action = $input['action'];

// Login action — authenticate with the admin socket and store token in session
if ($action === 'login') {
    if (!isset($input['password'])) {
        echo json_encode(['status' => 'error', 'message' => 'Missing password']);
        exit;
    }

    $result = adminSocketSend(['cmd' => 'auth', 'password' => $input['password']]);
    if ($result && isset($result['token'])) {
        // a new session id on login, so a planted one is worth nothing
        session_regenerate_id(true);
        $_SESSION['admin_token'] = $result['token'];
        echo json_encode(['status' => 'ok']);
    } else {
        http_response_code(401);
        echo json_encode(['status' => 'error', 'message' => $result['message'] ?? 'Authentication failed']);
    }
    exit;
}

// Logout
if ($action === 'logout') {
    unset($_SESSION['admin_token']);
    echo json_encode(['status' => 'ok']);
    exit;
}

// Check auth
if ($action === 'check_auth') {
    if (isset($_SESSION['admin_token'])) {
        echo json_encode(['status' => 'ok', 'authenticated' => true]);
    } else {
        echo json_encode(['status' => 'ok', 'authenticated' => false]);
    }
    exit;
}

// All other actions require authentication
if (!isset($_SESSION['admin_token'])) {
    http_response_code(401);
    echo json_encode(['status' => 'error', 'message' => 'Not authenticated']);
    exit;
}

$token = $_SESSION['admin_token'];

// Release the session file lock before talking to the admin socket. The admin page
// fires ~9 of these requests in parallel every 10s; holding the lock would serialize
// them all behind the slowest one and make the whole interface appear frozen.
session_write_close();

// Map actions to admin socket commands
switch ($action) {
    case 'tg_list':
        $cmd = ['cmd' => 'tg_list', 'token' => $token];
        if (isset($input['protocol'])) $cmd['protocol'] = $input['protocol'];
        break;

    case 'tg_add':
        $cmd = [
            'cmd'      => 'tg_add',
            'token'    => $token,
            'protocol' => $input['protocol'] ?? '',
            'tg'       => (int)($input['tg'] ?? 0),
            'module'   => $input['module'] ?? '',
        ];
        if (isset($input['ts']))  $cmd['ts']  = (int)$input['ts'];
        if (isset($input['ttl'])) $cmd['ttl'] = (int)$input['ttl'];
        if (isset($input['client'])) $cmd['client'] = (int)$input['client'];
        break;

    case 'tg_remove':
        $cmd = [
            'cmd'      => 'tg_remove',
            'token'    => $token,
            'protocol' => $input['protocol'] ?? '',
            'tg'       => (int)($input['tg'] ?? 0),
        ];
        if (isset($input['client'])) $cmd['client'] = (int)$input['client'];
        break;

    case 'status':
        $cmd = ['cmd' => 'status', 'token' => $token];
        break;

    case 'tc_stats':
        $cmd = ['cmd' => 'tc_stats', 'token' => $token];
        break;

    case 'credentials_status':
        $cmd = ['cmd' => 'credentials_status', 'token' => $token];
        break;

    case 'credentials_flush':
        $cmd = ['cmd' => 'credentials_flush', 'token' => $token];
        break;

    case 'log':
        $cmd = ['cmd' => 'log', 'token' => $token, 'lines' => (int)($input['lines'] ?? 50)];
        break;

    case 'reconnect':
        $cmd = [
            'cmd'      => 'reconnect',
            'token'    => $token,
            'protocol' => $input['protocol'] ?? '',
        ];
        if (isset($input['client'])) $cmd['client'] = (int)$input['client'];
        break;

    case 'kerchunk':
        $cmd = [
            'cmd'   => 'kerchunk',
            'token' => $token,
            'tg'    => (int)($input['tg'] ?? 0),
        ];
        if (isset($input['client'])) $cmd['client'] = (int)$input['client'];
        break;

    case 'svxs_user_add':
        $cmd = [
            'cmd'      => 'svxs_user_add',
            'token'    => $token,
            'callsign' => $input['callsign'] ?? '',
            'password' => $input['password'] ?? '',
        ];
        break;

    case 'svxs_user_remove':
        $cmd = [
            'cmd'      => 'svxs_user_remove',
            'token'    => $token,
            'callsign' => $input['callsign'] ?? '',
        ];
        break;

    case 'svxs_user_list':
        $cmd = ['cmd' => 'svxs_user_list', 'token' => $token];
        break;

    case 'block':
        $cmd = [
            'cmd'   => 'block',
            'token' => $token,
            'a'     => $input['a'] ?? '',
            'b'     => $input['b'] ?? '',
        ];
        break;

    case 'unblock':
        $cmd = [
            'cmd'   => 'unblock',
            'token' => $token,
            'a'     => $input['a'] ?? '',
            'b'     => $input['b'] ?? '',
        ];
        break;

    case 'block_reset':
        $cmd = ['cmd' => 'block_reset', 'token' => $token];
        break;

    case 'clear_users':
        $cmd = ['cmd' => 'clear_users', 'token' => $token];
        break;

    // --- MMDVM Server User Management ---
    case 'mmdvm_user_add':
        $cmd = [
            'cmd'   => 'mmdvm_user_add',
            'token' => $token,
        ];
        if (isset($input['dmrid']))    $cmd['dmrid'] = (int)$input['dmrid'];
        if (isset($input['callsign'])) $cmd['callsign'] = $input['callsign'];
        if (isset($input['password'])) $cmd['password'] = $input['password'];
        break;

    case 'mmdvm_user_remove':
        $cmd = [
            'cmd'   => 'mmdvm_user_remove',
            'token' => $token,
        ];
        if (isset($input['dmrid']))    $cmd['dmrid'] = (int)$input['dmrid'];
        if (isset($input['callsign'])) $cmd['callsign'] = $input['callsign'];
        break;

    case 'mmdvm_user_list':
        $cmd = ['cmd' => 'mmdvm_user_list', 'token' => $token];
        break;

    // --- MMDVM Server TG Management ---
    case 'mmdvm_tg_add':
        $cmd = [
            'cmd'    => 'mmdvm_tg_add',
            'token'  => $token,
            'tg'     => (int)($input['tg'] ?? 0),
            'module' => $input['module'] ?? '',
        ];
        if (isset($input['ts']))  $cmd['ts']  = (int)$input['ts'];
        if (isset($input['ttl'])) $cmd['ttl'] = (int)$input['ttl'];
        break;

    case 'mmdvm_tg_remove':
        $cmd = [
            'cmd'   => 'mmdvm_tg_remove',
            'token' => $token,
            'tg'    => (int)($input['tg'] ?? 0),
        ];
        break;

    case 'mmdvm_tg_list':
        $cmd = ['cmd' => 'mmdvm_tg_list', 'token' => $token];
        break;

    // --- MMDVM / SVX Peer Lists ---
    case 'mmdvm_peer_list':
        $cmd = ['cmd' => 'mmdvm_peer_list', 'token' => $token];
        break;

    case 'svxs_peer_list':
        $cmd = ['cmd' => 'svxs_peer_list', 'token' => $token];
        break;

    // a command of one of urfd's plugins: {"action": "plugin", "name": "Foo",
    // "command": "foo_list", "args": {...}}; the args go along as they are
    case 'plugin':
        $args = (isset($input['args']) && is_array($input['args'])) ? $input['args'] : [];
        unset($args['cmd'], $args['token'], $args['name'], $args['command']);
        $cmd = array_merge($args, [
            'cmd'     => 'plugin',
            'token'   => $token,
            'name'    => (string)($input['name'] ?? ''),
            'command' => (string)($input['command'] ?? ''),
        ]);
        break;

    case 'dcs_map_add':
        $cmd = [
            'cmd'           => 'dcs_map_add',
            'token'         => $token,
            'host'          => $input['host'] ?? '',
            'port'          => (int)($input['port'] ?? 30051),
            'remote_module' => $input['remote_module'] ?? '',
            'local_module'  => $input['local_module'] ?? '',
        ];
        break;

    case 'dcs_map_remove':
        $cmd = [
            'cmd'          => 'dcs_map_remove',
            'token'        => $token,
            'local_module' => $input['local_module'] ?? '',
        ];
        break;

    case 'dcs_map_list':
        $cmd = ['cmd' => 'dcs_map_list', 'token' => $token];
        break;

    case 'dextra_map_add':
        $cmd = [
            'cmd'           => 'dextra_map_add',
            'token'         => $token,
            'host'          => $input['host'] ?? '',
            'port'          => (int)($input['port'] ?? 30001),
            'remote_module' => $input['remote_module'] ?? '',
            'local_module'  => $input['local_module'] ?? '',
        ];
        break;

    case 'dextra_map_remove':
        $cmd = [
            'cmd'          => 'dextra_map_remove',
            'token'        => $token,
            'local_module' => $input['local_module'] ?? '',
        ];
        break;

    case 'dextra_map_list':
        $cmd = ['cmd' => 'dextra_map_list', 'token' => $token];
        break;

    case 'dplus_map_add':
        $cmd = [
            'cmd'           => 'dplus_map_add',
            'token'         => $token,
            'host'          => $input['host'] ?? '',
            'port'          => (int)($input['port'] ?? 20001),
            'remote_module' => $input['remote_module'] ?? '',
            'local_module'  => $input['local_module'] ?? '',
        ];
        break;

    case 'dplus_map_remove':
        $cmd = [
            'cmd'          => 'dplus_map_remove',
            'token'        => $token,
            'local_module' => $input['local_module'] ?? '',
        ];
        break;

    case 'dplus_map_list':
        $cmd = ['cmd' => 'dplus_map_list', 'token' => $token];
        break;

    case 'ysf_map_add':
        $cmd = [
            'cmd'          => 'ysf_map_add',
            'token'        => $token,
            'host'         => $input['host'] ?? '',
            'port'         => (int)($input['port'] ?? 42000),
            'local_module' => $input['local_module'] ?? '',
            'dgid'         => (int)($input['dgid'] ?? 0),
        ];
        break;

    case 'ysf_map_remove':
        $cmd = [
            'cmd'          => 'ysf_map_remove',
            'token'        => $token,
            'local_module' => $input['local_module'] ?? '',
        ];
        break;

    case 'ysf_map_list':
        $cmd = ['cmd' => 'ysf_map_list', 'token' => $token];
        break;

    default:
        echo json_encode(['status' => 'error', 'message' => 'Unknown action']);
        exit;
}

$result = adminSocketSend($cmd);

// If token was rejected, clear session
if ($result && isset($result['message']) && $result['message'] === 'authentication required') {
    // Session was closed above — reopen it to drop the stale token.
    AdminSessionStart();
    unset($_SESSION['admin_token']);
    session_write_close();
    http_response_code(401);
}

echo json_encode($result ?: ['status' => 'error', 'message' => 'No response from admin socket']);

/**
 * Send a JSON command to the urfd admin socket and return the parsed response.
 */
function adminSocketSend(array $cmd): ?array
{
    global $Admin;

    $sock = @stream_socket_client(
        'tcp://' . $Admin['Host'] . ':' . $Admin['Port'],
        $errno, $errstr, 5
    );

    if (!$sock) {
        error_log("Admin socket connect failed: $errstr ($errno)");
        return ['status' => 'error', 'message' => 'Cannot connect to admin socket'];
    }

    stream_set_timeout($sock, 5);

    $json = json_encode($cmd) . "\n";
    fwrite($sock, $json);

    // one reply is one line of any length (user and peer lists pass 8 KB)
    $response = fgets($sock);
    fclose($sock);

    if ($response === false) {
        return ['status' => 'error', 'message' => 'No response from admin socket'];
    }

    return json_decode(trim($response), true);
}
