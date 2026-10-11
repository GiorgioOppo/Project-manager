FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential pkg-config libssl-dev libcurl4-openssl-dev libsqlite3-dev libjson-c-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY Makefile ./
COPY src ./src
RUN make
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 libcurl4 libsqlite3-0 libjson-c5 ca-certificates curl && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10004 punto-calendar && useradd --uid 10004 --gid 10004 --no-create-home --shell /usr/sbin/nologin punto-calendar \
    && mkdir -p /var/lib/punto-calendar && chown 10004:10004 /var/lib/punto-calendar
COPY --from=build /build/build/punto-calendar /usr/local/bin/punto-calendar
USER 10004:10004
EXPOSE 8091
ENTRYPOINT ["/usr/local/bin/punto-calendar"]
