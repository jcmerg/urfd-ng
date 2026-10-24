export default {
  name: 'FeedPage',
  props: {
    state: { type: Object, required: true },
  },
  template: `
    <section class="page">
      <div class="page-header">
        <div>
          <p class="eyebrow">Router feed</p>
          <h1>QuadNet live</h1>
        </div>
      </div>

      <div class="panel">
        <div class="empty-state">
          <h2>QuadNet traffic is not exposed by the WebSocket notifier.</h2>
          <p>
            The old dashboard fetched the QuadNet feed through a proxy endpoint. This websocket dashboard focuses on reflector state,
            link health and live station activity delivered directly by the URFD notifier.
          </p>
        </div>
      </div>
    </section>
  `,
};
