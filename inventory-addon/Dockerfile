FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential pkg-config libssl-dev libsqlite3-dev libjson-c-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY Makefile ./
COPY src ./src
RUN make
FROM debian:bookworm-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends libssl3 libsqlite3-0 libjson-c5 ca-certificates curl tzdata && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10005 punto-inventory && useradd --uid 10005 --gid 10005 --no-create-home --shell /usr/sbin/nologin punto-inventory \
    && mkdir -p /var/lib/punto-inventory /var/backups/punto-inventory \
    && chown 10005:10005 /var/lib/punto-inventory /var/backups/punto-inventory \
    && chmod 0700 /var/backups/punto-inventory
COPY --from=build /build/build/punto-inventory /usr/local/bin/punto-inventory
USER 10005:10005
EXPOSE 8092
ENTRYPOINT ["/usr/local/bin/punto-inventory"]
