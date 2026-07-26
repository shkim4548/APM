#pragma once
#include "pch.h"
#include "ResilientSender.h"
#include "AesGcmCipher.h"
#include "LoadTesterConfig.h"
#include "LoadTestStats.h"

namespace apm { class Metric; }

/*-------------------
    SimulatedAgent
---------------------*/
// 실제 Agent 한 대에 대응하는 시뮬레이션 단위. ResilientSender를 그대로 재사용해서
// Collector 입장에서는 진짜 Agent와 구분 불가능한 트래픽을 만든다.
// MetricScheduler(ResourceCollector 의존)는 재사용하지 않음 - 부하 테스트는 실측 리소스가
// 아니라 고정/랜덤값으로 충분, 불필요한 결합을 만들지 않기 위함.

class SimulatedAgent
{
public:
    SimulatedAgent(asio::io_context& ioContext, asio::ssl::context& sslContext,
        const LoadTesterConfig& config, const AesGcmCipher::Key& agentCollectorKey,
        LoadTestStats& stats, int agentIndex);

    void Start();
    void Stop();

private:
    void ScheduleNextSend();
    apm::Metric BuildFakeMetric() const;

private:
    asio::io_context& _ioContext;
    const LoadTesterConfig& _config;
    LoadTestStats& _stats;
    int _agentIndex;
    ResilientSender _sender;
    asio::steady_timer _sendTimer;
    bool _running = false;
    bool _everConnected = false;   // ConnectionStateCallback에서 최초 연결/재연결 구분용
};
