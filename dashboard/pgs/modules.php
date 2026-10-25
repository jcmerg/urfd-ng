<?php
// Modules: what each module carries, who is linked to it and how other networks reach it

$Protocols = DashboardProtocols($Reflector);
$protoNames = array_column($Protocols, 'name');
$showDmrPlus = in_array('DMRPlus', $protoNames);
$showYsfDgid = in_array('YSF', $protoNames);
$showNxdnRan = in_array('NXDN', $protoNames);
$showNxdnDstId = $showNxdnRan && count(array_filter(array_column($Modules, 'nxdndstid'), 'strlen')) > 0;
$cols = 5 + ($showDmrPlus ? 1 : 0) + ($showYsfDgid ? 1 : 0) + ($showNxdnRan ? 1 : 0) + ($showNxdnDstId ? 1 : 0);

// linked nodes per module (dual-slot MMDVM clients count in both modules)
$LiveNodes = array();
for ($i = 0; $i < $Reflector->NodeCount(); $i++) {
    foreach ($Reflector->Nodes[$i]->GetAllLinkedModules() as $m) $LiveNodes[$m][] = $Reflector->Nodes[$i];
}

function Missing() { return '<span class="muted">none</span>'; }

function MappingLabel($map) {
    $parts = array($map['protocol']);
    if ($map['type'] == 'TG')       $parts[] = 'TG '.$map['id'];
    if ($map['type'] == 'RAN')      $parts[] = 'RAN '.$map['id'];
    if ($map['type'] == 'AutoLink') $parts[] = 'autolink';
    if ($map['type'] == 'Bridge')   $parts[] = 'bridge';
    if ($map['id'] != '' && $map['type'] != 'TG' && $map['type'] != 'RAN') $parts[] = '#'.$map['id'];
    if ($map['timeslot'] != '')     $parts[] = $map['timeslot'];
    if ($map['remoteName'] != '')   $parts[] = $map['remoteName'];
    $title = $map['dynamic'] ? ' title="Added at runtime, ends in '.FormatSpan($map['remaining']).'"' : '';
    return '<span class="tag'.($map['dynamic'] ? ' dyn' : '').'"'.$title.'>'.h(implode(' ', $parts)).'</span>';
}
?>
<div class="page">
    <div>
        <h1>Modules</h1>
        <p class="lede">What each module carries, who is linked to it and how the other networks reach it. Dashed bridges were added at runtime and run out on their own.</p>
    </div>

    <div class="scroll">
        <table class="t">
            <thead>
                <tr>
                    <th>Module</th>
                    <th>Name</th>
                    <th class="r">Nodes</th>
                    <?php if ($showDmrPlus) { ?><th>DMR+ TG</th><?php } ?>
                    <?php if ($showYsfDgid) { ?><th>YSF DG-ID</th><?php } ?>
                    <?php if ($showNxdnRan) { ?><th>NXDN RAN</th><?php } ?>
                    <?php if ($showNxdnDstId) { ?><th>NXDN DST-ID</th><?php } ?>
                    <th>Transcoded</th>
                    <th>Bridged to</th>
                </tr>
            </thead>
            <tbody>
<?php
foreach ($Modules as $mod => $info) {
    $nodes = isset($LiveNodes[$mod]) ? $LiveNodes[$mod] : array();
    $tx = ($info['onair'] != '');
    echo '<tr>';
    echo '<td class="nw"><span class="mod'.(count($nodes) ? ' on' : '').'">'.h($mod).'</span>'.($tx ? ' <span class="txtag" title="'.h($info['onair']).' is transmitting">TX</span>' : '').'</td>';
    echo '<td>'.($info['name'] != '' ? h($info['name']) : Missing()).($info['echo'] ? ' <span class="muted">(echo test)</span>' : '').'</td>';
    echo '<td class="r">'.max((int)$info['nodes'], count($nodes)).'</td>';
    if ($showDmrPlus) echo '<td>'.($info['dmrplus'] != '' ? h($info['dmrplus']) : Missing()).'</td>';
    if ($showYsfDgid) echo '<td>'.($info['ysfdgid'] != '' ? h($info['ysfdgid']) : Missing()).'</td>';
    if ($showNxdnRan) echo '<td>'.($info['nxdnran'] != '' ? h($info['nxdnran']) : Missing()).'</td>';
    if ($showNxdnDstId) echo '<td>'.($info['nxdndstid'] != '' ? h($info['nxdndstid']) : Missing()).'</td>';
    echo '<td>'.($info['transcoded'] ? 'Yes' : '<span class="muted">No</span>').'</td>';
    echo '<td>'.(count($info['mappings']) ? implode('', array_map('MappingLabel', $info['mappings'])) : Missing()).'</td>';
    echo '</tr>';
    if (count($nodes)) {
        echo '<tr><td></td><td colspan="'.($cols - 1).'" style="padding-top:0"><details class="nodes" id="nodes-'.h($mod).'"><summary>'
            .'Show linked stations</summary><ul>';
        foreach ($nodes as $node) {
            $name = $node->GetCallSign().($node->GetSuffix() != '' ? '-'.$node->GetSuffix() : '');
            echo '<li>'.h($name).'<span>'.h($node->GetProtocol()).'</span></li>';
        }
        echo '</ul></details></td></tr>';
    }
}
if (count($Modules) == 0) {
    echo '<tr class="empty"><td colspan="'.$cols.'">The reflector reports no modules. Check that urfd is running and writes its status file.</td></tr>';
}
?>
            </tbody>
        </table>
    </div>

<?php
// client protocols connect out to other reflectors, nobody connects to them here
$hiddenProtos = array('MMDVMClient', 'DCSClient', 'DExtraClient', 'DPlusClient', 'YSFClient', 'SvxReflector');
$protoTypes = array(
    'DExtra' => 'D-Star', 'DPlus' => 'D-Star', 'DCS' => 'D-Star', 'G3' => 'D-Star (Icom G3)',
    'MMDVM' => 'DMR hotspots and repeaters', 'DMRPlus' => 'DMR+ masters', 'XLXPeer' => 'XLX and BrandMeister peering',
    'M17' => 'M17', 'YSF' => 'YSF and Wires-X', 'P25' => 'P25', 'NXDN' => 'NXDN',
    'URF' => 'URF peer reflectors', 'USRP' => 'AllStar (USRP)', 'SVX' => 'SvxLink nodes',
);
// MMDVMClient2 ... MMDVMClient9 and MMDVMClient.<name> are client connections as well
$shown = array_filter($Protocols, function ($p) use ($hiddenProtos) { return !in_array($p['name'], $hiddenProtos) && !preg_match('/^MMDVMClient([2-9]|\..+)$/', $p['name']); });
if (count($shown) > 0) {
?>
    <section>
        <div class="tbl-head"><h2>Protocols</h2><p class="hint">Ports a hotspot, repeater or gateway connects to</p></div>
        <div class="scroll">
            <table class="t narrow">
                <thead><tr><th>Protocol</th><th class="r">Port</th><th>Used by</th></tr></thead>
                <tbody>
<?php
    foreach ($shown as $p) {
        echo '<tr><td>'.h($p['name']).'</td><td class="r">'.h($p['port']).'</td><td class="muted">'.h($protoTypes[$p['name']] ?? ($p['description'] !== '' ? $p['description'] : 'Other')).'</td></tr>';
    }
?>
                </tbody>
            </table>
        </div>
    </section>
<?php } ?>
</div>
