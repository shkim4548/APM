#!/bin/bash
# Collector->WebServer 구간용 AES-256 키 생성 - Agent/Collector용 ARIA/HMAC 키와는
# 별개(generate_payload_keys.sh 참고). .gitignore의 certs/*.key 패턴에 이미 포함됨.
set -e
cd "$(dirname "$0")"

openssl rand -hex 32 > webserver_aes.key

echo "생성됨: webserver_aes.key"
