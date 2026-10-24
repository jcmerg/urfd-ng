<?php
// Peers: other reflectors that share modules with this one

$IPMode = $PageOptions['PeerPage']['IPModus'];
$MC = $PageOptions['PeerPage']['MasqueradeCharacter'];
$ShowIP = ($IPMode != 'HideIP');

// dashboard links from the XLX directory; without it the names stay plain text
$DashboardURLs = array();
if ($Reflector->PeerCount() > 0) {
    $List = GetReflectorList();
    if ($List !== false) {
        $XML = new ParseXML();
        foreach ($List as $r) {
            $url = trim(html_entity_decode((string)$XML->GetElement($r, "dashboardurl"), ENT_QUOTES | ENT_HTML5, 'UTF-8'));
            if (preg_match('#^https?://#i', $url)) $DashboardURLs[(string)$XML->GetElement($r, "name")] = $url;
        }
    }
}
?>
<div class="page">
    <div>
        <h1>Peers</h1>
        <p class="lede">Other reflectors that share modules with <?php echo h($Name); ?>. A call on a shared module is heard on both.</p>
    </div>
    <div class="scroll">
        <table class="t">
            <thead>
                <tr><th>Reflector</th><th>Mode</th><th>Shared modules</th><th class="r">Last heard</th><th class="r opt">Linked for</th><?php if ($ShowIP) { ?><th class="opt">IP</th><?php } ?></tr>
            </thead>
            <tbody>
<?php
$count = min($Reflector->PeerCount(), (int)$PageOptions['PeerPage']['LimitTo'] + 1);
for ($i = 0; $i < $count; $i++) {
    $peer = $Reflector->Peers[$i];
    $name = $peer->GetCallSign();
    echo '<tr>';
    if (isset($DashboardURLs[$name])) {
        echo '<td class="cs"><a href="'.h($DashboardURLs[$name]).'" target="_blank" rel="noopener noreferrer" title="Open the dashboard of '.h($name).'">'.h($name).'</a></td>';
    } else {
        echo '<td class="cs"><b>'.h($name).'</b></td>';
    }
    echo '<td>'.h($peer->GetProtocol()).'</td>';
    $mods = array();
    foreach (str_split(preg_replace('/[^A-Z]/', '', strtoupper((string)$peer->GetLinkedModule()))) as $m) {
        if ($m !== '') $mods[] = '<span class="mod'.(isset($Modules[$m]) && $Modules[$m]['nodes'] > 0 ? ' on' : '').'">'.h($m).'</span>';
    }
    echo '<td>'.(count($mods) ? implode(' ', $mods) : '<span class="muted">none</span>').'</td>';
    echo '<td class="r" title="'.h(date("d.m.Y H:i", $peer->GetLastHeardTime())).'">'.FormatAgo($peer->GetLastHeardTime()).'</td>';
    echo '<td class="r opt">'.FormatSpan(time() - $peer->GetConnectTime()).'</td>';
    if ($ShowIP) echo '<td class="opt ip">'.h(MaskIP($peer->GetIP(), $IPMode, $MC)).'</td>';
    echo '</tr>';
}
if ($count == 0) {
    echo '<tr class="empty"><td colspan="'.($ShowIP ? 6 : 5).'">No peer reflector is linked. Peers are set up in the interlink file of urfd.</td></tr>';
}
?>
            </tbody>
        </table>
    </div>
</div>
