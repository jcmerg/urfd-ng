import { getVueRuntimeUrl } from './lib/config.js';

const { createApp } = await import(getVueRuntimeUrl());
const App = (await import('./components/App.js')).default;

createApp(App).mount('#app');
