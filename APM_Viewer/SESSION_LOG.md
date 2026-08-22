# APM_Viewer SESSION_LOG

코드가 포함된 답변을 여기에 기록(CLAUDE.md 규칙 6). 최신 항목은 아래쪽에 시간순으로 쌓는다.
실제 프로젝트 파일(`APM_Viewer/SqliteReader.cpp`, `APM_Viewer/ApmViewerDlg.cpp`)은 학습을 위해
TODO가 있는 스켈레톤 상태로 남겨두고, 여기 적힌 완성 예제를 보면서 직접 타이핑해 넣는 용도.

---

## 2026-08-22 — SqliteReader / ApmViewerDlg 완성 예제

스켈레톤(`ApmViewerDlg.h/.cpp`, `SqliteReader.h/.cpp`)의 TODO를 채운 참고용 완성 코드.
`sqlite3/` 폴더에 앰알거메이션(`sqlite3.c`/`sqlite3.h`)이 이미 들어가 있고, `.vcxproj`에
`sqlite3.c`가 추가돼 있다는 전제(`sqlite3/PLACEHOLDER.txt` 절차 참고).

### SqliteReader.h — 변경 없음(구조 그대로), 참고로 다시 적음

```cpp
#pragma once
#include <string>
#include <vector>

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

    std::vector<MetricRow> FetchLatest(int limit);

private:
    sqlite3* _db = nullptr;
};
```

### SqliteReader.cpp — 완성본

읽기 전용 오픈(`SQLITE_OPEN_READONLY`) + `busy_timeout`으로 잠금 대기, 실패해도 예외만
던지고(생성자) 조회 실패는 빈 벡터 반환(`FetchLatest`) — 호출부가 상태 텍스트만 갱신하도록.

```cpp
#include "pch.h"
#include "SqliteReader.h"
#include "sqlite3/sqlite3.h"
#include <stdexcept>

SqliteReader::SqliteReader(const std::wstring& dbPath)
{
    // 데모 범위 - 경로가 ASCII라고 가정(한글 경로 쓸 거면 WideCharToMultiByte로 UTF-8 변환 필요).
    std::string narrowPath(dbPath.begin(), dbPath.end());

    int rc = ::sqlite3_open_v2(narrowPath.c_str(), &_db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK)
    {
        std::string err = _db ? ::sqlite3_errmsg(_db) : "unknown";
        if (_db)
            ::sqlite3_close(_db);
        _db = nullptr;
        throw std::runtime_error("SqliteReader - open failed: " + err);
    }

    ::sqlite3_busy_timeout(_db, 3000);
}

SqliteReader::~SqliteReader()
{
    if (_db)
        ::sqlite3_close(_db);
}

std::vector<SqliteReader::MetricRow> SqliteReader::FetchLatest(int limit)
{
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
        row.ts              = ::sqlite3_column_int64(stmt, 0);
        row.cpuUsagePercent  = ::sqlite3_column_double(stmt, 1);
        row.memUsedBytes     = ::sqlite3_column_int64(stmt, 2);
        row.memTotalBytes    = ::sqlite3_column_int64(stmt, 3);
        row.diskUsedBytes    = ::sqlite3_column_int64(stmt, 4);
        row.diskTotalBytes   = ::sqlite3_column_int64(stmt, 5);
        row.netRxBytesPerSec = ::sqlite3_column_int64(stmt, 6);
        row.netTxBytesPerSec = ::sqlite3_column_int64(stmt, 7);
        row.tcpRttUs         = ::sqlite3_column_int(stmt, 8);
        row.tcpRetransmits   = ::sqlite3_column_int(stmt, 9);
        rows.push_back(row);
    }

    ::sqlite3_finalize(stmt);
    return rows;
}
```

### ApmViewerDlg.h — 변경 없음(구조 그대로), 참고로 다시 적음

```cpp
#pragma once
#include "SqliteReader.h"
#include <memory>

class CApmViewerDlg : public CDialogEx
{
public:
    explicit CApmViewerDlg(CWnd* pParent = nullptr);

#ifdef AFX_DESIGN_TIME
    enum { IDD = IDD_APM_VIEWER_DIALOG };
#endif

protected:
    virtual void DoDataExchange(CDataExchange* pDX);

    virtual BOOL OnInitDialog();
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnDestroy();
    DECLARE_MESSAGE_MAP()

private:
    CListCtrl m_listMetrics;
    CStatic m_staticStatus;
    std::unique_ptr<SqliteReader> m_reader;
};
```

### ApmViewerDlg.cpp — 완성본

`OnInitDialog`에서 리스트 컬럼을 세팅하고 DB를 열어봄(실패해도 죽지 않음). `SetTimer`로
2.5초 폴링 시작. `OnTimer`에서 매번 `m_reader`가 없으면 재오픈을 시도하고(Collector를
나중에 켜도 따라잡도록), 있으면 `FetchLatest`로 조회해서 리스트를 갱신한다.

```cpp
#include "pch.h"
#include "ApmViewerApp.h"
#include "ApmViewerDlg.h"
#include <ctime>

namespace
{
    // Collector가 실제로 쓰는 DB 경로(HOW_TO_RUN.md 기준) - 필요하면 여기만 바꾸면 됨.
    constexpr const wchar_t* DB_PATH = L"C:\\Dev\\APM\\APM_Agent\\apm_metrics.db";

    constexpr UINT_PTR TIMER_ID_POLL = 1;
    constexpr UINT TIMER_INTERVAL_MS = 2500;
    constexpr int FETCH_LIMIT = 20;
}

CApmViewerDlg::CApmViewerDlg(CWnd* pParent)
    : CDialogEx(IDD_APM_VIEWER_DIALOG, pParent)
{
}

void CApmViewerDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_LIST_METRICS, m_listMetrics);
    DDX_Control(pDX, IDC_STATIC_STATUS, m_staticStatus);
}

BEGIN_MESSAGE_MAP(CApmViewerDlg, CDialogEx)
    ON_WM_TIMER()
    ON_WM_DESTROY()
END_MESSAGE_MAP()

BOOL CApmViewerDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    m_listMetrics.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    m_listMetrics.InsertColumn(0, _T("시각"), LVCFMT_LEFT, 70);
    m_listMetrics.InsertColumn(1, _T("CPU%"), LVCFMT_RIGHT, 50);
    m_listMetrics.InsertColumn(2, _T("MEM"), LVCFMT_RIGHT, 60);
    m_listMetrics.InsertColumn(3, _T("DISK"), LVCFMT_RIGHT, 60);
    m_listMetrics.InsertColumn(4, _T("NET rx/tx"), LVCFMT_RIGHT, 90);
    m_listMetrics.InsertColumn(5, _T("TCP RTT(us)"), LVCFMT_RIGHT, 80);

    try
    {
        m_reader = std::make_unique<SqliteReader>(DB_PATH);
        m_staticStatus.SetWindowText(_T("상태: 연결됨"));
    }
    catch (const std::exception&)
    {
        m_reader.reset();
        m_staticStatus.SetWindowText(_T("상태: DB 파일 없음 - 재시도 대기중"));
    }

    SetTimer(TIMER_ID_POLL, TIMER_INTERVAL_MS, nullptr);

    return TRUE;
}

void CApmViewerDlg::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent == TIMER_ID_POLL)
    {
        if (!m_reader)
        {
            try
            {
                m_reader = std::make_unique<SqliteReader>(DB_PATH);
            }
            catch (const std::exception&)
            {
                m_staticStatus.SetWindowText(_T("상태: DB 파일 없음 - 재시도 대기중"));
            }
        }

        if (m_reader)
        {
            auto rows = m_reader->FetchLatest(FETCH_LIMIT);
            if (rows.empty())
            {
                m_staticStatus.SetWindowText(_T("상태: DB 잠김 또는 데이터 없음 - 재시도 중"));
            }
            else
            {
                m_listMetrics.DeleteAllItems();
                int idx = 0;
                for (const auto& row : rows)
                {
                    time_t t = static_cast<time_t>(row.ts);
                    tm localTm{};
                    localtime_s(&localTm, &t);
                    CString timeStr;
                    timeStr.Format(_T("%02d:%02d:%02d"), localTm.tm_hour, localTm.tm_min, localTm.tm_sec);

                    CString cpuStr, memStr, diskStr, netStr, rttStr;
                    cpuStr.Format(_T("%.1f"), row.cpuUsagePercent);
                    memStr.Format(_T("%lld MB"), row.memUsedBytes / (1024 * 1024));
                    diskStr.Format(_T("%lld GB"), row.diskUsedBytes / (1024LL * 1024 * 1024));
                    netStr.Format(_T("%lld/%lld"), row.netRxBytesPerSec, row.netTxBytesPerSec);
                    rttStr.Format(_T("%d"), row.tcpRttUs);

                    int item = m_listMetrics.InsertItem(idx, timeStr);
                    m_listMetrics.SetItemText(item, 1, cpuStr);
                    m_listMetrics.SetItemText(item, 2, memStr);
                    m_listMetrics.SetItemText(item, 3, diskStr);
                    m_listMetrics.SetItemText(item, 4, netStr);
                    m_listMetrics.SetItemText(item, 5, rttStr);
                    ++idx;
                }
                m_staticStatus.SetWindowText(_T("상태: 정상"));
            }
        }
    }

    CDialogEx::OnTimer(nIDEvent);
}

void CApmViewerDlg::OnDestroy()
{
    KillTimer(TIMER_ID_POLL);
    CDialogEx::OnDestroy();
}
```

---

## 2026-08-22 — 메시지 맵 매크로(`BEGIN_MESSAGE_MAP`) 설명

질문한 코드:

```cpp
BEGIN_MESSAGE_MAP(CApmViewerDlg, CDialogEx)
    ON_WM_TIMER()
    ON_WM_DESTROY()
END_MESSAGE_MAP()
```

### 왜 이런 매크로가 필요한가

Windows 메시지(`WM_TIMER`, `WM_DESTROY`, `WM_PAINT`, `WM_COMMAND`... 수백 개)를 C++ 가상 함수로
전부 처리하려면, `CWnd` 하나에 수백 개짜리 vtable이 생기고 그중 대부분은 아무도 안 씀 - 메모리
낭비가 큼. MFC는 그 대신 **클래스마다 정적(static) 테이블 하나**에 "이 메시지가 오면 이 멤버
함수를 호출해라"라는 매핑만 등록해두는 자체 디스패치 메커니즘을 씀 - 이게 메시지 맵.

### 매크로가 실제로 만들어내는 것

`BEGIN_MESSAGE_MAP(CApmViewerDlg, CDialogEx)` ~ `END_MESSAGE_MAP()`는 전처리기 단계에서
대략 이런 코드로 펼쳐짐(실제 매크로 정의는 `afxwin.h`/`afxmsg_.h`):

```cpp
// BEGIN_MESSAGE_MAP(CApmViewerDlg, CDialogEx)가 펼쳐지는 형태 (개념적으로)
const AFX_MSGMAP* CApmViewerDlg::GetMessageMap() const
{
    return &CApmViewerDlg::messageMap;
}

const AFX_MSGMAP CApmViewerDlg::messageMap =
{
    &CDialogEx::messageMap,        // 부모 클래스의 메시지 맵 - 여기서 못 찾으면 이쪽으로 넘어감
    &CApmViewerDlg::_messageEntries[0]
};

const AFX_MSGMAP_ENTRY CApmViewerDlg::_messageEntries[] =
{
    // ON_WM_TIMER()가 펼쳐진 것 - "WM_TIMER 오면 OnTimer 호출해라"
    { WM_TIMER, 0, 0, 0, AfxSig_vwp, (AFX_PMSG)(void (CApmViewerDlg::*)(UINT_PTR))&CApmViewerDlg::OnTimer },

    // ON_WM_DESTROY()가 펼쳐진 것 - "WM_DESTROY 오면 OnDestroy 호출해라"
    { WM_DESTROY, 0, 0, 0, AfxSig_vv, (AFX_PMSG)(void (CApmViewerDlg::*)())&CApmViewerDlg::OnDestroy },

    // END_MESSAGE_MAP()가 펼쳐진 것 - 배열 끝 표시
    { 0, 0, 0, 0, AfxSig_end, (AFX_PMSG)0 }
};
```

(이전 세션에서 `pch.h`에 `afxdialogex.h`를 빠뜨렸을 때 `TheBaseClass`, `messageMap`,
`_messageEntries` 같은 이름으로 에러가 났던 게 바로 이 펼쳐진 코드 - `CDialogEx`가 정의 안 된
상태라 매크로 확장 결과 자체가 깨졌던 것.)

### `ON_WM_TIMER()` / `ON_WM_DESTROY()`가 하는 일

`afxmsg_.h`에 미리 정의된 매크로들로, 각각 위 표에 항목 하나씩을 추가함. 이름 자체가
"어떤 Windows 메시지를, 어떤 이름/시그니처의 멤버 함수와" 연결할지를 고정해서 정해놓은
것이라 - `ON_WM_TIMER()`를 쓰려면 클래스에 반드시 `void OnTimer(UINT_PTR nIDEvent)`가,
`ON_WM_DESTROY()`를 쓰려면 `void OnDestroy()`가 정확히 그 이름/시그니처로 있어야 함
(그래서 헤더에 `afx_msg void OnTimer(UINT_PTR nIDEvent);`처럼 선언해둔 것 - `afx_msg`는
컴파일에는 영향 없는 빈 매크로이고, ClassWizard가 "이건 메시지 핸들러다"라고 소스에서
찾아내기 위한 표식용).

### 런타임에 실제로 벌어지는 일

1. Windows가 우리 다이얼로그 창(HWND)에 `WM_TIMER` 메시지를 보냄
2. 모든 MFC 창이 공유하는 윈도우 프로시저(`AfxWndProc`)가 그 HWND에 연결된 `CWnd*` 객체를
   찾아서 `OnWndMsg()` 호출
3. `OnWndMsg()`가 `GetMessageMap()`으로 받은 `_messageEntries[]`를 순회하며 `WM_TIMER`를
   찾음 - 자기 클래스에 없으면 `pBaseMap`(부모 클래스 메시지 맵)으로 타고 올라가며 찾음
   (C++ 가상함수 상속이 아니라 이 체이닝으로 상속 흉내를 냄)
4. 찾은 엔트리의 함수 포인터를 원래 시그니처로 캐스팅해서 `(this->*pfn)(wParam)` 형태로 호출
   → 결과적으로 우리가 작성한 `OnTimer(nIDEvent)`가 실행됨

### 우리 코드에서 이게 왜 중요한가

`OnInitDialog()`에서 `SetTimer(TIMER_ID_POLL, ...)`를 호출해도, `ON_WM_TIMER()`가 메시지 맵에
없으면 `WM_TIMER`가 와도 아무도 우리 `OnTimer()`로 연결해주지 않음(기본 `CWnd::OnTimer`는
아무것도 안 함) - 그러면 타이머는 "성공적으로 설정됐는데" 폴링 로직은 영원히 실행이 안 되는,
겉으로는 에러 없이 조용히 죽어있는 상태가 됨. `ON_WM_DESTROY()`도 마찬가지로 없으면
`OnDestroy()`가 안 불려서 `KillTimer()`가 실행이 안 됨(다이얼로그가 닫혀도 타이머가 안 정리됨).
