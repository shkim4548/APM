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
}

SqliteMetricStore::SqliteMetricStore(const String& dbPath)
{
	if (::sqlite3_open(dbPath.c_str(), &_db) != SQLITE_OK)
		throw std::runtime_error("SqliteMetricStore - open failed: " + String(::sqlite3_errmsg(_db)));

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