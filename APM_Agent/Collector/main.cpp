#include "pch.h"
#include "ApmSession.h"
#include "AesGcmPayload.h"
#include "ResilientSender.h"
#include "PrivilegeDrop.h"
#include "PacketHandler.h"
#include "KeyLoader.h"
#include "CollectorConfig.h"
#include "ScopedSpan.h"
#include "SpanRecorder.h"
#include "Protocol/Metric.pb.h"
#include "Storage/MetricStoreFactory.h"
#include <thread>

// SqliteMetricStore::Store()의 fdatasync가 네트워크 스레드를 블로킹하는 문제(1-5 실측 발견) 개선용
// - 저장 작업을 전용 워커 스레드로 넘기기 위한 JobQueue 서브시스템. 이 서브시스템은 여태 APM_Agent
// 어디서도 안 쓰였고, 자신의 pch(CorePch.h)에 기대는 방식이라 자기완결적이지 않음 - 그 pch가 쓰는
// 순서 그대로 나열해야 컴파일됨(2026-07-27 확인). CoreMacro.h의 PrintStackTrace()/CrashLog()가
// <fstream>/<execinfo.h>(Windows는 <dbghelp.h>)를 자기 스스로 include 안 해서 빌드 시도 중 추가 발견.
#include <fstream>
#ifdef _WIN32
#include <dbghelp.h>
#else
#include <execinfo.h>
#endif
#include "CoreMacro.h"      // WRITE_LOCK/USE_LOCK 매크로, GetCurrentTick()
#include "CoreGlobal.h"     // extern GThreadManager
#include "CoreTLS.h"        // thread_local LEndTickCount
#include "Lock.h"           // LockQueue가 쓰는 Lock 클래스
#include "ObjectPool.h"     // 전역 MakeShared<T>()
#include "LockQueue.h"      // JobQueue 내부 큐
#include "JobTimer.h"       // JobQueue가 참조
#include "JobQueue.h"       // JobQueue, JobQueueRef
#include "ThreadManager.h"  // GThreadManager->Launch(), DoGlobalQueueWork()

namespace
{
    constexpr unsigned short PORT = 9000;
    // SQLite: 파일 경로 / TimescaleDB: libpq 연결 문자열("host=... dbname=... user=...")
    constexpr const char* STORAGE_CONNECTION_INFO = "apm_metrics.db";
}

int main()
{
#ifdef _WIN32
    // 소스가 UTF-8(/utf-8)로 컴파일되는데 Windows 콘솔 기본 코드페이지(한글 Windows는 949)는
    // 그와 달라서, std::cout으로 찍는 한글 문자열이 콘솔에서 깨져 보임 - 출력 코드페이지를
    // UTF-8로 맞춰서 해결. Linux는 기본이 UTF-8이라 이 문제 자체가 없음.
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        // Agent<->Collector, Collector<->WebServer 두 구간 모두 AES-256-GCM(AesGcmPayload) -
        // 구간별 키는 분리 유지(한 쪽이 유출돼도 다른 구간은 안전). 배경: Docs/ARIA_TO_AES_MIGRATION.md
        AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");
        AesGcmCipher::Key webServerKey = LoadKeyFromHexFile("certs/webserver_aes.key");

        CollectorConfig config = LoadCollectorConfig("collector_config.json");

        auto store = CreateMetricStore(STORAGE_CONNECTION_INFO, config.metricsRetentionDays);

        // 1-7: SqliteMetricStore::Store()의 fdatasync가 네트워크 스레드를 막지 못하게, 저장 호출을
        // 전용 워커 스레드로 넘기는 큐. storePtr은 store(unique_ptr)가 main() 스코프 내내 살아있는
        // 것에 기대는 non-owning 포인터 - 워커 스레드도 main()이 끝나기 전까지만 존재하므로 안전.
        IMetricStore* storePtr = store.get();
        JobQueueRef metricStoreQueue = MakeShared<JobQueue>();

        // 전용 워커 스레드 1개 - SQLite는 어차피 단일 writer라 여러 개 띄워도 JobQueue 자체가
        // 직렬화함(늘릴 이유 없음). LEndTickCount를 루프마다 먼저 세팅해야 DoGlobalQueueWork()가
        // 즉시 break하지 않음(GW2 틱 서버 관례) - ThreadManager.cpp:63 참고.
        GThreadManager->Launch([]()
            {
                while (true)
                {
                    LEndTickCount = GetCurrentTick() + 100;
                    ThreadManager::DistributeReservedJobs();
                    ThreadManager::DoGlobalQueueWork();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });

        // Collector가 받은 뒤 아직 WebServer로 안 보낸 메트릭들 - 주기/CLI 트리거로 비워짐.
        std::vector<apm::Metric> pendingMetrics;

        asio::io_context ioContext;

        // Agent 접속을 받는 서버 역할 컨텍스트(기존)
        asio::ssl::context sslContext(asio::ssl::context::tls_server);
        sslContext.use_certificate_chain_file("certs/server.crt");
        sslContext.use_private_key_file("certs/server.key", asio::ssl::context::pem);

        // WebServer에 접속하는 클라이언트 역할 컨텍스트(신규) - Agent용과 모드가 달라 별도 필요.
        asio::ssl::context webServerSslContext(asio::ssl::context::tls_client);
        // 테스트용 자체 서명 인증서라 CA 검증 생략(Agent->Collector와 동일한 이유, 프로덕션 금지).
        webServerSslContext.set_verify_mode(asio::ssl::verify_none);

        ResilientSender webServerSender(ioContext, webServerSslContext,
            config.webServerHost, config.webServerPort,
            [webServerKey]() { return std::make_unique<AesGcmPayload>(webServerKey); });

        PacketHandler::Register<apm::Metric>(
            [storePtr, metricStoreQueue, &pendingMetrics](const apm::Metric& pkt)
            {
                // Collector 안에서 "트랜잭션"이라 부를 만한 지점 중 가장 자연스러운 곳 -
                // Agent가 보낸 메트릭 패킷 하나를 받아 저장하는 구간(2026-07-26 4순위 데모 계측).
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                // store->Store(pkt) 직접 호출(동기, fdatasync 블로킹 포함) 대신 워커 스레드로 위임.
                // pushOnly=true 필수 - 기본값(false)이면 호출 스레드가 다른 JobQueue::Execute() 안이
                // 아닐 때 그 자리에서 동기 실행해버려 아무 효과가 없어짐(JobQueue.cpp:18).
                // pkt은 값 복사로 캡처 - 비동기 실행 시점까지 살아있어야 함.
                metricStoreQueue->Push(MakeShared<Job>([storePtr, pkt]() { storePtr->Store(pkt); }), /*pushOnly=*/true);

                pendingMetrics.push_back(pkt);
                // 저장이 이제 비동기라 이 시점엔 아직 안 끝났을 수 있음 - "stored"는 부정확한 표현이라 정정.
                std::cout << "[Collector] metric received: cpu=" << pkt.cpu_usage_percent()
                    << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                    << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                    << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                    << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                    << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                    << ",cwnd:" << pkt.tcp_snd_cwnd()
                    << std::endl;
            });

        asio::ip::tcp::acceptor acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), PORT));

        // 포트바인딩 : 특권이 필요할 수 있는 유일한 단계, 완료 직후 권한 하향
        PrivilegeDrop::DropTo("nobody");

        std::function<void()> doAccept;
        doAccept = [&]()
        {
            acceptor.async_accept(
                [&](const asio::error_code &ec, asio::ip::tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::cout << "[Collector] connection accepted" << std::endl;
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::cerr << "[Collector] accept error : " << ec.message() << std::endl;
                    }
                    doAccept();
                });
        };
        doAccept();

        // pendingMetrics/SpanRecorder에 쌓인 걸 전부 WebServer로 보내고 비움 - 주기 타이머와
        // CLI 트리거 둘 다 이 함수 하나를 호출함(로직 중복 방지). span 전송을 여기 얹은 이유:
        // 이미 "주기적으로 WebServer에 밀어넣는" 책임을 지고 있는 함수라 새 타이머를 또
        // 만들 필요가 없음(2026-07-26 4순위 설계).
        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (!pendingMetrics.empty())
            {
                std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
                for (const auto& m : pendingMetrics)
                    webServerSender.Enqueue(m);

                pendingMetrics.clear();
            }

            auto spans = SpanRecorder::Instance().DrainAll();
            if (!spans.empty())
            {
                std::cout << "[Collector] WebServer로 span " << spans.size() << "건 전송 시도" << std::endl;
                for (const auto& s : spans)
                {
                    apm::TransactionSpan pkt;
                    pkt.set_operation_name(s.operationName);
                    pkt.set_duration_us(s.durationUs);
                    pkt.set_success(s.success);
                    webServerSender.Enqueue(pkt);
                }
            }
        };

        asio::steady_timer pushTimer(ioContext);
        std::function<void()> schedulePush;
        schedulePush = [&]()
        {
            pushTimer.expires_after(std::chrono::seconds(config.pushIntervalSeconds));
            pushTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        flushToWebServer();
                        schedulePush();
                    }
                });
        };
        schedulePush();

        // 로컬 저장소(store) 보존 정책 - pushTimer와 같은 패턴, 24시간 간격으로
        // 오래된 행 정리(TimescaleMetricStore는 내부적으로 no-op, SqliteMetricStore만 실제
        // DELETE 수행 - IMetricStore::Prune 문서 참고, 2026-07-26 3순위 설계).
        asio::steady_timer pruneTimer(ioContext);
        std::function<void()> schedulePrune;
        schedulePrune = [&]()
        {
            pruneTimer.expires_after(std::chrono::hours(24));
            pruneTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        store->Prune(config.metricsRetentionDays);
                        schedulePrune();
                    }
                });
        };
        schedulePrune();

        // Collector 콘솔에 "send"를 입력하면 즉시 전송. stdin 읽기는 블로킹이라 별도 스레드에서
        // 돌리고, 실제 전송(flushToWebServer)은 asio::post로 io_context 스레드에 넘김 -
        // pendingMetrics/webServerSender를 항상 단일 스레드에서만 건드리게 되어 락이 불필요함.
        std::thread cliThread(
            [&ioContext, &flushToWebServer]()
            {
                String line;
                while (std::getline(std::cin, line))
                {
                    if (line == "send")
                        asio::post(ioContext, flushToWebServer);
                }
            });
        cliThread.detach();

        std::cout << "Collector listening on port " << PORT << " (TLS)" << std::endl;
        std::cout << "[Collector] WebServer(" << config.webServerHost << ":" << config.webServerPort
            << ")로 " << config.pushIntervalSeconds << "초마다 전송 (콘솔에 'send' 입력 시 즉시 전송)" << std::endl;
        ioContext.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Collector] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}