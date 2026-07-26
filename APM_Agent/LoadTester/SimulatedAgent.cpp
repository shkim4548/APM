#include "pch.h"
#include "SimulatedAgent.h"
#include "AesGcmPayload.h"
#include "Protocol/Metric.pb.h"
#include <random>

namespace
{
    std::mt19937& RandomEngine()
    {
        static thread_local std::mt19937 engine{ std::random_device{}() };
        return engine;
    }

    double RandomPercent()
    {
        std::uniform_real_distribution<double> dist(0.0, 100.0);
        return dist(RandomEngine());
    }

    uint64 RandomBytes(uint64 maxValue)
    {
        std::uniform_int_distribution<uint64> dist(0, maxValue);
        return dist(RandomEngine());
    }
}

SimulatedAgent::SimulatedAgent(asio::io_context& ioContext, asio::ssl::context& sslContext,
    const LoadTesterConfig& config, const AesGcmCipher::Key& agentCollectorKey,
    LoadTestStats& stats, int agentIndex)
    : _ioContext(ioContext)
    , _config(config)
    , _stats(stats)
    , _agentIndex(agentIndex)
    , _sender(ioContext, sslContext, config.collectorHost, config.collectorPort,
        [agentCollectorKey]() { return std::make_unique<AesGcmPayload>(agentCollectorKey); },
        config.queueSize,
        [this](bool connected)
        {
            if (connected)
            {
                if (_everConnected)
                    _stats.OnReconnect();
                else
                    _stats.OnConnectSuccess();
                _everConnected = true;
            }
            else if (!_everConnected)
            {
                _stats.OnConnectFail();
            }
        })
    , _sendTimer(ioContext)
{
}

void SimulatedAgent::Start()
{
    _running = true;
    ScheduleNextSend();
}

void SimulatedAgent::Stop()
{
    _running = false;
    _sendTimer.cancel();
}

void SimulatedAgent::ScheduleNextSend()
{
    if (!_running)
        return;

    _sendTimer.expires_after(_config.sendInterval);
    _sendTimer.async_wait(
        [this](const asio::error_code& ec)
        {
            if (ec || !_running)
                return;

            auto sentAt = std::chrono::steady_clock::now();
            apm::Metric metric = BuildFakeMetric();

            _sender.Enqueue(metric,
                [this, sentAt](bool success)
                {
                    if (success)
                    {
                        auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - sentAt);
                        _stats.OnPacketSent(latency);
                    }
                    else
                    {
                        _stats.OnQueueDrop();
                    }
                });

            ScheduleNextSend();
        });
}

apm::Metric SimulatedAgent::BuildFakeMetric() const
{
    apm::Metric pkt;
    pkt.set_cpu_usage_percent(RandomPercent());
    pkt.set_mem_used_bytes(RandomBytes(8ULL * 1024 * 1024 * 1024));
    pkt.set_mem_total_bytes(16ULL * 1024 * 1024 * 1024);
    pkt.set_disk_used_bytes(RandomBytes(100ULL * 1024 * 1024 * 1024));
    pkt.set_disk_total_bytes(500ULL * 1024 * 1024 * 1024);
    pkt.set_net_rx_bytes_per_sec(RandomBytes(1024ULL * 1024));
    pkt.set_net_tx_bytes_per_sec(RandomBytes(1024ULL * 1024));
    pkt.set_tcp_rtt_us(static_cast<uint32>(RandomBytes(50000)));
    pkt.set_tcp_rtt_var_us(static_cast<uint32>(RandomBytes(5000)));
    pkt.set_tcp_retransmits(0);
    pkt.set_tcp_total_retrans(0);
    pkt.set_tcp_snd_cwnd(static_cast<uint32>(RandomBytes(100)));
    return pkt;
}
