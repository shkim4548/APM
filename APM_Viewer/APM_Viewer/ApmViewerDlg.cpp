#include "pch.h"
#include "ApmViewerApp.h"
#include "ApmViewerDlg.h"
#include <ctime>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

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

    // TODO: m_listMetrics에 컬럼 추가 (InsertColumn) - 시각/CPU/MEM/DISK/NET/TCP RTT 등
    // TODO: m_reader = std::make_unique<SqliteReader>(DB_PATH) - 실패해도 여기서 죽지 않게
    //       예외 처리하고 상태 텍스트만 갱신 (원칙: DB 없어도 앱이 죽으면 안 됨)
    // TODO: SetTimer(TIMER_ID_POLL, TIMER_INTERVAL_MS, nullptr) 로 폴링 시작

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
        m_staticStatus.SetWindowTextW(_T("상태 : DB 파일 없음 - 재시도 대기중"));
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
