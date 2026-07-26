#include "pch.h"
#include "SpanRecorder.h"

SpanRecorder& SpanRecorder::Instance()
{
    static SpanRecorder instance;
    return instance;
}

void SpanRecorder::Record(const String& operationName, uint64 durationUs, bool success)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _pending.push_back(SpanRecord{ operationName, durationUs, success });
}

std::vector<SpanRecord> SpanRecorder::DrainAll()
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<SpanRecord> result = std::move(_pending);
    _pending.clear();
    return result;
}
