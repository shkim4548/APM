#include "pch.h"
#include "LoadTester.h"
#include "KeyLoader.h"

LoadTester::LoadTester(LoadTesterConfig config)
    : _config(std::move(config))
    , _sslContext(asio::ssl::context::tls_client)
    , _agentCollectorKey(LoadKeyFromHexFile(_config.keyFilePath))
    , _spawnTimer(_ioContext)
    , _reportTimer(_ioContext)
    , _shutdownTimer(_ioContext)
{
    // 테스트용 자체 서명 인증서라 CA 검증 생략 - 실제 Agent(Agent/main.cpp)와 동일한 이유.
    _sslContext.set_verify_mode(asio::ssl::verify_none);
    _agents.reserve(_config.agentCount);
}

void LoadTester::Run()
{
    _startTime = std::chrono::steady_clock::now();

    std::cout << "[LoadTester] starting: agents=" << _config.agentCount
        << " interval-ms=" << _config.sendInterval.count()
        << " duration-sec=" << _config.duration.count()
        << " ramp-up-ms=" << _config.rampUp.count()
        << " collector=" << _config.collectorHost << ":" << _config.collectorPort
        << std::endl;

    SpawnAgents();
    ScheduleProgressReport();
    ScheduleShutdown();

    _ioContext.run();

    _stats.DumpCsv(_config.csvOutputPath);
}

void LoadTester::SpawnAgents()
{
    // rampUp==0: 전체 동시 connect(thundering herd 테스트) - 한 번에 다 생성.
    if (_config.rampUp.count() == 0)
    {
        for (int i = 0; i < _config.agentCount; ++i)
        {
            _agents.push_back(std::make_unique<SimulatedAgent>(
                _ioContext, _sslContext, _config, _agentCollectorKey, _stats, i));
            _agents.back()->Start();
        }
        return;
    }

    // rampUp>0: agentCount개를 rampUp 기간에 걸쳐 균등 간격으로 스태거링 connect.
    auto staggerInterval = _config.rampUp / _config.agentCount;

    auto spawnNext = std::make_shared<std::function<void(int)>>();
    *spawnNext = [this, staggerInterval, spawnNext](int index)
    {
        if (index >= _config.agentCount)
            return;

        _agents.push_back(std::make_unique<SimulatedAgent>(
            _ioContext, _sslContext, _config, _agentCollectorKey, _stats, index));
        _agents.back()->Start();

        _spawnTimer.expires_after(staggerInterval);
        _spawnTimer.async_wait(
            [this, spawnNext, index](const asio::error_code& ec)
            {
                if (!ec)
                    (*spawnNext)(index + 1);
            });
    };

    (*spawnNext)(0);
}

void LoadTester::ScheduleProgressReport()
{
    _reportTimer.expires_after(std::chrono::seconds(1));
    _reportTimer.async_wait(
        [this](const asio::error_code& ec)
        {
            if (ec)
                return;

            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - _startTime);
            _stats.PrintProgressAndReset(elapsed);
            ScheduleProgressReport();
        });
}

void LoadTester::ScheduleShutdown()
{
    _shutdownTimer.expires_after(_config.duration);
    _shutdownTimer.async_wait(
        [this](const asio::error_code& ec)
        {
            if (!ec)
                Shutdown();
        });
}

void LoadTester::Shutdown()
{
    std::cout << "[LoadTester] duration elapsed, shutting down" << std::endl;

    _spawnTimer.cancel();
    _reportTimer.cancel();

    for (auto& agent : _agents)
        agent->Stop();

    // 마지막 진행 상황 한 번 더 출력하고 io_context를 정지 - ResilientSender의
    // 재연결 타이머 등 아직 남아있는 비동기 작업이 있어도 run()이 즉시 반환하게 함.
    // (소켓을 정중히 닫지는 않음 - 프로세스가 곧 종료되는 부하 테스트 도구라 범위 밖으로 판단.
    // 필요해지면 SimulatedAgent/ResilientSender에 Close() 계열 메서드 추가 검토.)
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - _startTime);
    _stats.PrintProgressAndReset(elapsed);

    _ioContext.stop();
}
