#include "pch.h"
#include "TimescaleMetricStore.h"
#include <cstdio>

TimescaleMetricStore::TimescaleMetricStore(const String& connectionString, int retentionDays)
{
	if (!_connection.Connect(connectionString.c_str()))
		throw std::runtime_error("TimescaleMetricStore - DB 연결 실패");

	const char* createSql =
		"CREATE TABLE IF NOT EXISTS metrics ("
		"  ts TIMESTAMPTZ NOT NULL DEFAULT now(),"
		"  cpu_usage_percent DOUBLE PRECISION,"
		"  mem_used_bytes BIGINT,"
		"  mem_total_bytes BIGINT,"
		"  disk_used_bytes BIGINT,"
		"  disk_total_bytes BIGINT,"
		"  net_rx_bytes_per_sec BIGINT,"
		"  net_tx_bytes_per_sec BIGINT,"
		"  tcp_rtt_us INTEGER,"
		"  tcp_retransmits INTEGER"
		");"
		"SELECT create_hypertable('metrics', 'ts', if_not_exists => TRUE);";

	if (!_connection.Execute(createSql, 0, nullptr))
		throw std::runtime_error("TimescaleMetricStore - 테이블/하이퍼테이블 생성 실패");

	// TimescaleDB 네이티브 보존 정책 - 청크(시간 범위 파티션) 단위로 통째로 드롭하므로
	// SqliteMetricStore처럼 행 단위 DELETE보다 훨씬 저렴함. 등록 후엔 TimescaleDB 백그라운드
	// 잡이 알아서 주기 실행 - Collector가 반복 호출할 필요가 없음(Prune()이 no-op인 이유,
	// 2026-07-26 3순위 설계). if_not_exists=>TRUE라 재시작마다 다시 호출해도 안전(중복 등록 안 됨).
	char retentionDaysStr[16];
	std::snprintf(retentionDaysStr, sizeof(retentionDaysStr), "%d", retentionDays);
	const char* policyParams[1] = { retentionDaysStr };
	const char* policySql =
		"SELECT add_retention_policy('metrics', INTERVAL '1 day' * $1::int, if_not_exists => TRUE);";

	if (!_connection.Execute(policySql, 1, policyParams))
		throw std::runtime_error("TimescaleMetricStore - 보존 정책 등록 실패");
}

void TimescaleMetricStore::Store(const apm::Metric& metric)
{
	char cpu[64], memUsed[32], memTotal[32], diskUsed[32], diskTotal[32];
	char netRx[32], netTx[32], rtt[16], retrans[16];

	std::snprintf(cpu, sizeof(cpu), "%f", metric.cpu_usage_percent());
	std::snprintf(memUsed, sizeof(memUsed), "%llu", (unsigned long long)metric.mem_used_bytes());
	std::snprintf(memTotal, sizeof(memTotal), "%llu", (unsigned long long)metric.mem_total_bytes());
	std::snprintf(diskUsed, sizeof(diskUsed), "%llu", (unsigned long long)metric.disk_used_bytes());
	std::snprintf(diskTotal, sizeof(diskTotal), "%llu", (unsigned long long)metric.disk_total_bytes());
	std::snprintf(netRx, sizeof(netRx), "%llu", (unsigned long long)metric.net_rx_bytes_per_sec());
	std::snprintf(netTx, sizeof(netTx), "%llu", (unsigned long long)metric.net_tx_bytes_per_sec());
	std::snprintf(rtt, sizeof(rtt), "%u", metric.tcp_rtt_us());
	std::snprintf(retrans, sizeof(retrans), "%u", metric.tcp_retransmits());

	const char* params[9] = { cpu, memUsed, memTotal, diskUsed, diskTotal, netRx, netTx, rtt, retrans };

	const char* insertSql =
		"INSERT INTO metrics "
		"(cpu_usage_percent, mem_used_bytes, mem_total_bytes, disk_used_bytes, disk_total_bytes, "
		" net_rx_bytes_per_sec, net_tx_bytes_per_sec, tcp_rtt_us, tcp_retransmits) "
		"VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9);";

	if (!_connection.Execute(insertSql, 9, params))
		std::cerr << "[TimescaleMetricStore] insert failed" << std::endl;
}

void TimescaleMetricStore::Prune(int /*retentionDays*/)
{
	// no-op - 생성자에서 등록한 add_retention_policy가 TimescaleDB 백그라운드 워커로
	// 알아서 처리함(위 생성자 주석 참고). Collector 쪽에서 주기 호출은 하지만 여기선 아무것도
	// 안 함 - IMetricStore 인터페이스를 통일하기 위한 형식상의 오버라이드.
}
