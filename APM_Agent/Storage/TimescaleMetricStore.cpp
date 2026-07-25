#include "pch.h"
#include "TimescaleMetricStore.h"
#include <cstdio>

TimescaleMetricStore::TimescaleMetricStore(const String& connectionString)
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
