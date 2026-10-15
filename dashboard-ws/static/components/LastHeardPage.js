export default {
  name: 'LastHeardPage',
  props: {
    state: { type: Object, required: true },
  },
  computed: {
    moduleOptions() {
      const options = new Set();
      (this.state.modules || []).forEach((module) => {
        if (module && module.name) options.add(module.name);
      });
      (this.state.lastHeard || []).forEach((item) => {
        if (item && item.on_module) options.add(item.on_module);
      });
      (this.state.openChannels || []).forEach((channel) => {
        if (channel && channel.module) options.add(channel.module);
        if (channel && channel.linked_module) options.add(channel.linked_module);
        if (channel && channel.on_module) options.add(channel.on_module);
      });
      return [...options].sort((a, b) => a.localeCompare(b));
    },
    rows() {
      return [...(this.state.lastHeard || [])]
        .filter((item) => {
          if (!this.state.selectedModule || this.state.selectedModule === 'all') return true;
          return (item.on_module || '') === this.state.selectedModule;
        })
        .sort((a, b) => (b.last_heard_time || 0) - (a.last_heard_time || 0))
        .slice(0, 200);
    },
  },
  methods: {
    formatDateTime(ts) {
      if (!ts) return '—';
      const date = new Date(Number(ts) * 1000);
      if (Number.isNaN(date.getTime())) return '—';
      return new Intl.DateTimeFormat(undefined, {
        year: 'numeric',
        month: 'short',
        day: '2-digit',
        hour: '2-digit',
        minute: '2-digit',
        second: '2-digit',
      }).format(date);
    },
    formatLocator(item) {
      return item.locator || '—';
    },
    viaLabel(item) {
      return item.via_peer || item.via_node || 'Direct';
    },
    isActive(item) {
      const channels = this.state.openChannels || [];
      return channels.some((channel) => {
        const user = channel.user || channel.callsign || '';
        return user === item.callsign && (channel.module === item.on_module || channel.linked_module === item.on_module);
      });
    },
  },
  template: `
    <section class="page">
      <div class="page-header">
        <div>
          <p class="eyebrow">Live feed</p>
          <h1>Last heard</h1>
        </div>
        <div class="status-pill" :class="state.connectionStatus === 'connected' ? 'ok' : 'warn'">
          {{ state.connectionStatus === 'connected' ? 'WebSocket live' : state.connectionStatus }}
        </div>
      </div>

      <div class="panel">
        <div class="module-filter-bar" v-if="moduleOptions.length">
          <button
            type="button"
            class="module-filter-btn"
            :class="{ active: !state.selectedModule || state.selectedModule === 'all' }"
            @click="state.selectedModule = 'all'"
          >
            All modules
          </button>
          <button
            v-for="module in moduleOptions"
            :key="module"
            type="button"
            class="module-filter-btn"
            :class="{ active: state.selectedModule === module }"
            @click="state.selectedModule = module"
          >
            {{ module }}
          </button>
        </div>

        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th>State</th>
                <th>Callsign</th>
                <th>Module</th>
                <th>Mode</th>
                <th>Via</th>
                <th>Position</th>
                <th class="align-right">Heard</th>
              </tr>
            </thead>
            <tbody>
              <tr v-if="rows.length === 0">
                <td colspan="7" class="empty">No activity yet. As soon as a station keys up, it will appear here.</td>
              </tr>
              <tr v-for="item in rows" :key="item.callsign + ':' + item.on_module + ':' + item.last_heard_time">
                <td><span class="tag live" v-if="isActive(item)">TX</span></td>
                <td class="callsign">{{ item.callsign }}</td>
                <td><span class="module-chip">{{ item.on_module || '—' }}</span></td>
                <td>{{ item.protocol || 'unknown' }}</td>
                <td>{{ viaLabel(item) }}</td>
                <td>{{ formatLocator(item) }}</td>
                <td class="align-right">{{ formatDateTime(item.last_heard_time) }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </section>
  `,
};
