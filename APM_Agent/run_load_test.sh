#!/usr/bin/env bash
# APM_Agent/Collector를 strace -c + RSS/CPU 샘플링으로 감싼 채 LoadTester로 부하를 걸고,
# 결과를 loadtest_results/ 아래에 남긴다. Linux(strace) 전용 - Windows에서는 미지원.
# 어느 위치에서 호출해도 스크립트 자신의 디렉토리(APM_Agent/) 기준으로 동작.
set -euo pipefail
cd "$(dirname "$0")"

AGENTS="${1:?사용법: run_load_test.sh <agent_count> [duration_sec=60] [interval_ms=100] [ramp_up_ms=0]}"
DURATION="${2:-60}"
INTERVAL_MS="${3:-100}"
RAMP_UP_MS="${4:-0}"

if [ ! -x build/Collector ] || [ ! -x build/LoadTester ]; then
    echo "[run_load_test] build/Collector 또는 build/LoadTester가 없음 - 먼저 빌드하세요:" >&2
    echo "  cmake --build build -j\$(nproc)" >&2
    exit 1
fi

RESULT_DIR="loadtest_results/agents_${AGENTS}_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULT_DIR"
echo "[run_load_test] agents=$AGENTS duration=${DURATION}s interval=${INTERVAL_MS}ms ramp-up=${RAMP_UP_MS}ms"
echo "[run_load_test] results -> $RESULT_DIR"

rm -f apm_metrics.db   # 이전 실행의 누적 데이터와 섞이지 않게 Collector를 매번 새 DB로 시작

# 1) Collector를 strace -c로 감싸서 백그라운드 실행(syscall 프로파일, README 7-2절과 동일 방식).
strace -c -o "$RESULT_DIR/collector_strace.txt" ./build/Collector > "$RESULT_DIR/collector_stdout.log" 2>&1 &
STRACE_PID=$!

sleep 1
# strace는 자식 프로세스를 새로 fork+exec하므로 $STRACE_PID는 strace 자신의 pid다 -
# 실제 Collector(트레이싱 대상)의 pid는 그 직속 자식에서 찾는다(pgrep -f로 커맨드라인
# 문자열을 매칭하면 strace 자신의 인자에도 "Collector"가 들어있어 오탐할 수 있어 피함).
# "|| true"로 pgrep 실패(자식을 못 찾음)를 무시 - set -e/pipefail 상태에서 그대로 두면
# 아래 진단 메시지도 못 찍고 스크립트가 조용히 죽어버림(실제로 이렇게 죽는 걸 확인함, 2026-07-26).
COLLECTOR_PID=$(pgrep -P "$STRACE_PID" | head -n1 || true)
if [ -z "$COLLECTOR_PID" ]; then
    echo "[run_load_test] Collector pid를 찾지 못함 - Collector가 바로 죽었을 가능성이 큼." >&2
    echo "[run_load_test] 로그 확인: $RESULT_DIR/collector_stdout.log" >&2
    echo "[run_load_test] (흔한 원인: 9000번 포트를 이미 다른 Collector가 쓰고 있음 - 'pgrep -af Collector'로 확인)" >&2
    cat "$RESULT_DIR/collector_stdout.log" >&2 2>/dev/null || true
    kill "$STRACE_PID" 2>/dev/null || true
    exit 1
fi
echo "[run_load_test] Collector pid=$COLLECTOR_PID (strace pid=$STRACE_PID)"

# 2) RSS/CPU를 1초 간격으로 백그라운드 샘플링(README 7-3절과 동일 방식 - /proc/[pid]/status,
#    /proc/[pid]/stat 기준. 원본은 15초 간격/5분이었지만 부하 테스트는 훨씬 짧게 도니 1초로 촘촘히).
SAMPLE_CSV="$RESULT_DIR/collector_resource_samples.csv"
echo "elapsed_sec,rss_kb,cpu_ticks_total" > "$SAMPLE_CSV"
(
    START=$(date +%s)
    while kill -0 "$COLLECTOR_PID" 2>/dev/null; do
        ELAPSED=$(( $(date +%s) - START ))
        RSS=$(awk '/VmRSS/ {print $2}' "/proc/$COLLECTOR_PID/status" 2>/dev/null || echo 0)
        read -r UTIME STIME < <(awk '{print $14, $15}' "/proc/$COLLECTOR_PID/stat" 2>/dev/null || echo "0 0")
        echo "${ELAPSED},${RSS:-0},$(( UTIME + STIME ))" >> "$SAMPLE_CSV"
        sleep 1
    done
) &
SAMPLER_PID=$!

# 3) Collector가 accept 가능해질 시간을 준 뒤 LoadTester 실행 - 이게 실제 부하 발생원.
sleep 1
./build/LoadTester \
    --agents "$AGENTS" \
    --interval-ms "$INTERVAL_MS" \
    --duration-sec "$DURATION" \
    --ramp-up-ms "$RAMP_UP_MS" \
    --csv-out "$RESULT_DIR/loadtester_result.csv" \
    2>&1 | tee "$RESULT_DIR/loadtester_stdout.log"

# 4) LoadTester가 끝나면 Collector를 정지 -> strace -c 요약이 이 시점에 파일로 flush됨.
kill -TERM "$COLLECTOR_PID" 2>/dev/null || true
wait "$STRACE_PID" 2>/dev/null || true
wait "$SAMPLER_PID" 2>/dev/null || true

echo "[run_load_test] 완료. 결과: $RESULT_DIR/{collector_strace.txt, collector_resource_samples.csv, loadtester_result.csv}"
