export default {
  name: 'ReflectorListPage',
  props: {
    state: { type: Object, required: true },
  },
  data() {
    return {
      entries: [],
      loading: true,
      error: null,
      search: '',
      page: 1,
      perPage: 25,
    };
  },
  computed: {
    filteredEntries() {
      const query = this.search.trim().toLowerCase();
      if (!query) return this.entries;
      return this.entries.filter((entry) => {
        const haystack = `${entry.name} ${entry.country} ${entry.comment}`.toLowerCase();
        return haystack.includes(query);
      });
    },
    totalPages() {
      return Math.max(1, Math.ceil(this.filteredEntries.length / this.perPage));
    },
    paginatedEntries() {
      const start = (this.page - 1) * this.perPage;
      return this.filteredEntries.slice(start, start + this.perPage);
    },
    pageButtons() {
      const total = this.totalPages;
      if (total <= 7) {
        return Array.from({ length: total }, (_, index) => index + 1);
      }

      const pages = [];
      const left = Math.max(1, this.page - 2);
      const right = Math.min(total, this.page + 2);

      if (left > 1) {
        pages.push(1);
        if (left > 2) pages.push('…');
      }

      for (let page = left; page <= right; page += 1) {
        pages.push(page);
      }

      if (right < total) {
        if (right < total - 1) pages.push('…');
        pages.push(total);
      }

      return pages;
    },
  },
  watch: {
    search() {
      this.page = 1;
    },
  },
  created() {
    this.loadReflectors();
  },
  methods: {
    async loadReflectors() {
      this.loading = true;
      this.error = null;
      try {
        const response = await fetch('/api/reflector-list', { cache: 'no-store' });
        if (!response.ok) {
          throw new Error(`HTTP ${response.status}`);
        }
        const payload = await response.json();
        this.entries = Array.isArray(payload.reflectors) ? payload.reflectors : [];
      } catch (err) {
        this.error = err.message || 'Unable to load reflector list';
        this.entries = [];
      } finally {
        this.loading = false;
      }
    },
    isOnline(entry) {
      if (!Number.isFinite(entry.lastContact) || entry.lastContact <= 0) return false;
      return (Date.now() / 1000) - entry.lastContact <= 600;
    },
    formatUptime(seconds) {
      if (!Number.isFinite(seconds) || seconds <= 0) return '—';
      const days = Math.floor(seconds / 86400);
      const hours = Math.floor((seconds % 86400) / 3600);
      const mins = Math.floor((seconds % 3600) / 60);
      return days ? `${days}d ${hours}h ${mins}m` : `${hours}h ${mins}m`;
    },
    formatLastSeen(epoch) {
      if (!Number.isFinite(epoch) || epoch <= 0) return '—';
      const date = new Date(epoch * 1000);
      return new Intl.DateTimeFormat(undefined, {
        month: 'short',
        day: '2-digit',
        hour: '2-digit',
        minute: '2-digit',
      }).format(date);
    },
    formatAgo(epoch) {
      if (!Number.isFinite(epoch) || epoch <= 0) return 'never';
      const delta = Math.max(0, Math.floor((Date.now() / 1000) - epoch));
      if (delta < 60) return 'just now';
      const minutes = Math.floor(delta / 60);
      if (minutes < 60) return `${minutes} min ago`;
      const hours = Math.floor(minutes / 60);
      if (hours < 24) return `${hours} h ago`;
      const days = Math.floor(hours / 24);
      return `${days} d ago`;
    },
    setPage(nextPage) {
      this.page = Math.min(Math.max(1, nextPage), this.totalPages);
    },
  },
  template: `
    <section class="page">
      <div class="page-header">
        <div>
          <p class="eyebrow">Directory</p>
          <h1>Reflectors</h1>
        </div>
        <div v-if="!loading && !error" class="toolbar search-box">
          <label for="reflector-search">Search</label>
          <input id="reflector-search" v-model="search" type="search" placeholder="Name, country, comment" />
          <span class="muted">{{ filteredEntries.length }} reflectors</span>
        </div>
      </div>

      <div class="panel">
        <div v-if="loading" class="empty-state">
          <h2>Loading reflector list…</h2>
        </div>

        <div v-else-if="error" class="empty-state error-box">
          <h2>Unable to load reflector directory</h2>
          <p>{{ error }}</p>
        </div>

        <div v-else class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th>Reflector</th>
                <th>Country</th>
                <th>Comment</th>
                <th>Status</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="entry in paginatedEntries" :key="entry.name">
                <td>
                  <a v-if="entry.dashboardUrl" :href="entry.dashboardUrl" target="_blank" rel="noreferrer noopener">
                    <strong>{{ entry.name }}</strong>
                  </a>
                  <strong v-else>{{ entry.name }}</strong>
                </td>
                <td>{{ entry.country || 'Unknown' }}</td>
                <td class="muted">{{ entry.comment || '—' }}</td>
                <td>
                  <span v-if="isOnline(entry)" class="status-pill success">online</span>
                  <span v-else class="status-pill muted" :title="formatLastSeen(entry.lastContact)">seen {{ formatAgo(entry.lastContact) }}</span>
                </td>
              </tr>
            </tbody>
          </table>

          <div v-if="totalPages > 1" class="pager" aria-label="Reflector list pagination">
            <button type="button" class="pager-button" :disabled="page === 1" @click="setPage(page - 1)">Previous</button>
            <div class="pager-pages" role="navigation" aria-label="Choose reflector page">
              <button
                v-for="(item, index) in pageButtons"
                :key="typeof item === 'number' ? item : 'ellipsis-' + index"
                type="button"
                class="pager-page"
                :class="{ active: item === page }"
                :disabled="item === '…'"
                @click="typeof item === 'number' && setPage(item)"
              >
                {{ item }}
              </button>
            </div>
            <button type="button" class="pager-button" :disabled="page >= totalPages" @click="setPage(page + 1)">Next</button>
          </div>
        </div>
      </div>
    </section>
  `,
};
