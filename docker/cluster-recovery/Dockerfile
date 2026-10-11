FROM haproxy:3.2-alpine

USER root
RUN apk add --no-cache python3 mariadb-client mariadb-connector-c \
    && addgroup -g 10001 punto-recovery \
    && adduser -D -H -u 10001 -G punto-recovery punto-recovery \
    && mkdir -p /app /var/lib/punto-cluster \
    && chown 10001:10001 /var/lib/punto-cluster

COPY recovery.py /app/recovery.py
USER 10001:10001
STOPSIGNAL SIGTERM
ENTRYPOINT ["python3", "/app/recovery.py"]
CMD ["run"]
HEALTHCHECK --interval=10s --timeout=5s --start-period=45s --retries=3 \
    CMD ["python3", "/app/recovery.py", "healthz"]
