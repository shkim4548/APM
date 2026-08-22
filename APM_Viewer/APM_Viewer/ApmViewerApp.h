#pragma once
#include "resource.h"

class CApmViewerApp : public CWinApp
{
public:
    CApmViewerApp();

public:
    virtual BOOL InitInstance();

    DECLARE_MESSAGE_MAP()
};

extern CApmViewerApp theApp;
