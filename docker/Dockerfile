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

WORKDIR /build/reflector
RUN make clean 2>/dev/null; make -j$(nproc)

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
    php-fpm \
    supervisor \
    curl \
    ca-certificates \
    && apt-get upgrade -y --no-install-recommends \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /var/run/php /var/log/supervisor /var/lib/xlxd-ch /usr/local/etc/urfd

# Refuse to build with a PHP open to php2shell (CVE-2026-109618/-109619, RCE via
# use-after-free after an out-of-memory fatal error), fixed in Ubuntu's
# 8.3.6-0ubuntu0.24.04.13. Raising the floor also invalidates a cached apt layer.
RUN set -eux; \
    v="$(dpkg-query -W -f='${Version}' php8.3-fpm)"; \
    dpkg --compare-versions "$v" ge 8.3.6-0ubuntu0.24.04.13 \
        || { echo "php8.3-fpm $v lacks the php2shell fix" >&2; exit 1; }

# The stock pool allows 5 workers, so a handful of slow requests starves the whole
# dashboard — including the admin API. Cap outbound waits too, so a dead upstream
# can never hold a worker for the 60s default.
RUN set -eux; \
    pool="$(ls /etc/php/*/fpm/pool.d/www.conf)"; \
    sed -i 's/^pm.max_children = .*/pm.max_children = 25/; \
            s/^pm.start_servers = .*/pm.start_servers = 4/; \
            s/^pm.min_spare_servers = .*/pm.min_spare_servers = 2/; \
            s/^pm.max_spare_servers = .*/pm.max_spare_servers = 8/' "$pool"; \
    ini="$(ls /etc/php/*/fpm/php.ini)"; \
    sed -i 's/^;\?default_socket_timeout = .*/default_socket_timeout = 10/' "$ini"; \
    sed -i 's/^;\?session.upload_progress.enabled = .*/session.upload_progress.enabled = Off/' "$ini"

# Copy urfd binary and tools
COPY --from=builder /build/reflector/urfd /usr/local/bin/urfd
COPY --from=builder /build/reflector/inicheck /usr/local/bin/inicheck
COPY --from=builder /build/reflector/dbutil /usr/local/bin/dbutil

# Copy dashboard
COPY dashboard/ /var/www/urfd-dashboard/
# live dashboard (coolacid), served as /live/
COPY dashboard-ws/static/ /var/www/urfd-dashboard-ws/
RUN chown -R www-data:www-data /var/www/urfd-dashboard /var/lib/xlxd-ch

# Supervisord and nginx configs are injected at build time via --build-context
COPY --from=optfiles supervisord.conf /etc/supervisor/conf.d/urfd.conf
COPY --from=optfiles nginx-dashboard.conf /etc/nginx/sites-available/urfd-dashboard.conf
RUN rm -f /etc/nginx/sites-enabled/default \
    && ln -sf /etc/nginx/sites-available/urfd-dashboard.conf /etc/nginx/sites-enabled/urfd-dashboard.conf

# Ensure XML file exists for dashboard
RUN touch /var/log/xlxd.xml && chown root:root /var/log/xlxd.xml

HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD pgrep urfd > /dev/null && curl -sf http://127.0.0.1:8363/ > /dev/null

CMD ["/usr/bin/supervisord", "-n", "-c", "/etc/supervisor/conf.d/urfd.conf"]
