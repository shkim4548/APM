#!/bin/bash
# Agent->Collector 구간용 AES-256 키 생성 - Collector->WebServer용 webserver_aes.key와는
# 별개(구간별 키 분리 유지 - 한 쪽이 유출돼도 다른 구간은 안전).
# 이 구간은 원래 ARIA-CBC+HMAC(SecurePayload, certs/generate_payload_keys.sh)을 썼으나
# AES-256-GCM으로 통일 - 배경은 Docs/ARIA_TO_AES_MIGRATION.md 참고.
# .gitignore의 certs/*.key 패턴에 이미 포함됨.
set -e
cd "$(dirname "$0")"

openssl rand -hex 32 > agent_collector_aes.key

echo "생성됨: agent_collector_aes.key"