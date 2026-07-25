#include "MetricScheduler.h"

MetricScheduler::MetricScheduler(asio::io_context &ioContext, std::chrono::milliseconds interval, MetricCallback callback)
    : _timer(ioContext), _interval(interval), _callback(std::move(callback))
{
}

void MetricScheduler::Start()
{
    _running = true;
    ScheduleNext();
}

void MetricScheduler::Stop()
{
    _running = false;
    _timer.cancel();
}

void MetricScheduler::ScheduleNext()
{
    if (!_running)
        return;

    _timer.expires_after(_interval);
    _timer.async_wait(
        [this](const asio::error_code& ec)
        {
            // Stop()의 cancel() 등으로 취소된 경우 - 콜백을 실행하지 않고 종료
            if(ec)
                return;

            SystemMetrics metrics = _collector.Collect();
            _callback(metrics);

            ScheduleNext();
        }
    );
}
