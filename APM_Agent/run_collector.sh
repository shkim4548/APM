#!/usr/bin/env bash
# APM_Agent/Collector 실행 - 어느 위치에서 호출해도 스크립트 자신의 디렉토리 기준으로 동작.
set -euo pipefail
cd "$(dirname "$0")"

if [ -x build/Collector ]; then
    exec ./build/Collector
elif [ -x build-static/Debug/Collector.exe ]; then
    exec ./build-static/Debug/Collector.exe
else
    echo "[run_collector] Collector 실행 파일을 찾을 수 없음 (build/Collector 또는 build-static/Debug/Collector.exe)." >&2
    echo "[run_collector] 먼저 빌드하세요 - HOW_TO_RUN.md 3번 참고." >&2
    exit 1
fi
