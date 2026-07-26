#include "pch.h"
#include "ScopedSpan.h"
#include "SpanRecorder.h"

ScopedSpan::ScopedSpan(String operationName)
    : _operationName(std::move(operationName)), _start(std::chrono::steady_clock::now())
{
}

ScopedSpan::~ScopedSpan()
{
    auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - _start).count();

    SpanRecorder::Instance().Record(_operationName, static_cast<uint64>(durationUs), _success);
}
