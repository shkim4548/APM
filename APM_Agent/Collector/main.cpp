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

// SqliteMetricStore::Store()의 fdatasync가 네트워크 스레드를 블로킹하는 문제(1-5 실측 발견) 개선용.
// GW2_CrossPlatformCore/Thread/JobQueue는 재실측 중 Lock::WriteUnlock() 버그로 크로스 스레드
// 사용 시 크래시하는 게 확인돼(2026-07-27, 이 파일은 검증된 코드라 수정하지 않기로 결정)
// APM_Agent 자체 WorkerQueue(std::mutex/condition_variable만 사용)로 대체.
#include "WorkerQueue.h"

namespace
{
    constexpr unsigned short PORT = 9000;
    // SQLite: 파일 경로 / TimescaleDB: libpq 연결 문자열("host=... dbname=... user=...")
    constexpr const char* STORAGE_CONNECTION_INFO = "apm_metrics.db";
}

int main()
{
    // std::cout이 C stdio(printf 등)와 동기화되지 않게 함 - 이 코드베이스는 std::cout만 쓰고
    // C stdio는 안 써서 순서 꼬임 리스크 없음, << 연산의 불필요한 동기화 오버헤드만 제거.
    std::ios::sync_with_stdio(false);

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
        WorkerQueue metricStoreQueue;

        // 콘솔 로깅 병목(1-7-b 300-agent 재실측에서 발견) 개선용 - std::cout 조립/출력 자체를
        // 네트워크 스레드에서 떼어내는 전용 워커 큐. metricStoreQueue와 분리한 이유: 로그 flush
        // 지연이 메트릭 저장(또는 그 반대)을 밀리게 하지 않도록 책임을 나눔.
        WorkerQueue consoleLogQueue;

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
            [storePtr, &metricStoreQueue, &pendingMetrics, &consoleLogQueue](const apm::Metric& pkt)
            {
                // Collector 안에서 "트랜잭션"이라 부를 만한 지점 중 가장 자연스러운 곳 -
                // Agent가 보낸 메트릭 패킷 하나를 받아 저장하는 구간(2026-07-26 4순위 데모 계측).
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                // store->Store(pkt) 직접 호출(동기, fdatasync 블로킹 포함) 대신 워커 스레드로 위임.
                // pkt은 값 복사로 캡처 - 비동기 실행 시점까지 살아있어야 함.
                metricStoreQueue.Push([storePtr, pkt]() { storePtr->Store(pkt); });

                pendingMetrics.push_back(pkt);

                // 콘솔 로그 조립+출력을 네트워크 스레드에서 떼어내 별도 워커로 위임(2순위 채택).
                // std::endl 대신 '\n' 사용 - 워커 스레드 안에서도 매번 강제 flush하면 로그 1건
                // 처리 비용 자체는 안 줄어, 유입 속도가 처리 속도를 앞지를 때 WorkerQueue 내부
                // std::queue(무제한)에 처리 못 한 작업이 계속 쌓여 메모리가 늘어나는 리스크가
                // 있음(1-7-d WAL 검증 교훈과 동일 - 스레드 이동은 "누가 블로킹되는가"만 바꿈).
                // '\n'으로 워커 처리 비용 자체를 낮춰 큐 적체 위험을 줄임.
                // 저장이 이제 비동기라 이 시점엔 아직 안 끝났을 수 있음 - "stored"는 부정확한 표현이라 정정.
                consoleLogQueue.Push([pkt]()
                {
                    std::cout << "[Collector] metric received: cpu=" << pkt.cpu_usage_percent()
                        << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                        << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                        << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                        << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                        << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                        << ",cwnd:" << pkt.tcp_snd_cwnd()
                        << '\n';
                });
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
                        // 네트워크 스레드가 cout을 직접 건드리지 않게 consoleLogQueue로 위임 -
                        // sync_with_stdio(false) 상태에서 여러 스레드가 동시에 cout/cerr에 쓰면
                        // 내부 버퍼가 레이스로 깨질 수 있음(재실측 중 발견, 진단 로그가 실제로
                        // 스레드 간 뒤섞여 깨지는 걸 확인). consoleLogQueue 워커 스레드 하나만
                        // 런타임 중 스트림을 쓰도록 통일해 레이스를 원천 차단.
                        consoleLogQueue.Push([]() { std::cout << "[Collector] connection accepted\n"; });
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::string errMsg = ec.message();
                        consoleLogQueue.Push([errMsg]() { std::cerr << "[Collector] accept error : " << errMsg << '\n'; });
                    }
                    doAccept();
                });
        };
        doAccept();

        // pendingMetrics/SpanRecorder에 쌓인 걸 전부 WebServer로 보내고 비움 - 주기 타이머와
        // CLI 트리거 둘 다 이 함수 하나를 호출함(로직 중복 방지). span 전송을 여기 얹은 이유:
        // 이미 "주기적으로 WebServer에 밀어넣는" 책임을 지고 있는 함수라 새 타이머를 또
        // 만들 필요가 없음(2026-07-26 4순위 설계).
        auto flushToWebServer = [&pendingMetrics, &webServerSender, &consoleLogQueue]()
        {
            if (!pendingMetrics.empty())
            {
                // consoleLogQueue로 위임(위 accept 핸들러와 같은 이유) - pendingMetrics는 이 직후
                // clear()되므로 크기를 미리 값으로 캡처(워커 스레드 실행 시점엔 이미 비어있을 수 있음).
                size_t count = pendingMetrics.size();
                consoleLogQueue.Push([count]() { std::cout << "[Collector] WebServer로 " << count << "건 전송 시도\n"; });
                for (const auto& m : pendingMetrics)
                    webServerSender.Enqueue(m);

                pendingMetrics.clear();
            }

            auto spans = SpanRecorder::Instance().DrainAll();
            if (!spans.empty())
            {
                size_t spanCount = spans.size();
                consoleLogQueue.Push([spanCount]() { std::cout << "[Collector] WebServer로 span " << spanCount << "건 전송 시도\n"; });
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