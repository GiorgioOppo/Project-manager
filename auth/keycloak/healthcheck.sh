#!/bin/bash
set -euo pipefail
# The official image has no curl. Management HTTP is confined to loopback.
exec 3<>/dev/tcp/127.0.0.1/9000
printf 'GET /health/ready HTTP/1.0\r\nHost: localhost\r\n\r\n' >&3
read -r status <&3
[[ "$status" == *" 200 "* ]]
