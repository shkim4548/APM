#pragma once
#include "pch.h"

/*-------------------
    LoadTestStats
---------------------*/
// io_context 이벤트 루프(단일 스레드)에서만 접근된다는 전제 - 별도 스레드가 없으므로
// 락 없이 단순 카운터/vector로 구현.

class LoadTestStats
{
public:
    void OnConnectSuccess();
    void OnConnectFail();
    void OnReconnect();
    void OnQueueDrop();
    void OnPacketSent(std::chrono::milliseconds latency);   // enqueue -> Send 완료까지 지연

    // 마지막 호출 이후의 델타만 계산해 콘솔 한 줄 출력(최근 처리량 파악용) 후 델타 리셋.
    void PrintProgressAndReset(std::chrono::seconds elapsed);

    // 종료 시 누적 통계 + 지연시간 분포(p50/p95/p99)를 CSV로 저장.
    void DumpCsv(const String& path) const;

private:
    static std::chrono::milliseconds Percentile(std::vector<std::chrono::milliseconds> sorted, double p);

private:
    uint64_t _connectSuccessCount = 0;
    uint64_t _connectFailCount = 0;
    uint64_t _reconnectCount = 0;
    uint64_t _queueDropCount = 0;
    uint64_t _sentCount = 0;
    uint64_t _sentSinceLastReport = 0;
    std::vector<std::chrono::milliseconds> _latencySamples;   // 실행 시간 내내 무제한 누적(현재 계획 규모 기준 - SESSION_LOG.md 참고)
};
