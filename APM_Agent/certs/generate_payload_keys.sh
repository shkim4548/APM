#!/bin/bash
# [참고용 - 2026-07-20부로 기본 빌드/실행에는 더 이상 필요하지 않음]
# Agent<->Collector 구간이 AES-256-GCM(agent_collector_aes.key)로 통일되면서 이 스크립트가
# 만드는 aria.key/hmac.key는 SecurePayload를 직접 쓰는 코드(현재는 실행 경로에 없고,
# 참고/향후 테스트용으로만 남겨둔 클래스)에만 필요함. 배경은 Docs/ARIA_TO_AES_MIGRATION.md 참고.
# ARIA/HMAC 대칭키 생성 - Agent/Collector가 로컬 파일로 공유(TLS 인증서와 같은 패턴).
# .gitignore의 certs/*.key 패턴에 이미 포함되어 커밋되지 않음.
set -e
cd "$(dirname "$0")"

openssl rand -hex 32 > aria.key
openssl rand -hex 32 > hmac.key

echo "생성됨: aria.key, hmac.key"