#include "pch.h"
#include "LoadTestStats.h"
#include <algorithm>
#include <fstream>

void LoadTestStats::OnConnectSuccess() { ++_connectSuccessCount; }
void LoadTestStats::OnConnectFail() { ++_connectFailCount; }
void LoadTestStats::OnReconnect() { ++_reconnectCount; }
void LoadTestStats::OnQueueDrop() { ++_queueDropCount; }

void LoadTestStats::OnPacketSent(std::chrono::milliseconds latency)
{
    ++_sentCount;
    ++_sentSinceLastReport;
    _latencySamples.push_back(latency);
}

void LoadTestStats::PrintProgressAndReset(std::chrono::seconds elapsed)
{
    std::cout << "[LoadTester] t=" << elapsed.count() << "s"
        << " sent(total)=" << _sentCount
        << " sent(recent)=" << _sentSinceLastReport
        << " connected=" << _connectSuccessCount
        << " reconnects=" << _reconnectCount
        << " connectFails=" << _connectFailCount
        << " queueDrops=" << _queueDropCount
        << std::endl;

    _sentSinceLastReport = 0;
}

std::chrono::milliseconds LoadTestStats::Percentile(std::vector<std::chrono::milliseconds> sorted, double p)
{
    if (sorted.empty())
        return std::chrono::milliseconds(0);

    std::sort(sorted.begin(), sorted.end());
    size_t index = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

void LoadTestStats::DumpCsv(const String& path) const
{
    std::ofstream out(path);
    if (!out)
    {
        std::cerr << "[LoadTestStats] failed to open csv output: " << path << std::endl;
        return;
    }

    std::chrono::milliseconds p50 = Percentile(_latencySamples, 0.50);
    std::chrono::milliseconds p95 = Percentile(_latencySamples, 0.95);
    std::chrono::milliseconds p99 = Percentile(_latencySamples, 0.99);

    out << "metric,value\n";
    out << "sent_total," << _sentCount << "\n";
    out << "connect_success," << _connectSuccessCount << "\n";
    out << "connect_fail," << _connectFailCount << "\n";
    out << "reconnect," << _reconnectCount << "\n";
    out << "queue_drop," << _queueDropCount << "\n";
    out << "latency_p50_ms," << p50.count() << "\n";
    out << "latency_p95_ms," << p95.count() << "\n";
    out << "latency_p99_ms," << p99.count() << "\n";

    std::cout << "[LoadTester] result written to " << path << std::endl;
}
