import { getDashboardConfig, getVueRuntimeUrl } from '../lib/config.js';

const { computed, reactive } = await import(getVueRuntimeUrl());
const { connectNotifier } = await import('../lib/notifier.js');
const [
  { default: LastHeardPage },
  { default: ModulesPage },
  { default: LinksPage },
  { default: PeersPage },
  { default: ReflectorListPage },
  { default: FeedPage },
  { default: ProtocolsPage },
] = await Promise.all([
  import('./LastHeardPage.js'),
  import('./ModulesPage.js'),
  import('./LinksPage.js'),
  import('./PeersPage.js'),
  import('./ReflectorListPage.js'),
  import('./FeedPage.js'),
  import('./ProtocolsPage.js'),
]);

export default {
  name: 'DashboardApp',
  components: {
    LastHeardPage,
    ModulesPage,
    ProtocolsPage,
    LinksPage,
    PeersPage,
    ReflectorListPage,
    FeedPage,
  },
  setup() {
    const dashboardConfig = getDashboardConfig();
    const nav = computed(() => [
      { key: 'lastheard', label: 'Last heard' },
      { key: 'modules', label: 'Modules' },
      { key: 'protocols', label: 'Protocols' },
      { key: 'links', label: 'Links' },
      { key: 'peers', label: 'Peers' },
      ...(dashboardConfig.reflectorListUrl ? [{ key: 'reflectors', label: 'Reflector list' }] : []),
      ...(dashboardConfig.quadnetLiveUrl ? [{ key: 'quadnet', label: 'QuadNet live' }] : []),
    ]);

    const state = reactive({
      page: 'lastheard',
      connectionStatus: 'connecting',
      reflector: {},
      modules: [],
      protocols: [],
      links: [],
      peers: [],
      lastHeard: [],
      openChannels: [],
      selectedModule: 'all',
      serverStarted: 0,
      uptimeSeconds: 0,
      currentTime: 0,
      eventLog: [],
      linkEventLog: [],
    });

    const activePage = computed(() => {
      const pageMap = {
        lastheard: 'LastHeardPage',
        modules: 'ModulesPage',
        protocols: 'ProtocolsPage',
        links: 'LinksPage',
        peers: 'PeersPage',
        reflectors: 'ReflectorListPage',
        quadnet: 'FeedPage',
      };
      return pageMap[state.page] || 'LastHeardPage';
    });

    const summary = computed(() => ({
      callsign: state.reflector.callsign || 'URF',
      country: state.reflector.country || 'Unknown',
      sponsor: state.reflector.sponsor || 'URFD dashboard',
      uptime: formatDuration(state.uptimeSeconds),
      liveLinks: state.links.length,
      liveModules: state.modules.filter((module) => module.on_air).length,
    }));

    const banner = computed(() => {
      const liveChannel = (state.openChannels || []).find((channel) => channel && (channel.user || channel.callsign));
      const liveEntry = liveChannel
        ? (state.lastHeard || []).find((item) => {
            const user = liveChannel.user || liveChannel.callsign || '';
            return user === item.callsign && (liveChannel.module === item.on_module || liveChannel.linked_module === item.on_module || liveChannel.on_module === item.on_module);
          })
        : null;

      const recent = [...(state.lastHeard || [])]
        .sort((a, b) => (b.last_heard_time || 0) - (a.last_heard_time || 0))[0];

      const item = liveEntry || recent;
      if (item) {
        return {
          isLive: Boolean(liveEntry),
          callsign: item.callsign || 'Unknown',
          module: item.on_module || '—',
          detail: liveEntry ? 'Transmitting' : formatDateTime(item.last_heard_time),
          badge: liveEntry ? 'TX' : 'Last heard',
        };
      }
      return {
        isLive: false,
        callsign: 'Nobody heard yet',
        module: '—',
        detail: 'Quiet so far',
        badge: 'Idle',
      };
    });

    function formatDuration(seconds) {
      if (!Number.isFinite(Number(seconds)) || Number(seconds) < 0) return '0s';
      const total = Number(seconds);
      const days = Math.floor(total / 86400);
      const hours = Math.floor((total % 86400) / 3600);
      const minutes = Math.floor((total % 3600) / 60);
      const secs = total % 60;
      if (days) return `${days}d ${hours}h ${minutes}m`;
      if (hours) return `${hours}h ${minutes}m`;
      if (minutes) return `${minutes}m ${secs}s`;
      return `${secs}s`;
    }

    function formatDateTime(ts) {
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
    }

    function updateModuleState(module) {
      const active = (state.openChannels || []).find((channel) => channel.module === module.name || channel.on_module === module.name);
      if (!active) {
        return { ...module, on_air: module.on_air || null };
      }
      return { ...module, on_air: active.user || active.callsign || module.on_air || null };
    }

    function applyInitialConnect(payload = {}) {
      if (payload.reflector) {
        state.reflector = { ...state.reflector, ...payload.reflector };
      }
      state.modules = Array.isArray(payload.modules) ? payload.modules.map((module) => ({ ...module })) : [];
      state.protocols = Array.isArray(payload.protocols) ? payload.protocols : [];
      if (typeof payload.server_started !== 'undefined') state.serverStarted = Number(payload.server_started);
      if (typeof payload.uptime_seconds !== 'undefined') state.uptimeSeconds = Number(payload.uptime_seconds);
      if (typeof payload.current_time !== 'undefined') state.currentTime = Number(payload.current_time);
    }

    function applySnapshot(payload = {}) {
      if (Array.isArray(payload.links)) state.links = payload.links;
      if (Array.isArray(payload.last_heard)) state.lastHeard = payload.last_heard;
      if (Array.isArray(payload.open_channels)) state.openChannels = payload.open_channels;
      if (Array.isArray(payload.peers)) state.peers = payload.peers;

      state.modules = state.modules.map((module) => {
        const active = state.openChannels.find((channel) => channel.module === module.name || channel.linked_module === module.name);
        if (active) {
          return { ...module, on_air: active.user || active.callsign || module.on_air || null };
        }
        return { ...module, on_air: module.on_air || null };
      });

      if (typeof payload.server_started !== 'undefined') state.serverStarted = Number(payload.server_started);
      if (typeof payload.uptime_seconds !== 'undefined') state.uptimeSeconds = Number(payload.uptime_seconds);
      if (typeof payload.current_time !== 'undefined') state.currentTime = Number(payload.current_time);
    }

    function logLinkActivity(type, entry) {
      const eventType = type === 'join' ? 'join' : 'leave';
      const callsign = entry.callsign || 'Unknown station';
      const protocol = entry.protocol || 'unknown';
      const module = entry.reflector_module || entry.on_module || 'unknown';
      const message = `${callsign} (${protocol}) ${eventType === 'join' ? 'joined' : 'left'} module ${module}`;
      state.linkEventLog = [{
        id: `${eventType}-${Date.now()}-${Math.random().toString(16).slice(2, 8)}`,
        type: eventType,
        message,
        timestamp: Date.now(),
      }, ...state.linkEventLog].slice(0, 12);
    }

    function handleClientAdded(entry) {
      const existing = state.links.find((item) => item.callsign === entry.callsign && item.protocol === entry.protocol && item.on_module === entry.reflector_module);
      if (!existing) {
        state.links = [...state.links, { ...entry, connect_time: entry.connect_time || Math.floor(Date.now() / 1000), on_module: entry.reflector_module }];
      }
      logLinkActivity('join', { ...entry, on_module: entry.reflector_module });
    }

    function handleClientRemoved(entry) {
      state.links = state.links.filter((item) => !(item.callsign === entry.callsign && item.protocol === entry.protocol && item.on_module === entry.reflector_module));
      logLinkActivity('leave', { ...entry, on_module: entry.reflector_module });
    }

    function handlePeerAdded(entry) {
      const existingIndex = state.peers.findIndex((peer) => peer.callsign === entry.callsign && peer.protocol === entry.protocol);
      if (existingIndex >= 0) {
        state.peers[existingIndex] = { ...state.peers[existingIndex], ...entry, clients: entry.clients || state.peers[existingIndex].clients || [] };
      } else {
        state.peers = [...state.peers, { ...entry, clients: entry.clients || [] }];
      }
    }

    function handlePeerRemoved(entry) {
      state.peers = state.peers.filter((peer) => !(peer.callsign === entry.callsign && peer.protocol === entry.protocol));
    }

    function handleStationStarted(entry) {
      const nextEntry = {
        callsign: entry.callsign,
        name: entry.name || '',
        on_module: entry.on_module,
        protocol: entry.protocol,
        via_peer: entry.via_peer,
        via_node: entry.via_node,
        last_heard_time: entry.last_heard_time || Math.floor(Date.now() / 1000),
        locator: entry.locator || '',
      };
      // the station was shown under another callsign so far (a DMR ID urfd learned from its talker alias)
      if (entry.replaces) {
        state.lastHeard = state.lastHeard.filter((item) => !(item.callsign === entry.replaces && item.on_module === entry.on_module));
      }
      const index = state.lastHeard.findIndex((item) => item.callsign === entry.callsign && item.on_module === entry.on_module);
      // urfd learns a locator only from M17 GNSS data: keep a known one
      if (!nextEntry.locator && index >= 0) nextEntry.locator = state.lastHeard[index].locator || '';
      if (index >= 0) {
        state.lastHeard[index] = { ...state.lastHeard[index], ...nextEntry };
      } else {
        state.lastHeard = [nextEntry, ...state.lastHeard];
      }

      state.openChannels = [
        ...(state.openChannels || []).filter((channel) => !(channel.user === entry.callsign && (channel.module === entry.on_module || channel.linked_module === entry.on_module))),
        {
          module: entry.on_module,
          user: entry.callsign,
          callsign: entry.callsign,
          linked_module: entry.on_module,
          protocol: entry.protocol,
        },
      ];

      const module = state.modules.find((item) => item.name === entry.on_module);
      if (module) {
        module.on_air = entry.callsign;
      }
    }

    function handleStationStopped(entry) {
      state.openChannels = (state.openChannels || []).filter((channel) => !(channel.user === entry.callsign && (channel.module === entry.on_module || channel.linked_module === entry.on_module)));
      const module = state.modules.find((item) => item.name === entry.on_module);
      if (module) {
        module.on_air = null;
      }
    }

    function handleMessage(message) {
      const payload = message && message.payload ? message.payload : {};
      state.eventLog = [{ type: message.event_type, at: message.timestamp || Date.now() / 1000 }, ...state.eventLog].slice(0, 8);

      switch (message.event_type) {
        case 'initial_connect':
          applyInitialConnect(payload);
          break;
        case 'state_snapshot':
          applySnapshot(payload);
          break;
        case 'client_added':
          handleClientAdded(payload);
          break;
        case 'client_removed':
          handleClientRemoved(payload);
          break;
        case 'peer_added':
          handlePeerAdded(payload);
          break;
        case 'peer_removed':
          handlePeerRemoved(payload);
          break;
        case 'station_started':
          handleStationStarted(payload);
          break;
        case 'station_stopped':
          handleStationStopped(payload);
          break;
        case 'module_mappings':
          // the bridges of every module, sent when one changed
          if (payload.modules) {
            state.modules = state.modules.map((module) => ({ ...module, mappings: payload.modules[module.name] || [] }));
          }
          break;
        case 'uptime_update':
          if (typeof payload.uptime_seconds !== 'undefined') state.uptimeSeconds = Number(payload.uptime_seconds);
          if (typeof payload.current_time !== 'undefined') state.currentTime = Number(payload.current_time);
          break;
        default:
          break;
      }
    }

    let socket = null;
    let reconnectTimer = null;
    let uptimeTicker = null;

    function startUptimeTicker() {
      if (uptimeTicker) {
        clearInterval(uptimeTicker);
      }

      uptimeTicker = setInterval(() => {
        if (state.serverStarted > 0 || state.uptimeSeconds > 0) {
          state.uptimeSeconds = Number(state.uptimeSeconds) + 1;
        }
      }, 1000);
    }

    function connectSocket() {
      state.connectionStatus = 'connecting';
      socket = connectNotifier({
        onOpen() {
          state.connectionStatus = 'connected';
          if (reconnectTimer) clearTimeout(reconnectTimer);
        },
        onMessage(message) {
          handleMessage(message);
        },
        onClose() {
          state.connectionStatus = 'reconnecting';
          reconnectTimer = setTimeout(() => {
            connectSocket();
          }, 3000);
        },
        onError() {
          state.connectionStatus = 'error';
        },
      });
    }

    startUptimeTicker();
    connectSocket();

    const currentComponent = computed(() => activePage.value);
    const pageTitle = computed(() => nav.find((item) => item.key === state.page)?.label || 'Dashboard');

    return {
      nav,
      state,
      summary,
      banner,
      pageTitle,
      currentComponent,
      formatDuration,
      setPage(page) {
        state.page = page;
      },
    };
  },
  template: `
    <div class="shell">
      <header class="topbar">
        <div class="brand-wrap">
          <div class="brand-mark">URF</div>
          <div>
            <div class="brand-name">{{ summary.callsign }}</div>
            <div class="brand-subtitle">{{ summary.sponsor }}</div>
          </div>
        </div>

        <nav class="main-nav">
          <button
            v-for="item in nav"
            :key="item.key"
            type="button"
            class="nav-button"
            :class="{ active: state.page === item.key }"
            @click="setPage(item.key)"
          >
            {{ item.label }}
          </button>
        </nav>

        <div class="status-meta">
          <span class="connection-dot" :class="state.connectionStatus"></span>
          <span>{{ state.connectionStatus }}</span>
        </div>
      </header>

      <div class="last-heard-banner" :class="{ live: banner.isLive }">
        <div class="last-heard-banner__left">
          <span class="banner-kicker">Last heard</span>
          <strong>{{ banner.callsign }}</strong>
          <span class="banner-meta">Module {{ banner.module }}</span>
        </div>
        <div class="last-heard-banner__right">
          <span class="banner-tag" v-if="banner.isLive">TX</span>
          <span>{{ banner.detail }}</span>
        </div>
      </div>

      <div class="hero-strip">
        <div class="metric">
          <span class="metric-label">Reflector</span>
          <strong>{{ summary.callsign }}</strong>
        </div>
        <div class="metric">
          <span class="metric-label">Country</span>
          <strong>{{ summary.country }}</strong>
        </div>
        <div class="metric">
          <span class="metric-label">Live links</span>
          <strong>{{ summary.liveLinks }}</strong>
        </div>
        <div class="metric">
          <span class="metric-label">Active modules</span>
          <strong>{{ summary.liveModules }}</strong>
        </div>
        <div class="metric">
          <span class="metric-label">Uptime</span>
          <strong>{{ summary.uptime }}</strong>
        </div>
      </div>

      <main class="content">
        <component :is="currentComponent" :state="state" />
      </main>
    </div>
  `,
};
