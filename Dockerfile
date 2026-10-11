FROM debian:bookworm-slim AS builder

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        pkg-config \
        libmariadb-dev \
        libmongoc-dev \
        libssl-dev \
        libcurl4-openssl-dev \
        libjson-c-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY Makefile ./
COPY src/ ./src/
COPY auth/oidc.c auth/oidc.h ./auth/
RUN make DB_BACKEND=mysql

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libmariadb3 \
        libmongoc-1.0-0 \
        libssl3 \
        libcurl4 \
        libjson-c5 \
        ca-certificates \
        curl \
        tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --user-group --no-create-home \
        --home-dir /nonexistent --shell /usr/sbin/nologin punto

WORKDIR /app
COPY --from=builder --chown=10001:10001 /build/build/punto ./punto
COPY --chown=10001:10001 public/ ./public/

ENV PUBLIC_DIR=/app/public \
    PORT=8080 \
    BIND_HOST=0.0.0.0 \
    TZ=Europe/Rome

USER 10001:10001
EXPOSE 8080
STOPSIGNAL SIGTERM

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD curl --noproxy '*' --fail --silent --show-error --max-time 3 --header "Host: localhost:${PORT}" "http://${BIND_HOST:-127.0.0.1}:${PORT}/healthz" || exit 1

CMD ["./punto"]
