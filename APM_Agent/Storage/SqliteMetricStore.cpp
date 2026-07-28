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

	// sqlite3_exec 콜백 - "PRAGMA journal_mode" 같은 SELECT류 PRAGMA가 반환하는 첫 컬럼 값을
	// out(String* 캐스팅)에 담아 호출자가 실제로 적용됐는지 확인할 수 있게 함.
	int CapturePragmaResult(void* out, int columnCount, char** columnValues, char**)
	{
		if (out && columnCount > 0 && columnValues[0])
			*static_cast<String*>(out) = columnValues[0];
		return 0;
	}
}

SqliteMetricStore::SqliteMetricStore(const String& dbPath)
{
	if (::sqlite3_open(dbPath.c_str(), &_db) != SQLITE_OK)
		throw std::runtime_error("SqliteMetricStore - open failed: " + String(::sqlite3_errmsg(_db)));

	// WAL(Write-Ahead Logging) - 기본 롤백 저널은 INSERT 1건마다 저널 파일을 열고/쓰고/
	// fdatasync로 강제 flush하고/지운다. strace -f로 저장 워커 스레드까지 추적해 실측한 결과
	// fdatasync/pwrite64/fcntl이 전체 syscall 시간의 약 25%를 차지하는 걸 확인한 뒤 도입
	// (2026-07-28, Docs/PROJECT_TECHNICAL_REVIEW.md §7-6). WAL은 커밋마다 새 저널 파일을
	// 만드는 대신 하나의 -wal 파일에 append만 하고, 체크포인트 시점에만 fsync한다.
	// synchronous=NORMAL은 WAL과 짝을 이루는 표준 조합 - 매 커밋마다 fsync하지 않으므로
	// OS 크래시/정전 시 마지막 몇 건의 커밋을 잃을 수 있다(앱 크래시엔 안전 - WAL 자체가
	// 원자적). 메트릭은 계속 흘러들어오는 시계열 관측 데이터라 이 손실 범위를 감내할 수
	// 있다고 판단해 선택. journal_mode는 실패해도 예외를 던지지 않고(레거시 저널로 계속
	// 동작 가능) 로그만 남김 - 저장 자체가 안 되는 것보다 낫다는 판단.
	String journalMode;
	::sqlite3_exec(_db, "PRAGMA journal_mode = WAL;", CapturePragmaResult, &journalMode, nullptr);
	if (journalMode != "wal")
		std::cerr << "[SqliteMetricStore] WAL 모드 전환 실패 - 현재 journal_mode=" << journalMode << std::endl;

	::sqlite3_exec(_db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);

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