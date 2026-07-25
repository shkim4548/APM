#pragma once
#include "pch.h"
#include "ResourceCollector.h"

/*-------------------
    MetricScheduler
---------------------*/
// 지정한 주기(interval)마다 ResourceCollector::Collect()를 반복 호출해서
// 콜백에 전달한다. Asio의 steady_timer 기반 - 별도 스레드/블로킹 sleep 없이
// io_context의 이벤트 루프 안에서 비동기로 동작.

class MetricScheduler
{
public:
    using MetricCallback = std::function<void(const SystemMetrics&)>;
    MetricScheduler(asio::io_context& ioContext, std::chrono::milliseconds interval, MetricCallback callback);

    void Start();
    void Stop();

private:
    void ScheduleNext();

private:
    asio::steady_timer _timer;
    std::chrono::milliseconds _interval;
    MetricCallback _callback;
    ResourceCollector _collector;
    bool _running = false;
};