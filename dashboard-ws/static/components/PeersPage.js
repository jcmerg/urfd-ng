export default {
  name: 'PeersPage',
  props: {
    state: { type: Object, required: true },
  },
  computed: {
    rows() {
      return [...(this.state.peers || [])].sort((a, b) => (a.callsign || '').localeCompare(b.callsign || ''));
    },
  },
  methods: {
    sharedModules(peer) {
      if (!Array.isArray(peer.clients) || peer.clients.length === 0) return '—';
      return peer.clients
        .map((client) => client.reflector_module || client.on_module || '—')
        .filter(Boolean)
        .join(', ');
    },
  },
  template: `
    <section class="page">
      <div class="page-header">
        <div>
          <p class="eyebrow">Interlink</p>
          <h1>Peers</h1>
        </div>
      </div>

      <div class="panel">
        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th>Reflector</th>
                <th>Protocol</th>
                <th>Shared modules</th>
              </tr>
            </thead>
            <tbody>
              <tr v-if="rows.length === 0">
                <td colspan="3" class="empty">No peer reflectors are currently linked.</td>
              </tr>
              <tr v-for="peer in rows" :key="peer.callsign + ':' + peer.protocol">
                <td class="callsign">{{ peer.callsign }}</td>
                <td>{{ peer.protocol || 'unknown' }}</td>
                <td>{{ sharedModules(peer) }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </section>
  `,
};
