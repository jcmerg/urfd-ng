<?php
// Links: repeaters, hotspots and gateways linked to the reflector

$IPMode = $PageOptions['RepeatersPage']['IPModus'];
$MC = $PageOptions['RepeatersPage']['MasqueradeCharacter'];
$ShowIP = ($IPMode != 'HideIP');

function NodeBand($node) {
    if ($node->GetPrefix() == 'REF') return 'REF link';
    if ($node->GetPrefix() == 'XRF') return 'XRF link';
    switch ($node->GetSuffix()) {
        case 'A' : return '23cm';
        case 'B' : return '70cm';
        case 'C' : return '2m';
        case 'D' : return 'Dongle';
        case 'G' : return 'Gateway';
    }
    return '';
}

function NodeModules($node) {
    $ts1 = $node->GetLinkedModuleTS1();
    $ts2 = $node->GetLinkedModuleTS2();
    if ($ts1 != '' && $ts2 != '' && $ts1 != $ts2) return '<span class="mod on">'.h($ts1).'</span> <span class="muted">TS1</span> <span class="mod on">'.h($ts2).'</span> <span class="muted">TS2</span>';
    if ($ts1 != '') return '<span class="mod on">'.h($ts1).'</span> <span class="muted">TS1</span>';
    if ($ts2 != '') return '<span class="mod on">'.h($ts2).'</span> <span class="muted">TS2</span>';
    return '<span class="mod on">'.h($node->GetLinkedModule()).'</span>';
}
?>
<div class="page">
    <div>
        <h1>Links</h1>
        <p class="lede">Repeaters, hotspots and gateways linked to <?php echo h($Name); ?> right now.</p>
    </div>
    <div class="scroll">
        <table class="t">
            <thead>
                <tr><th>Station</th><th class="opt">Band</th><th>Mode</th><th>Module</th><th class="r">Last heard</th><th class="r opt">Linked for</th><?php if ($ShowIP) { ?><th class="opt">IP</th><?php } ?></tr>
            </thead>
            <tbody>
<?php
$count = min($Reflector->NodeCount(), (int)$PageOptions['RepeatersPage']['LimitTo'] + 1);
for ($i = 0; $i < $count; $i++) {
    $node = $Reflector->Nodes[$i];
    list($flag, $country) = $Reflector->GetFlag($node->GetCallSign());
    echo '<tr>';
    // like the last heard list: the callsign leads to QRZ, without the suffix
    echo '<td class="cs"><a href="https://www.qrz.com/db/'.rawurlencode(trim($node->GetCallSign())).'" target="_blank" rel="noopener">'.h($node->GetCallSign()).'</a>';
    if ($node->GetSuffix() != '') echo '<span class="sfx">-'.h($node->GetSuffix()).'</span>';
    if ($flag != '') echo '<span class="cc" title="'.h($country).'">'.h(strtoupper($flag)).'</span>';
    echo '</td>';
    echo '<td class="opt">'.h(NodeBand($node)).'</td>';
    echo '<td>'.h($node->GetProtocol()).'</td>';
    echo '<td class="nw">'.NodeModules($node).'</td>';
    echo '<td class="r" title="'.h(date("d.m.Y H:i", $node->GetLastHeardTime())).'">'.FormatAgo($node->GetLastHeardTime()).'</td>';
    echo '<td class="r opt">'.FormatSpan(time() - $node->GetConnectTime()).'</td>';
    if ($ShowIP) echo '<td class="opt ip">'.h(MaskIP($node->GetIP(), $IPMode, $MC)).'</td>';
    echo '</tr>';
}
if ($count == 0) {
    echo '<tr class="empty"><td colspan="'.($ShowIP ? 7 : 6).'">No repeater or hotspot is linked right now. Linked stations appear here within a few seconds.</td></tr>';
}
?>
            </tbody>
        </table>
    </div>
</div>
