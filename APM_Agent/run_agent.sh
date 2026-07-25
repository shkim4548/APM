#!/usr/bin/env bash
# APM_Agent/Agent 실행 - 어느 위치에서 호출해도 스크립트 자신의 디렉토리 기준으로 동작.
set -euo pipefail
cd "$(dirname "$0")"

if [ -x build/Agent ]; then
    exec ./build/Agent
elif [ -x build-static/Debug/Agent.exe ]; then
    exec ./build-static/Debug/Agent.exe
else
    echo "[run_agent] Agent 실행 파일을 찾을 수 없음 (build/Agent 또는 build-static/Debug/Agent.exe)." >&2
    echo "[run_agent] 먼저 빌드하세요 - HOW_TO_RUN.md 3번 참고." >&2
    exit 1
fi
