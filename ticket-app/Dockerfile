FROM debian:bookworm-slim AS builder
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential pkg-config libssl-dev libsqlite3-dev libjson-c-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY Makefile ./
COPY src/ ./src/
RUN make

FROM debian:bookworm-slim AS runtime
RUN apt-get update \
    && apt-get install -y --no-install-recommends libssl3 libsqlite3-0 libjson-c5 curl \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10006 punto-tickets \
    && useradd --uid 10006 --gid 10006 --no-create-home --shell /usr/sbin/nologin punto-tickets \
    && mkdir -p /var/lib/punto-tickets \
    && chown 10006:10006 /var/lib/punto-tickets
WORKDIR /app
COPY --from=builder /build/build/punto-tickets /usr/local/bin/punto-tickets
COPY public/ ./public/
ENV TICKETS_BIND=0.0.0.0 \
    TICKETS_PORT=8083 \
    TICKETS_DB_PATH=/var/lib/punto-tickets/tickets.db \
    TICKETS_PUBLIC_DIR=/app/public
USER 10006:10006
EXPOSE 8083
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/punto-tickets"]
