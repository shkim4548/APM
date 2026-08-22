#pragma once
#include <string>
#include <vector>

// sqlite3.h는 아직 프로젝트에 없음(sqlite3/ 폴더에 앰알거메이션 추가 후 .cpp에서 include 예정) -
// 헤더는 불투명 포인터로만 다뤄서, 이 헤더를 include하는 쪽(ApmViewerDlg.h 등)이
// sqlite3.h 존재 여부와 무관하게 컴파일되게 함.
struct sqlite3;

class SqliteReader
{
public:
    explicit SqliteReader(const std::wstring& dbPath);
    ~SqliteReader();

    struct MetricRow
    {
        long long ts = 0;
        double cpuUsagePercent = 0;
        long long memUsedBytes = 0;
        long long memTotalBytes = 0;
        long long diskUsedBytes = 0;
        long long diskTotalBytes = 0;
        long long netRxBytesPerSec = 0;
        long long netTxBytesPerSec = 0;
        int tcpRttUs = 0;
        int tcpRetransmits = 0;
    };

    // TODO: "SELECT ... FROM metrics ORDER BY ts DESC LIMIT ?" 로 최신 N개 조회해서 반환.
    // 원칙: 읽기 전용(SQLITE_OPEN_READONLY)으로만 열고, sqlite3_busy_timeout으로 잠금 대기,
    // 그래도 실패하면 예외 던지지 말고 빈 벡터 반환(호출부가 상태 텍스트만 갱신하도록).
    std::vector<MetricRow> FetchLatest(int limit);

private:
    sqlite3* _db = nullptr;
};
