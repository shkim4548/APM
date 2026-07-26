#pragma once
#include "pch.h"
#include "LoadTesterConfig.h"
#include "LoadTestStats.h"
#include "SimulatedAgent.h"
#include "AesGcmCipher.h"

/*-------------------
    LoadTester
---------------------*/
// 오케스트레이터. io_context 하나에 N개의 SimulatedAgent를 램프업 스케줄에 맞춰 생성하고,
// 지정된 duration 동안 실행한 뒤 정리하고 CSV를 남긴다.

class LoadTester
{
public:
    explicit LoadTester(LoadTesterConfig config);

    // 블로킹 - ioContext.run()을 내부에서 호출하고 duration 경과 시 스스로 정지.
    void Run();

private:
    void SpawnAgents();          // ramp-up 스케줄에 맞춰 SimulatedAgent 생성(동시 또는 스태거링)
    void ScheduleProgressReport();
    void ScheduleShutdown();
    void Shutdown();

private:
    LoadTesterConfig _config;
    asio::io_context _ioContext;
    asio::ssl::context _sslContext;
    AesGcmCipher::Key _agentCollectorKey;
    LoadTestStats _stats;
    std::vector<std::unique_ptr<SimulatedAgent>> _agents;
    asio::steady_timer _spawnTimer;
    asio::steady_timer _reportTimer;
    asio::steady_timer _shutdownTimer;
    std::chrono::steady_clock::time_point _startTime;
};
