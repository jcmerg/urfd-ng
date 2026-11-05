## ----- Build Stage -----
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    g++ \
    make \
    libcurl4-gnutls-dev \
    libopendht-dev \
    libwebsockets-dev \
    nlohmann-json3-dev \
    libopus-dev \
    libssl-dev \
    pkg-config \
    libsqlite3-dev \
    libmariadb-dev \
    libpq-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY reflector/ ./reflector/
COPY config/urfd.mk ./reflector/urfd.mk
COPY plugins/ ./plugins/

# DHT=false builds urfd without OpenDHT (URFD_DHT=false for docker compose)
ARG DHT=true
WORKDIR /build/reflector
RUN make clean 2>/dev/null; make -j$(nproc) DHT=$DHT

# the plugins in plugins/<name>/, each with a Makefile taking URFD= (see Plugins
# in the README)
RUN make plugins-clean >/dev/null 2>&1 || true; \
    make -j$(nproc) plugins-install PLUGINDIR=/build/plugin-lib

## ----- Runtime Stage -----
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl3t64-gnutls \
    libopendht3t64 \
    libopus0 \
    libwebsockets19t64 \
    libsqlite3-0 \
    libmariadb3 \
    libpq5 \
    nginx \
    supervisor \
    curl \
    ca-certificates \
    procps \
    && apt-get upgrade -y --no-install-recommends \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /var/log/supervisor /var/cache/nginx /usr/local/etc/urfd

# Copy urfd binary and tools
COPY --from=builder /build/reflector/urfd /usr/local/bin/urfd
COPY --from=builder /build/reflector/inicheck /usr/local/bin/inicheck
COPY --from=builder /build/reflector/dbutil /usr/local/bin/dbutil
# plugins, loaded with [Plugins] Load = /usr/local/lib/urfd/<name>.so
COPY --from=builder /build/plugin-lib/ /usr/local/lib/urfd/

# the dashboard: static files, the state comes from urfd's WebSocket
COPY dashboard/ /var/www/urfd-dashboard/

# Supervisord and nginx configs are injected at build time via --build-context
COPY --from=optfiles supervisord.conf /etc/supervisor/conf.d/urfd.conf
COPY --from=optfiles nginx-dashboard.conf /etc/nginx/sites-available/urfd-dashboard.conf
RUN rm -f /etc/nginx/sites-enabled/default \
    && ln -sf /etc/nginx/sites-available/urfd-dashboard.conf /etc/nginx/sites-enabled/urfd-dashboard.conf

HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD pgrep urfd > /dev/null && curl -sf http://127.0.0.1:8363/ > /dev/null

CMD ["/usr/bin/supervisord", "-n", "-c", "/etc/supervisor/conf.d/urfd.conf"]
