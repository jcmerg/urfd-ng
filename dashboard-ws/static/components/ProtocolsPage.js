export default {
  name: 'ProtocolsPage',
  props: {
    state: { type: Object, required: true },
  },
  computed: {
    rows() {
      return [...(this.state.protocols || [])].sort((a, b) => (a.name || '').localeCompare(b.name || ''));
    },
  },
  methods: {
    prettyPort(protocol) {
      if (protocol && protocol.port !== undefined && protocol.port !== null && protocol.port !== '') {
        return Number(protocol.port);
      }
      return '—';
    },
  },
  template: `
    <section class="page">
      <div class="page-header">
        <div>
          <p class="eyebrow">Network services</p>
          <h1>Protocols</h1>
        </div>
      </div>

      <div class="panel">
        <div class="table-wrap">
          <table class="data-table compact">
            <thead>
              <tr>
                <th>Protocol</th>
                <th>Port</th>
              </tr>
            </thead>
            <tbody>
              <tr v-if="rows.length === 0">
                <td colspan="2" class="empty">No enabled protocols are available yet.</td>
              </tr>
              <tr v-for="protocol in rows" :key="protocol.name + ':' + protocol.port">
                <td>{{ protocol.name || 'unknown' }}</td>
                <td>{{ prettyPort(protocol) }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </section>
  `,
};
