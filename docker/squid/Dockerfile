FROM debian:trixie-slim

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        squid ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10002 --user-group --no-create-home \
        --home-dir /nonexistent --shell /usr/sbin/nologin squid-egress

COPY squid.conf allowed-domains.txt allowed-smtp-domains.txt denied-destinations.txt /etc/squid/
COPY proxy-health /usr/local/bin/proxy-health
RUN chmod 0555 /usr/local/bin/proxy-health \
    && squid -k parse -f /etc/squid/squid.conf

USER 10002:10002
STOPSIGNAL SIGTERM
CMD ["squid", "-N", "-d", "1", "-f", "/etc/squid/squid.conf"]
