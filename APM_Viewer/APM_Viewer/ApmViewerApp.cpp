#include "pch.h"
#include "ApmViewerApp.h"
#include "ApmViewerDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CApmViewerApp, CWinApp)
END_MESSAGE_MAP()

CApmViewerApp::CApmViewerApp()
{
}

CApmViewerApp theApp;

BOOL CApmViewerApp::InitInstance()
{
    CWinApp::InitInstance();

    CApmViewerDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();

    // 모달 다이얼로그가 닫히면 바로 종료 - 메시지 루프를 더 돌릴 필요 없음(FALSE 반환 관례).
    return FALSE;
}
