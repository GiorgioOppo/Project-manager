FROM haproxy:3.2-alpine

USER root
RUN apk add --no-cache python3 mariadb-client mariadb-connector-c

COPY health.py /usr/local/bin/punto-db-health.py
COPY entrypoint.sh /usr/local/bin/punto-db-proxy-entrypoint.sh
COPY haproxy.cfg /usr/local/etc/haproxy/haproxy.cfg
RUN chmod 0555 /usr/local/bin/punto-db-health.py \
    /usr/local/bin/punto-db-proxy-entrypoint.sh

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    CLUSTER_MONITOR_USER=punto_monitor

USER haproxy
EXPOSE 3306

HEALTHCHECK --interval=10s --timeout=12s --start-period=30s --retries=3 \
    CMD python3 /usr/local/bin/punto-db-health.py ready || exit 1

ENTRYPOINT ["/usr/local/bin/punto-db-proxy-entrypoint.sh"]
CMD ["haproxy", "-W", "-db", "-f", "/usr/local/etc/haproxy/haproxy.cfg"]
