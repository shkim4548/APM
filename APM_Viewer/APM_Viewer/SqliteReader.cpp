#include "pch.h"
#include "SqliteReader.h"
#include "sqlite3/sqlite3.h"
#include <stdexcept>

// TODO: sqlite3.c/sqlite3.h를 sqlite3/ 폴더에 넣은 뒤 여기서 include
// #include "sqlite3/sqlite3.h"

SqliteReader::SqliteReader(const std::wstring& dbPath)
{
    // TODO: sqlite3_open_v2(..., SQLITE_OPEN_READONLY, nullptr)로 읽기 전용 오픈
    // TODO: sqlite3_busy_timeout(_db, 3000) 설정
    // TODO: open 실패 시 예외를 던져서 호출부(OnInitDialog)가 "DB 없음" 상태로 처리하게 함
    // 데모 범위 : 경로가 ASCII라고 가정
    std::string narrowPath(dbPath.begin(), dbPath.end());

    int rc = ::sqlite3_open_v2(narrowPath.c_str(), &_db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK)
    {
        std::string err = _db ? ::sqlite3_errmsg(_db) : "unknown";
        if (_db)
        {
            ::sqlite3_close(_db);
        }
        _db = nullptr;
        throw std::runtime_error("SqliteReader - opne failed : " + err);
    }
    ::sqlite3_busy_timeout(_db, 3000);
}

SqliteReader::~SqliteReader()
{
    // TODO: _db가 열려 있으면 sqlite3_close(_db)
    if (_db)
        ::sqlite3_close(_db);
}

std::vector<SqliteReader::MetricRow> SqliteReader::FetchLatest(int limit)
{
    // TODO: sqlite3_prepare_v2 -> sqlite3_bind_int(limit) -> sqlite3_step 루프 -> MetricRow 채우기
    // TODO: SQLITE_BUSY 등 실패 시 예외 던지지 말고 빈 벡터 반환
    std::vector<MetricRow> rows;

    if (!_db)
        return rows;

    constexpr const char* SQL =
        "SELECT ts, cpu_usage_percent, mem_used_bytes, mem_total_bytes, "
        "       disk_used_bytes, disk_total_bytes, net_rx_bytes_per_sec, net_tx_bytes_per_sec, "
        "       tcp_rtt_us, tcp_retransmits "
        "FROM metrics ORDER BY ts DESC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    if (::sqlite3_prepare_v2(_db, SQL, -1, &stmt, nullptr) != SQLITE_OK)
        return rows;

    ::sqlite3_bind_int(stmt, 1, limit);

    while (::sqlite3_step(stmt) == SQLITE_ROW)
    {
        MetricRow row;
        row.ts = ::sqlite3_column_int64(stmt, 0);
        row.cpuUsagePercent = ::sqlite3_column_double(stmt, 1);
        row.memUsedBytes = ::sqlite3_column_int64(stmt, 2);
        row.memTotalBytes = ::sqlite3_column_int64(stmt, 3);
        row.diskUsedBytes = ::sqlite3_column_int64(stmt, 4);
        row.diskTotalBytes = ::sqlite3_column_int64(stmt, 5);
        row.netRxBytesPerSec = ::sqlite3_column_int64(stmt, 6);
        row.netTxBytesPerSec = ::sqlite3_column_int64(stmt, 7);
        row.tcpRttUs = ::sqlite3_column_int(stmt, 8);
        row.tcpRetransmits = ::sqlite3_column_int(stmt, 9);
        rows.push_back(row);
    }

    ::sqlite3_finalize(stmt);
    return rows;
}
