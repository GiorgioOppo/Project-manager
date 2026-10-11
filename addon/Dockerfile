FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential libssl-dev libcurl4-openssl-dev libsqlite3-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY Makefile ./
COPY src ./src
RUN make

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 libcurl4 libsqlite3-0 ca-certificates curl && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10003 punto-mailer && useradd --uid 10003 --gid 10003 --no-create-home --shell /usr/sbin/nologin punto-mailer \
    && mkdir -p /var/lib/punto-mailer && chown 10003:10003 /var/lib/punto-mailer
COPY --from=build /build/build/punto-mailer /usr/local/bin/punto-mailer
USER 10003:10003
EXPOSE 8090
ENTRYPOINT ["/usr/local/bin/punto-mailer"]
