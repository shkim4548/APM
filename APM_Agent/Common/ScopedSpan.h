#pragma once
#include "pch.h"
#include <chrono>

/*------------
    ScopedSpan
--------------*/
// C++ 쪽 계측 SDK - 생성자~소멸자 구간(RAII)을 자동으로 측정해서 SpanRecorder에 적재.
// 스택 언와인딩(예외로 스코프를 빠져나가는 경우) 중에도 소멸자는 반드시 호출되므로
// 측정 자체는 누락되지 않음 - "RAII 스코프 기반" 확정(2026-07-26 4순위 설계).
class ScopedSpan
{
public:
    explicit ScopedSpan(String operationName);
    ~ScopedSpan();

    // 계측 대상 코드가 실패를 명시적으로 표시할 때 호출(기본은 성공으로 간주) -
    // 예외가 던져져도 자동으로 실패 처리되진 않음(과설계 방지, .NET TraceScope.MarkFailed()와 동일 설계).
    void MarkFailed() { _success = false; }

private:
    String _operationName;
    std::chrono::steady_clock::time_point _start;
    bool _success = true;
};

#define APM_CONCAT_INNER(a, b) a##b
#define APM_CONCAT(a, b) APM_CONCAT_INNER(a, b)
// 함수/블록 진입 지점에 이 한 줄만 추가하면 스코프를 빠져나갈 때(정상 반환/예외 무관)
// 자동으로 측정되어 SpanRecorder에 쌓임.
#define APM_TRACE_SCOPE(name) ScopedSpan APM_CONCAT(_apmSpan_, __LINE__)(name)
