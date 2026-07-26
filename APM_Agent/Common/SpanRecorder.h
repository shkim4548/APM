#pragma once
#include "pch.h"
#include <mutex>
#include <vector>

struct SpanRecord
{
    String operationName;
    uint64 durationUs;
    bool success;
};

/*-------------
    SpanRecorder
---------------*/
// ScopedSpan이 다 끝난 span을 여기 적재만 해두는 프로세스 전역 큐 - 실제 WebServer
// 전송은 Collector/main.cpp의 flushToWebServer가 주기적으로 DrainAll()해서 비움.
// pendingMetrics(main.cpp 지역 변수, io_context 스레드 전용이라 락 불필요)와 달리
// 이건 어떤 스레드에서 계측 매크로가 쓰일지 SDK 입장에서 보장할 수 없어 뮤텍스로 보호
// (지금 당장은 io_context 스레드에서만 쓰지만, 나중에 cliThread 같은 별도 스레드의
// 코드에 계측을 추가해도 안전해야 함 - 2026-07-26 4순위 설계).
class SpanRecorder
{
public:
    static SpanRecorder& Instance();

    void Record(const String& operationName, uint64 durationUs, bool success);

    // 지금까지 쌓인 걸 전부 꺼내고 비움.
    std::vector<SpanRecord> DrainAll();

private:
    std::mutex _mutex;
    std::vector<SpanRecord> _pending;
};
