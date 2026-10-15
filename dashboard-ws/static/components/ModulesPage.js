export default {
  name: 'ModulesPage',
  props: {
    state: { type: Object, required: true },
  },
  computed: {
    rows() {
      return [...(this.state.modules || [])].sort((a, b) => (a.name || '').localeCompare(b.name || ''));
    },
  },
  methods: {
    moduleBadge(module) {
      return module.on_air ? 'TX' : (module.linked_nodes ? 'Linked' : 'Idle');
    },
    valueOrDash(value) {
      if (value === null || value === undefined || value === '') return '—';
      if (Array.isArray(value)) return value.length ? value.join(', ') : '—';
      return value;
    },
    boolText(value) {
      return value ? 'Yes' : 'No';
    },
    // one urfd mapping as text, e.g. "MMDVMClient TG 26363 TS1" or "DCSClient C host:port (down)"
    mappingText(mapping) {
      if (!mapping || typeof mapping !== 'object') return String(mapping);
      const parts = [mapping.protocol, mapping.type === 'TG' ? 'TG' : '', mapping.id, mapping.timeslot ? 'TS' + mapping.timeslot : '', mapping.remote_name];
      let text = parts.filter((part) => part !== undefined && part !== null && part !== '').join(' ');
      if (mapping.expires) text += ' (dynamic)';
      if (mapping.connected === false) text += ' (down)';
      return text;
    },
    bridgeText(module) {
      const bridged = module.bridged_to || module.bridgedTo || module.mappings || module.bridge || '—';
      if (Array.isArray(bridged)) return bridged.length ? bridged.map((mapping) => this.mappingText(mapping)).join(', ') : '—';
      if (bridged && typeof bridged === 'object') {
        const names = Object.keys(bridged).map((key) => bridged[key]);
        return names.length ? names.join(', ') : '—';
      }
      return bridged || '—';
    },
  },
  template: `
    <section class="page">
      <div class="page-header">
        <div>
          <p class="eyebrow">Reflector health</p>
          <h1>Modules</h1>
        </div>
      </div>

      <div class="panel">
        <div class="table-wrap">
          <table class="data-table compact">
            <thead>
              <tr>
                <th>Module</th>
                <th>Name</th>
                <th>Nodes</th>
                <th>DMR+ TG</th>
                <th>YSF DG-ID</th>
                <th>NXDN RAN</th>
                <th>NXDN DST-ID</th>
                <th>Transcoded</th>
                <th>Bridged to</th>
                <th>On air</th>
              </tr>
            </thead>
            <tbody>
              <tr v-if="rows.length === 0">
                <td colspan="10" class="empty">No module metadata is available yet.</td>
              </tr>
              <tr v-for="module in rows" :key="module.name">
                <td><span class="module-chip">{{ module.name }}</span></td>
                <td>{{ valueOrDash(module.description) }}</td>
                <td>{{ module.linked_nodes || 0 }}</td>
                <td>{{ valueOrDash(module.DMRplus) }}</td>
                <td>{{ valueOrDash(module.YSFDGID) }}</td>
                <td>{{ valueOrDash(module.NXDNRAN) }}</td>
                <td>{{ valueOrDash(module.NXDNDstId) }}</td>
                <td>{{ boolText(module.transcoded) }}</td>
                <td>{{ bridgeText(module) }}</td>
                <td>
                  <span class="tag live" v-if="module.on_air">{{ module.on_air }}</span>
                  <span class="muted" v-else>Idle</span>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </section>
  `,
};
