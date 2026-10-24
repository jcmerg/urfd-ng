<?php
// Reflector list: the XLX directory, searchable, 25 per page

$Reflectors = GetReflectorList();
?>
<div class="page">
    <div class="tbl-head">
        <div>
            <h1>Reflector list</h1>
            <p class="lede">Reflectors registered in the XLX directory. Open a dashboard to see who is on there.</p>
        </div>
<?php if ($Reflectors !== false) { ?>
        <div class="filters">
            <label for="f-refl">Search</label>
            <input class="field" id="f-refl" type="search" placeholder="Name, country, comment" autocomplete="off">
            <span class="hint" id="refl-count"></span>
        </div>
<?php } ?>
    </div>
<?php
if ($Reflectors === false) {
    echo DirectoryUnavailableNotice();
    echo '</div>';
    return;
}
$XML = new ParseXML();
?>
    <div>
        <div class="scroll">
            <table class="t" id="refl">
                <thead><tr><th>Reflector</th><th>Country</th><th class="opt">Comment</th><th class="r">Directory</th></tr></thead>
                <tbody>
<?php
foreach ($Reflectors as $r) {
    // every reflector in the world writes these, so they are escaped
    $name    = DirectoryText($XML->GetElement($r, "name"));
    $country = DirectoryText($XML->GetElement($r, "country"));
    $comment = DirectoryText($XML->GetElement($r, "comment"));
    $last    = (int)$XML->GetElement($r, "lastcontact");
    $url     = trim(html_entity_decode((string)$XML->GetElement($r, "dashboardurl"), ENT_QUOTES | ENT_HTML5, 'UTF-8'));
    $online  = ($last >= time() - 600);

    echo '<tr>';
    if (preg_match('#^https?://#i', $url)) {
        echo '<td class="cs"><a href="'.h($url).'" target="_blank" rel="noopener noreferrer" title="Open the dashboard of '.$name.'">'.$name.'</a></td>';
    } else {
        echo '<td class="cs"><b>'.$name.'</b></td>';
    }
    echo '<td>'.$country.'</td><td class="opt muted">'.$comment.'</td>';
    echo '<td class="r">'.($online ? '<span class="state-ok">online</span>' : '<span class="muted" title="'.h(date("d.m.Y H:i", $last)).'">seen '.FormatAgo($last).'</span>').'</td>';
    echo '</tr>';
}
?>
                    <tr class="empty" id="refl-none" hidden><td colspan="4">No reflector matches this search. Try a country or a number.</td></tr>
                </tbody>
            </table>
        </div>
        <nav class="pager" id="refl-pager" aria-label="Pages of the reflector list"></nav>
    </div>
</div>

<script>
(function () {
    var perPage = 25, page = 1;
    var rows = Array.prototype.slice.call(document.querySelectorAll('#refl tbody tr:not(#refl-none)'));
    var input = document.getElementById('f-refl'), pager = document.getElementById('refl-pager');

    function render() {
        var q = input.value.trim().toLowerCase();
        var hits = rows.filter(function (tr) { return !q || tr.textContent.toLowerCase().indexOf(q) >= 0; });
        var pages = Math.max(1, Math.ceil(hits.length / perPage));
        if (page > pages) page = pages;
        rows.forEach(function (tr) { tr.hidden = true; });
        hits.slice((page - 1) * perPage, page * perPage).forEach(function (tr) { tr.hidden = false; });
        document.getElementById('refl-none').hidden = hits.length > 0;
        document.getElementById('refl-count').textContent = hits.length + (hits.length === 1 ? ' reflector' : ' reflectors');

        pager.innerHTML = '';
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
    input.addEventListener('input', function () { page = 1; render(); });
    render();
})();
</script>
