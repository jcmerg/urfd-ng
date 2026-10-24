<?php

/*
 *  URF dashboard, originally the XLX dashboard by the DVBrazil Team.
 *  Page shell: top bar, the slim on-air line, the page itself and the footer.
*/

if (file_exists("./pgs/functions.php")) {
    require_once("./pgs/functions.php");
} else {
    die("functions.php does not exist.");
}
if (file_exists("./pgs/config.inc.php")) {
    require_once("./pgs/config.inc.php");
} else {
    die("config.inc.php does not exist.");
}

if (!class_exists('ParseXML')) require_once("./pgs/class.parsexml.php");
if (!class_exists('Node')) require_once("./pgs/class.node.php");
if (!class_exists('xReflector')) require_once("./pgs/class.reflector.php");
if (!class_exists('Station')) require_once("./pgs/class.station.php");
if (!class_exists('Peer')) require_once("./pgs/class.peer.php");

$Reflector = new xReflector();
$Reflector->SetFlagFile("./pgs/country.csv");
$Reflector->SetPIDFile($Service['PIDFile']);
$Reflector->SetXMLFile($Service['XMLFile']);

$Reflector->LoadXML();
$Reflector->LoadFlags();

// the page to show, only one of the known ones: it ends up in the page and in a script
$Pages = array('users', 'modules', 'modulesd', 'repeaters', 'peers', 'reflectors', 'livequadnet', 'admin');
$Show = (isset($_GET['show']) && in_array($_GET['show'], $Pages, true)) ? $_GET['show'] : 'users';
if ($Show == 'modulesd') $Show = 'modules';   // the old module overview is part of Modules now
if ($Show == 'admin' && !(isset($Admin) && $Admin['Enable'])) $Show = 'users';

$Modules = DashboardModules($Reflector);
$OnAir   = OnAirNow($Reflector, $Modules);

$Name    = str_replace("XLX", "URF", (string)$Reflector->GetReflectorName());
$Refresh = $PageOptions['PageRefreshActive'] && in_array($Show, array('users', 'modules', 'repeaters', 'peers'), true);

function NavLink($page, $label, $count = null) {
    global $Show;
    $href = ($page == 'users') ? './index.php' : './index.php?show='.$page;
    echo '<a href="'.$href.'"'.(($Show == $page) ? ' aria-current="page"' : '').'>'.$label
        .(($count !== null) ? '<small>'.(int)$count.'</small>' : '').'</a>';
}

// the slim LCD line shown above every page but Last heard and Admin
function OnAirStrip($Reflector, $OnAir, $Modules) {
    if ($OnAir !== null) {
        $also = array();
        foreach (OnAirAll($Reflector, $Modules) as $e) if ($e[0] != $OnAir[0]) $also[] = $e[0];
        sort($also);
        list($mod, $cs, $st) = $OnAir;
        echo '<div class="strip" id="strip" aria-live="polite"><div class="strip-in"><span class="ann">TX</span><b>'.h($cs).'</b>'.(($st && $st->GetName() != '') ? '<span>'.h($st->GetName()).'</span>' : '').'<span>Module '.h($mod).'</span>';
        if ($st) {
            if ($st->GetProtocol() != '') echo '<span>'.h($st->GetProtocol()).'</span>';
            $via = StationVia($Reflector, $st);
            if ($via != '') echo '<span>via '.h($via).'</span>';
        }
        echo AlsoOnAir($also);
        if ($st) {
            echo '<span class="t" data-since="'.(int)$st->GetLastHeardTime().'"></span>';
        }
        echo '</div></div>';
        return;
    }
    $st = $Reflector->GetStation(0);
    if ($st === false) {
        echo '<div class="strip idle" id="strip"><div class="strip-in"><span>Nobody heard yet</span></div></div>';
        return;
    }
    echo '<div class="strip idle" id="strip"><div class="strip-in"><span>Last heard</span><b>'.h($st->GetCallsignOnly()).'</b>'.($st->GetName() != '' ? '<span>'.h($st->GetName()).'</span>' : '').'<span>Module '.h($st->GetModule()).'</span>';
    if ($st->GetProtocol() != '') echo '<span>'.h($st->GetProtocol()).'</span>';
    echo '<span class="t">'.FormatAgo($st->GetLastHeardTime()).'</span></div></div>';
}

?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <meta name="description" content="<?php echo h($PageOptions['MetaDescription']); ?>">
    <meta name="keywords" content="<?php echo h($PageOptions['MetaKeywords']); ?>">
    <meta name="author" content="<?php echo h($PageOptions['MetaAuthor']); ?>">
    <meta name="robots" content="<?php echo h($PageOptions['MetaRobots'] ?? 'index,follow'); ?>">
    <title><?php echo h($Name); ?> Universal Reflector</title>
    <link rel="icon" href="./favicon.ico" type="image/vnd.microsoft.icon">
    <link rel="preload" href="./fonts/saira-latin.woff2" as="font" type="font/woff2" crossorigin>
    <link href="css/dashboard.css?v=<?php echo h($PageOptions['DashboardVersion']); ?>" rel="stylesheet">
    <script>
    // the pages hook into the in-place refresh through this
    window.URF = { onRefresh: [] };
    // theme before the first paint: system, light or dark
    try { var t = localStorage.getItem('urf-theme'); if (t === 'light' || t === 'dark') document.documentElement.setAttribute('data-theme', t); } catch (e) {}
    </script>
</head>
<body>
<?php if (file_exists("./tracking.php")) {
    include_once("tracking.php");
} ?>
<header class="bar">
    <div class="wrap">
        <a class="brand" href="./index.php"><b><?php echo h($Name); ?></b><span>Universal Reflector</span></a>
        <nav class="nav" aria-label="Pages" id="nav">
            <?php
            NavLink('users', 'Last heard');
            NavLink('modules', 'Modules');
            NavLink('repeaters', 'Links', $Reflector->NodeCount());
            NavLink('peers', 'Peers', $Reflector->PeerCount());
            NavLink('reflectors', 'Reflector list');
            NavLink('livequadnet', 'QuadNet live');
            ?>
        </nav>
        <div class="status">
            <?php if ($Reflector->GetServiceUptime() !== null) { ?><span class="up">Up <?php echo FormatSpan($Reflector->GetServiceUptime()); ?></span><?php } ?>
            <span class="ver">urfd <?php echo h($Reflector->GetVersion()); ?>, dashboard <?php echo h($PageOptions['DashboardVersion']); ?></span>
            <button class="theme-btn" id="theme-btn" type="button" aria-label="Theme: system" title="Theme: system. Click to switch">System</button>
        </div>
    </div>
</header>
<?php
if (isset($PageOptions['MOTD']) && trim($PageOptions['MOTD']) !== '') {
    echo '<div class="motd"><div class="wrap">'.h(trim($PageOptions['MOTD'])).'</div></div>';
}
?>
<main>
    <div class="wrap">
        <?php if ($Show != 'users' && $Show != 'admin') OnAirStrip($Reflector, $OnAir, $Modules); ?>
        <div id="page" data-now="<?php echo time(); ?>" data-state="<?php echo OnAirFingerprint($Reflector->GetXMLContent()); ?>">
<?php
switch ($Show) {
    case 'modules'     : require_once("./pgs/modules.php"); break;
    case 'repeaters'   : require_once("./pgs/repeaters.php"); break;
    case 'peers'       : require_once("./pgs/peers.php"); break;
    case 'reflectors'  : require_once("./pgs/reflectors.php"); break;
    case 'livequadnet' : require_once("./pgs/livequadnet.php"); break;
    case 'admin'       : require_once("./pgs/admin.php"); break;
    default            : require_once("./pgs/users.php");
}
?>
        </div>
    </div>
</main>

<footer class="foot">
    <div class="wrap">
        <span><?php
            // the address is put together by script, against harvesters
            $parts = explode('@', (string)$PageOptions['ContactEmail']);
            if (count($parts) == 2) {
                echo 'Sysop: <span class="email-protect" data-u="'.h($parts[0]).'" data-d="'.h($parts[1]).'">address needs JavaScript</span>';
            }
        ?></span>
        <?php if (isset($Admin) && $Admin['Enable']) { ?>
        <a class="pi" href="./index.php?show=admin" title="Admin">&pi;</a>
        <?php } ?>
    </div>
</footer>

<script>
(function () {
    var $ = function (id) { return document.getElementById(id); };

    document.querySelectorAll('.email-protect').forEach(function (el) {
        var a = el.dataset.u + '@' + el.dataset.d;
        el.innerHTML = '<a href="mai' + 'lto:' + a + '">' + a + '</a>';
    });

    // theme toggle: system, light, dark
    var modes = ['system', 'light', 'dark'], mode = 'system';
    try { mode = localStorage.getItem('urf-theme') || 'system'; } catch (e) {}
    function applyTheme() {
        if (mode === 'system') document.documentElement.removeAttribute('data-theme');
        else document.documentElement.setAttribute('data-theme', mode);
        var b = $('theme-btn'), label = mode.charAt(0).toUpperCase() + mode.slice(1);
        b.textContent = label;
        b.setAttribute('aria-label', 'Theme: ' + mode);
        b.title = 'Theme: ' + mode + '. Click to switch';
    }
    $('theme-btn').addEventListener('click', function () {
        mode = modes[(modes.indexOf(mode) + 1) % modes.length];
        try { localStorage.setItem('urf-theme', mode); } catch (e) {}
        applyTheme();
    });
    applyTheme();

    // running transmit timers, against the server's clock
    var skew = 0;
    function setSkew() { var p = $('page'); if (p) skew = Date.now() / 1000 - parseInt(p.dataset.now, 10); }
    function tick() {
        document.querySelectorAll('[data-since]').forEach(function (el) {
            var s = Math.max(0, Math.floor(Date.now() / 1000 - skew - parseInt(el.dataset.since, 10)));
            el.textContent = Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
        });
    }
    setSkew(); tick();
    setInterval(tick, 1000);

    window.URF.tick = tick;

<?php if ($Refresh) { ?>
    // reload the page content in place; keep open details and skip while someone types
    // and right away when the on-air state changes, which a small probe tells every 1.5 s
    var delay = <?php echo (int)$PageOptions['PageRefreshDelay']; ?>, timer = null, busy = false;
    function later() { clearTimeout(timer); timer = setTimeout(reload, delay); }
    function reload() {
        var a = document.activeElement;
        if (busy) return;
        if (a && $('page').contains(a) && (a.tagName === 'INPUT' || a.tagName === 'SELECT')) { later(); return; }
        busy = true;
        clearTimeout(timer);
        fetch(location.href, { credentials: 'same-origin' }).then(function (r) { return r.ok ? r.text() : null; }).then(function (html) {
            if (!html) return;
            var doc = new DOMParser().parseFromString(html, 'text/html');
            var open = [];
            document.querySelectorAll('#page details[open][id]').forEach(function (d) { open.push(d.id); });
            ['page', 'strip', 'nav'].forEach(function (id) {
                var cur = $(id), next = doc.getElementById(id);
                if (cur && next) { cur.innerHTML = next.innerHTML; cur.className = next.className; Array.prototype.forEach.call(next.attributes, function (at) { if (at.name.indexOf('data-') === 0) cur.setAttribute(at.name, at.value); }); }
            });
            open.forEach(function (id) { var d = $(id); if (d) d.open = true; });
            setSkew(); tick();
            window.URF.onRefresh.forEach(function (fn) { fn(); });
        }).catch(function () {}).then(function () { busy = false; later(); });
    }
    function probe() {
        if (document.hidden || busy) return;
        fetch('./api/onair.php', { credentials: 'same-origin', cache: 'no-store' }).then(function (r) { return r.ok ? r.json() : null; }).then(function (j) {
            if (j && j.state && j.state !== $('page').dataset.state) reload();
        }).catch(function () {});
    }
    later();
    setInterval(probe, 1500);
<?php } ?>
})();
</script>
</body>
</html>
