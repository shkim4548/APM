#include "pch.h"
#include "LocalAlertEvaluator.h"
#include "LogLevel.h"

AlertTransition EvaluateTransition(double currentValue, double threshold, bool currentlyOpen)
{
    bool isBreaching = currentValue >= threshold;

    if (isBreaching && !currentlyOpen)
        return AlertTransition::Opened;

    if (!isBreaching && currentlyOpen)
        return AlertTransition::Resolved;

    return AlertTransition::None;
}

LocalAlertEvaluator::LocalAlertEvaluator(const ThresholdSet& thresholds, AgentAlertStore& store)
    : _thresholds(thresholds), _store(store)
{
}

void LocalAlertEvaluator::OnNewMetric(const apm::Metric& metric)
{
    const double memPercent = metric.mem_total_bytes() > 0
        ? metric.mem_used_bytes() * 100.0 / metric.mem_total_bytes()
        : 0.0;
    const double diskPercent = metric.disk_total_bytes() > 0
        ? metric.disk_used_bytes() * 100.0 / metric.disk_total_bytes()
        : 0.0;

    EvaluateOne(AlertMetricType::CpuPercent, metric.cpu_usage_percent());
    EvaluateOne(AlertMetricType::MemoryPercent, memPercent);
    EvaluateOne(AlertMetricType::DiskPercent, diskPercent);
    EvaluateOne(AlertMetricType::TcpRttUs, static_cast<double>(metric.tcp_rtt_us()));
}

void LocalAlertEvaluator::EvaluateOne(AlertMetricType type, double currentValue)
{
    double threshold = _thresholds.Get(type);
    std::optional<LocalAlertRecord> openAlert = _store.FindOpen(type);

    AlertTransition transition = EvaluateTransition(currentValue, threshold, openAlert.has_value());

    if (transition == AlertTransition::Opened)
    {
        _store.Open(type, threshold, currentValue);
        if (GetLogLevel() >= LogLevel::Info)
            std::cout << "[LocalAlertEvaluator] 알림 발생: type=" << static_cast<int>(type)
                << " value=" << currentValue << " (임계치 " << threshold << ")" << std::endl;
    }
    else if (transition == AlertTransition::Resolved && openAlert.has_value())
    {
        _store.Resolve(openAlert->id, currentValue);
        if (GetLogLevel() >= LogLevel::Info)
            std::cout << "[LocalAlertEvaluator] 알림 해제: type=" << static_cast<int>(type)
                << " value=" << currentValue << std::endl;
    }
}
