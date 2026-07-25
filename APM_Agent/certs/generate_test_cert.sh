#!/usr/bin/env bash
# 로컬 TLS 핸드셰이크 테스트용 자체 서명 인증서 생성 (프로덕션에 절대 사용 금지)

set -euo pipefail
cd "$(dirname "$0")"

openssl req -x509 -newkey rsa:2048 \
    -keyout server.key -out server.crt \
    -days 365 -nodes \
    -subj "/CN=localhost"

echo "생성 완료: certs/server.key, certs/server.crt"