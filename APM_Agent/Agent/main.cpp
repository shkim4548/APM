#include "pch.h"
#include "ResilientSender.h"
#include "ResourceCollector.h"
#include "MetricScheduler.h"
#include "KeyLoader.h"
#include "AesGcmPayload.h"
#include "Protocol/Metric.pb.h"
#include "ThresholdSet.h"
#include "LocalAlertEvaluator.h"
#include "AgentControlServer.h"
#include "Storage/AgentAlertStore.h"

namespace
{
	constexpr const char* COLLECTOR_HOST = "127.0.0.1";
	constexpr unsigned short COLLECTOR_PORT = 9000;
	// Phase A : Qt가 붙는 로컬 제어 소켓 경로. 장비당 Agent 하나라는 전제라 고정 경로로 충분.
	constexpr const char* CONTROL_SOCKET_PATH = "/tmp/apm_agent.sock";
	constexpr const char* ALERT_DB_PATH = "agent_alerts.db";
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
		AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");

		asio::io_context ioContext;

		asio::ssl::context sslContext(asio::ssl::context::tls_client);
		// 테스트용 자체 서명 인증서라 CA 검증 생략. 프로덕션에서는 절대 금지 -
		// 실제로는 Collector의 CA 인증서를 신뢰 목록에 등록해서 검증해야 함.
		sslContext.set_verify_mode(asio::ssl::verify_none);

		ResilientSender sender(ioContext, sslContext, COLLECTOR_HOST, COLLECTOR_PORT,
			[agentCollectorKey]() 
			{
				return std::make_unique<AesGcmPayload>(agentCollectorKey); 
			});

		// Phase A : 로컬 알림 판단. Collector에 apm_metrics.db가 이미 있으니 지표는 중복
		// 저장하지 않고, 이 Agent 자신의 알림 이력만 별도 파일에 남긴다.
		ThresholdSet thresholds;
		AgentAlertStore alertStore(ALERT_DB_PATH);
		LocalAlertEvaluator alertEvaluator(thresholds, alertStore);

		MetricScheduler scheduler(ioContext, std::chrono::seconds(5),
			[&sender, &alertEvaluator](const SystemMetrics& metrics)
			{
				apm::Metric pkt;
				pkt.set_cpu_usage_percent(metrics.cpuUsagePercent);
				pkt.set_mem_used_bytes(metrics.memUsedBytes);
				pkt.set_mem_total_bytes(metrics.memTotalBytes);
				pkt.set_disk_used_bytes(metrics.diskUsedBytes);
				pkt.set_disk_total_bytes(metrics.diskTotalBytes);
				pkt.set_net_rx_bytes_per_sec(metrics.netRxBytesPerSec);
				pkt.set_net_tx_bytes_per_sec(metrics.netTxBytesPerSec);

				TcpConnectionInfo tcpInfo = sender.GetConnectionInfo();
				pkt.set_tcp_rtt_us(tcpInfo.rttMicros);
				pkt.set_tcp_rtt_var_us(tcpInfo.rttVarMicros);
				pkt.set_tcp_retransmits(tcpInfo.retransmits);
				pkt.set_tcp_total_retrans(tcpInfo.totalRetrans);
				pkt.set_tcp_snd_cwnd(tcpInfo.sndCwnd);

				// Collector 전송 "전에" 로컬 판단 먼저 - Agent<->Collector 연결이 끊긴
				// 상태에서도 로컬 알림은 항상 최신 상태를 유지하도록.
				alertEvaluator.OnNewMetric(pkt);
				sender.Enqueue(pkt);
			});
		scheduler.Start();

		AgentControlServer controlServer(ioContext, CONTROL_SOCKET_PATH, scheduler, sender);
		controlServer.Start();

		ioContext.run();
	}
	catch (const std::exception& e)
	{
		std::cerr << "[Agent] fatal: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}