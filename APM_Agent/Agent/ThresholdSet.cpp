#include "pch.h"
#include "ThresholdSet.h"

namespace
{
// APM_Console의 AlertThresholds 시드값과 동일(ApmDbContext.cs 초기 데이터) -
// Phase B에서 실제 값을 받기 전까지 같은 기준으로 동작하도록 맞춤.
constexpr double kDefaultCpuPercent = 90.0;
constexpr double kDefaultMemoryPercent = 90.0;
constexpr double kDefaultDiskPercent = 90.0;
constexpr double kDefaultTcpRttUs = 200000.0;
}

ThresholdSet::ThresholdSet()
    : _values{ kDefaultCpuPercent, kDefaultMemoryPercent, kDefaultDiskPercent, kDefaultTcpRttUs }
{
}

double ThresholdSet::Get(AlertMetricType type) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _values[static_cast<size_t>(type)];
}

void ThresholdSet::Update(AlertMetricType type, double value)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _values[static_cast<size_t>(type)] = value;
}
