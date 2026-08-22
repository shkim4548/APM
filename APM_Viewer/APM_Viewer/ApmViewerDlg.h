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

    // TODO: 폴링 주기(ms)/타이머 ID 등 필요한 상수·상태는 여기 멤버로 추가
};
