#include "pch.h"
#include "ApmSession.h"
#include "AesGcmPayload.h"
#include "ResilientSender.h"
#include "PrivilegeDrop.h"
#include "PacketHandler.h"
#include "KeyLoader.h"
#include "CollectorConfig.h"
#include "Protocol/Metric.pb.h"
#include "Storage/MetricStoreFactory.h"
#include <thread>

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

        auto store = CreateMetricStore(STORAGE_CONNECTION_INFO);

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
            [&store, &pendingMetrics](const apm::Metric& pkt)
            {
                store->Store(pkt);
                pendingMetrics.push_back(pkt);
                std::cout << "[Collector] metric stored: cpu=" << pkt.cpu_usage_percent()
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

        // pendingMetrics에 쌓인 걸 전부 WebServer로 보내고 비움 - 주기 타이머와 CLI 트리거
        // 둘 다 이 함수 하나를 호출함(로직 중복 방지).
        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (pendingMetrics.empty())
                return;

            std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
            for (const auto& m : pendingMetrics)
                webServerSender.Enqueue(m);

            pendingMetrics.clear();
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