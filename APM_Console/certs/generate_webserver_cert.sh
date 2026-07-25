#!/usr/bin/env bash
# WebServer가 collector로 부터 TLS 연결을 받을 때, 쓰는 자체 서명 인증서(프로덕션 금지)
set -euo pipefail
cd "$(dirname "$0")"

openssl req -x509 -newkey rsa:2048 \
    -keyout webserver.key -out webserver.crt \
    -days 365 -nodes \
    -subj "/CN=localhost"

echo "생성 완료 : cert/webserver.key, certs/webserver.crt"