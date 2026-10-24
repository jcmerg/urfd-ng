import { getDashboardConfig } from './config.js';

export function buildSocketUrl() {
  const config = getDashboardConfig();
  const hasConfig = !!(config && config.hasConfig);

  if (hasConfig) {
    if (config.wsUrl) return config.wsUrl;
  }

  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const host = window.location.hostname || 'localhost';
  return `${protocol}//${host}:8080`;
}

export function connectNotifier({ onOpen, onMessage, onClose, onError }) {
  const socket = new WebSocket(buildSocketUrl());

  socket.addEventListener('open', () => {
    if (typeof onOpen === 'function') onOpen();
  });

  socket.addEventListener('message', (event) => {
    try {
      const message = JSON.parse(event.data);
      if (typeof onMessage === 'function') onMessage(message);
    } catch (error) {
      console.warn('Failed to parse websocket payload', error, event.data);
    }
  });

  socket.addEventListener('close', () => {
    if (typeof onClose === 'function') onClose();
  });

  socket.addEventListener('error', () => {
    if (typeof onError === 'function') onError();
  });

  return socket;
}
