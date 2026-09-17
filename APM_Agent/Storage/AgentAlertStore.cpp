#include "pch.h"
#include "AgentAlertStore.h"

namespace
{
constexpr const char* CREATE_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS local_alerts ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  metric_type INTEGER NOT NULL,"
    "  threshold_value REAL,"
    "  trigger_value REAL,"
    "  opened_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "  resolved_value REAL,"
    "  closed_at INTEGER"
    ");";

// step 6' : 1행만 쓰는 상태 테이블 - id를 1로 고정(CHECK)해서 항상 그 한 행만 UPSERT한다.
constexpr const char* CREATE_STATUS_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS agent_status ("
    "  id INTEGER PRIMARY KEY CHECK (id = 1),"
    "  connected INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ");";

constexpr const char* UPDATE_STATUS_SQL =
    "INSERT INTO agent_status (id, connected, updated_at) VALUES (1, ?, strftime('%s','now')) "
    "ON CONFLICT(id) DO UPDATE SET connected = excluded.connected, updated_at = excluded.updated_at;";

constexpr const char* FIND_OPEN_SQL =
    "SELECT id, metric_type, threshold_value, trigger_value, opened_at "
    "FROM local_alerts WHERE metric_type = ? AND closed_at IS NULL "
    "ORDER BY id DESC LIMIT 1;";

constexpr const char* OPEN_SQL =
    "INSERT INTO local_alerts (metric_type, threshold_value, trigger_value) VALUES (?, ?, ?);";

constexpr const char* RESOLVE_SQL =
    "UPDATE local_alerts SET resolved_value = ?, closed_at = strftime('%s','now') WHERE id = ?;";

// SqliteMetricStore.cpp와 동일한 헬퍼 - PRAGMA 적용 결과 확인용.
int CapturePragmaResult(void* out, int columnCount, char** columnValues, char**)
{
    if (out && columnCount > 0 && columnValues[0])
        *static_cast<String*>(out) = columnValues[0];
    return 0;
}
}

AgentAlertStore::AgentAlertStore(const String& dbPath)
{
    if (::sqlite3_open(dbPath.c_str(), &_db) != SQLITE_OK)
        throw std::runtime_error("AgentAlertStore - open failed: " + String(::sqlite3_errmsg(_db)));

    // SqliteMetricStore와 같은 이유(fdatasync 병목 회피) - Qt가 동시에 읽어도 안전해야 함.
    String journalMode;
    ::sqlite3_exec(_db, "PRAGMA journal_mode = WAL;", CapturePragmaResult, &journalMode, nullptr);
    if (journalMode != "wal")
        std::cerr << "[AgentAlertStore] WAL 모드 전환 실패 - 현재 journal_mode=" << journalMode << std::endl;
    ::sqlite3_exec(_db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);

    char* errMsg = nullptr;
    if (::sqlite3_exec(_db, CREATE_TABLE_SQL, nullptr, nullptr, &errMsg) != SQLITE_OK)
    {
        String err = errMsg ? errMsg : "unknown";
        ::sqlite3_free(errMsg);
        throw std::runtime_error("AgentAlertStore - CREATE TABLE failed: " + err);
    }
    if (::sqlite3_exec(_db, CREATE_STATUS_TABLE_SQL, nullptr, nullptr, &errMsg) != SQLITE_OK)
    {
        String err = errMsg ? errMsg : "unknown";
        ::sqlite3_free(errMsg);
        throw std::runtime_error("AgentAlertStore - CREATE agent_status TABLE failed: " + err);
    }

    if (::sqlite3_prepare_v2(_db, FIND_OPEN_SQL, -1, &_findOpenStmt, nullptr) != SQLITE_OK
        || ::sqlite3_prepare_v2(_db, OPEN_SQL, -1, &_openStmt, nullptr) != SQLITE_OK
        || ::sqlite3_prepare_v2(_db, RESOLVE_SQL, -1, &_resolveStmt, nullptr) != SQLITE_OK
        || ::sqlite3_prepare_v2(_db, UPDATE_STATUS_SQL, -1, &_updateStatusStmt, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error("AgentAlertStore - prepare failed: " + String(::sqlite3_errmsg(_db)));
    }
}

AgentAlertStore::~AgentAlertStore()
{
    if (_findOpenStmt) ::sqlite3_finalize(_findOpenStmt);
    if (_openStmt) ::sqlite3_finalize(_openStmt);
    if (_resolveStmt) ::sqlite3_finalize(_resolveStmt);
    if (_updateStatusStmt) ::sqlite3_finalize(_updateStatusStmt);
    if (_db) ::sqlite3_close(_db);
}

std::optional<LocalAlertRecord> AgentAlertStore::FindOpen(AlertMetricType type) const
{
    ::sqlite3_reset(_findOpenStmt);
    ::sqlite3_bind_int(_findOpenStmt, 1, static_cast<int>(type));

    if (::sqlite3_step(_findOpenStmt) != SQLITE_ROW)
        return std::nullopt;

    LocalAlertRecord record;
    record.id = ::sqlite3_column_int64(_findOpenStmt, 0);
    record.metricType = ::sqlite3_column_int(_findOpenStmt, 1);
    record.thresholdValue = ::sqlite3_column_double(_findOpenStmt, 2);
    record.triggerValue = ::sqlite3_column_double(_findOpenStmt, 3);
    record.openedAt = ::sqlite3_column_int64(_findOpenStmt, 4);
    return record;
}

void AgentAlertStore::Open(AlertMetricType type, double thresholdValue, double triggerValue)
{
    ::sqlite3_reset(_openStmt);
    ::sqlite3_bind_int(_openStmt, 1, static_cast<int>(type));
    ::sqlite3_bind_double(_openStmt, 2, thresholdValue);
    ::sqlite3_bind_double(_openStmt, 3, triggerValue);

    if (::sqlite3_step(_openStmt) != SQLITE_DONE)
        std::cerr << "[AgentAlertStore] open insert failed: " << ::sqlite3_errmsg(_db) << std::endl;
}

void AgentAlertStore::Resolve(long long id, double resolvedValue)
{
    ::sqlite3_reset(_resolveStmt);
    ::sqlite3_bind_double(_resolveStmt, 1, resolvedValue);
    ::sqlite3_bind_int64(_resolveStmt, 2, id);

    if (::sqlite3_step(_resolveStmt) != SQLITE_DONE)
        std::cerr << "[AgentAlertStore] resolve failed: " << ::sqlite3_errmsg(_db) << std::endl;
}

void AgentAlertStore::UpdateStatus(bool connected)
{
    ::sqlite3_reset(_updateStatusStmt);
    ::sqlite3_bind_int(_updateStatusStmt, 1, connected ? 1 : 0);

    if (::sqlite3_step(_updateStatusStmt) != SQLITE_DONE)
        std::cerr << "[AgentAlertStore] update status failed: " << ::sqlite3_errmsg(_db) << std::endl;
}
