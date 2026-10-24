<?php
// QuadNet live: D-Star routing traffic, parsed from the OpenQuad ICS data stream
?>
<div class="page">
   <div class="tbl-head">
      <div>
         <h1>QuadNet live</h1>
         <p class="lede">D-Star routing traffic across the QuadNet servers, newest first.</p>
      </div>
      <div class="filters">
         <label for="quadnet-search">Search</label>
         <input class="field" type="search" id="quadnet-search" placeholder="Callsign, name or reflector" autocomplete="off">
         <span class="hint" id="quadnet-status"></span>
      </div>
   </div>
   <div class="scroll">
      <table class="t" id="quadnet-table">
         <thead>
         <tr>
            <th>Time</th>
            <th>Callsign</th>
            <th class="opt">Radio</th>
            <th>Target</th>
            <th>Repeater</th>
            <th class="opt">Name</th>
            <th>Reflector</th>
            <th class="r opt">Duration</th>
         </tr>
         </thead>
         <tbody id="quadnet-body">
         <tr class="empty"><td colspan="8">Loading the QuadNet feed</td></tr>
         </tbody>
      </table>
   </div>
</div>

<script>
(function() {
   var body = document.getElementById('quadnet-body');
   var search = document.getElementById('quadnet-search');
   var status = document.getElementById('quadnet-status');
   var entries = [];

   function esc(v) {
      return String(v).replace(/[&<>"]/g, function (c) { return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]; });
   }
   function cleanCall(s) {
      // Preserve portable/mobile suffixes: _P -> /P, _M -> /M, _AM -> /AM etc.
      var m = s.match(/^(.+?)_(P|M|AM|MM|QRP)$/);
      if (m) return m[1].replace(/_/g, '') + '/' + m[2];
      return s.replace(/_/g, '');
   }

   function cleanModule(s) {
      // e.g. W2ECR__B -> W2ECR-B, DCS237_A -> DCS237-A
      return s.replace(/_/g, '').replace(/([A-Z0-9])([A-Z])$/, '$1-$2');
   }

   function cleanName(s) {
      return s.replace(/_/g, ' ').trim();
   }

   function parseLine(line) {
      var m = line.match(/^(\d{4}-\d{2}-\d{2})\s+(\d{2}:\d{2}:\d{2})\s+([\d.]+)s:\s*(\d+)%:\s*([\d.]+)%\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(.*?)\s+(\S+)\s*$/);
      if (!m) return null;
      var cr = m[6].split('/');
      return {
         time: m[2],
         duration: m[3] + 's',
         call: cleanCall(cr[0] || ''),
         radio: (cr[1] || '').replace(/_/g, ''),
         target: cleanCall(m[7]),
         repeater: cleanModule(m[8]),
         name: cleanName(m[10]),
         reflector: cleanModule(m[11])
      };
   }

   function render() {
      var q = search.value.toLowerCase();
      var html = '';
      var shown = 0;
      for (var i = entries.length - 1; i >= 0; i--) {
         var e = entries[i];
         if (q) {
            var text = [e.call, e.radio, e.target, e.repeater, e.name, e.reflector].join(' ').toLowerCase();
            if (text.indexOf(q) === -1) continue;
         }
         html += '<tr>' +
            '<td class="nw">' + esc(e.time) + '</td>' +
            '<td class="cs"><b>' + esc(e.call) + '</b></td>' +
            '<td class="opt">' + esc(e.radio) + '</td>' +
            '<td>' + esc(e.target) + '</td>' +
            '<td>' + esc(e.repeater) + '</td>' +
            '<td class="opt muted">' + esc(e.name) + '</td>' +
            '<td>' + esc(e.reflector) + '</td>' +
            '<td class="r opt">' + esc(e.duration) + '</td>' +
            '</tr>';
         shown++;
      }
      if (!html) html = '<tr class="empty"><td colspan="8">' + (q ? 'Nothing in the feed matches this search.' : 'The feed has no traffic yet.') + '</td></tr>';
      body.innerHTML = html;
      status.textContent = shown + (shown === 1 ? ' entry' : ' entries');
   }

   function poll() {
      fetch('./pgs/quadnet_proxy.php', { credentials: 'same-origin' }).then(function (r) { return r.ok ? r.text() : ''; }).then(function (data) {
         var lines = data.split('\n');
         var parsed = [];
         for (var i = 0; i < lines.length; i++) {
            var e = parseLine(lines[i]);
            if (e) parsed.push(e);
         }
         entries = parsed;
         render();
      }).catch(function () {
         status.textContent = 'Feed not reachable, retrying';
      }).then(function () {
         setTimeout(poll, 3000);
      });
   }

   search.addEventListener('input', render);
   poll();
})();
</script>
