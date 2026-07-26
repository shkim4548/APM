#include "pch.h"
#include "SqliteMetricStore.h"

namespace
{
	constexpr const char* CREATE_TABLE_SQL =
		"CREATE TABLE IF NOT EXISTS metrics ("
		"  ts INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
		"  cpu_usage_percent REAL,"
		"  mem_used_bytes INTEGER,"
		"  mem_total_bytes INTEGER,"
		"  disk_used_bytes INTEGER,"
		"  disk_total_bytes INTEGER,"
		"  net_rx_bytes_per_sec INTEGER,"
		"  net_tx_bytes_per_sec INTEGER,"
		"  tcp_rtt_us INTEGER,"
		"  tcp_retransmits INTEGER"
		");";

	constexpr const char* INSERT_SQL =
		"INSERT INTO metrics "
		"(cpu_usage_percent, mem_used_bytes, mem_total_bytes, disk_used_bytes, disk_total_bytes, "
		" net_rx_bytes_per_sec, net_tx_bytes_per_sec, tcp_rtt_us, tcp_retransmits) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";

	// retentionDays를 바인딩 파라미터로 받음(문자열 조립 없이) - 삭제 기준을 매번
	// "지금 - N일"로 재계산(2026-07-26 3순위 설계).
	constexpr const char* PRUNE_SQL =
		"DELETE FROM metrics WHERE ts < strftime('%s','now') - (? * 86400);";
}

SqliteMetricStore::SqliteMetricStore(const String& dbPath)
{
	if (::sqlite3_open(dbPath.c_str(), &_db) != SQLITE_OK)
		throw std::runtime_error("SqliteMetricStore - open failed: " + String(::sqlite3_errmsg(_db)));

	// DELETE만으로는 SQLite 파일 크기가 줄지 않음(빈 페이지가 파일 내부에서 재사용될 뿐 OS에
	// 반환되지 않음) - incremental_vacuum 모드로 열어두면 Prune() 직후 PRAGMA incremental_vacuum
	// 한 번으로 빈 페이지를 점진적으로 반환할 수 있음(풀 VACUUM처럼 테이블 전체를 오래 잠그지
	// 않음). auto_vacuum 모드는 빈 DB에만 적용되므로 CREATE TABLE보다 먼저 설정해야 함 - 이미
	// auto_vacuum=NONE으로 만들어진 기존 apm_metrics.db 파일에는 소급 적용 안 됨(2026-07-26 확인,
	// 기존 파일 전환은 이번 설계 범위 밖 - 필요시 수동 VACUUM 한 번 또는 파일 재생성).
	::sqlite3_exec(_db, "PRAGMA auto_vacuum = INCREMENTAL;", nullptr, nullptr, nullptr);

	char* errMsg = nullptr;
	if (::sqlite3_exec(_db, CREATE_TABLE_SQL, nullptr, nullptr, &errMsg) != SQLITE_OK)
	{
		String err = errMsg ? errMsg : "unknown";
		::sqlite3_free(errMsg);
		throw std::runtime_error("SqliteMetricStore - CREATE TABLE failed: " + err);
	}

	if (::sqlite3_prepare_v2(_db, INSERT_SQL, -1, &_insertStmt, nullptr) != SQLITE_OK)
		throw std::runtime_error("SqliteMetricStore - prepare failed: " + String(::sqlite3_errmsg(_db)));
}

SqliteMetricStore::~SqliteMetricStore()
{
	if (_insertStmt)
		::sqlite3_finalize(_insertStmt);
	if (_db)
		::sqlite3_close(_db);
}

void SqliteMetricStore::Store(const apm::Metric& metric)
{
	::sqlite3_reset(_insertStmt);
	::sqlite3_bind_double(_insertStmt, 1, metric.cpu_usage_percent());
	::sqlite3_bind_int64(_insertStmt, 2, static_cast<sqlite3_int64>(metric.mem_used_bytes()));
	::sqlite3_bind_int64(_insertStmt, 3, static_cast<sqlite3_int64>(metric.mem_total_bytes()));
	::sqlite3_bind_int64(_insertStmt, 4, static_cast<sqlite3_int64>(metric.disk_used_bytes()));
	::sqlite3_bind_int64(_insertStmt, 5, static_cast<sqlite3_int64>(metric.disk_total_bytes()));
	::sqlite3_bind_int64(_insertStmt, 6, static_cast<sqlite3_int64>(metric.net_rx_bytes_per_sec()));
	::sqlite3_bind_int64(_insertStmt, 7, static_cast<sqlite3_int64>(metric.net_tx_bytes_per_sec()));
	::sqlite3_bind_int(_insertStmt, 8, static_cast<int>(metric.tcp_rtt_us()));
	::sqlite3_bind_int(_insertStmt, 9, static_cast<int>(metric.tcp_retransmits()));

	if (::sqlite3_step(_insertStmt) != SQLITE_DONE)
		std::cerr << "[SqliteMetricStore] insert failed: " << ::sqlite3_errmsg(_db) << std::endl;
}

void SqliteMetricStore::Prune(int retentionDays)
{
	sqlite3_stmt* pruneStmt = nullptr;
	if (::sqlite3_prepare_v2(_db, PRUNE_SQL, -1, &pruneStmt, nullptr) != SQLITE_OK)
	{
		std::cerr << "[SqliteMetricStore] prune prepare failed: " << ::sqlite3_errmsg(_db) << std::endl;
		return;
	}

	::sqlite3_bind_int(pruneStmt, 1, retentionDays);

	if (::sqlite3_step(pruneStmt) != SQLITE_DONE)
		std::cerr << "[SqliteMetricStore] prune failed: " << ::sqlite3_errmsg(_db) << std::endl;
	else
		std::cout << "[SqliteMetricStore] prune 완료: " << ::sqlite3_changes(_db)
			<< "건 삭제 (retention=" << retentionDays << "일)" << std::endl;

	::sqlite3_finalize(pruneStmt);

	// 빈 페이지를 점진적으로 OS에 반환 - 호출 1번에 일부만 처리되므로(전체 VACUUM처럼
	// 오래 잠그지 않음) 매 Prune() 호출마다 같이 실행해도 부담이 적음.
	::sqlite3_exec(_db, "PRAGMA incremental_vacuum;", nullptr, nullptr, nullptr);
}