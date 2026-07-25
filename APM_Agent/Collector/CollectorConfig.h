#pragma once
#include "pch.h"

/*------------------
    CollectorConfig
--------------------*/
// Collector가 WebServer로 데이터를 보낼 때 쓰는 설정. JSON 파일로 관리 - 차후
// WebServer의 웹 UI에서 이 값을 읽거나 편집하기 쉽도록 미리 대비한 포맷
// (지금은 편집 기능 자체는 구현 범위 밖 - 파일 포맷만 준비).

struct CollectorConfig
{
    String webServerHost = "127.0.0.1";
    unsigned short webServerPort = 9100;
    int pushIntervalSeconds = 10;
};

// JSON 파일을 읽어 설정을 만듦. 파일이 없으면 기본값을 그대로 씀(로컬 데모 시
// 설정 파일 없이도 동작). 파일은 있는데 특정 키가 빠졌으면 그 키만 기본값 유지.
CollectorConfig LoadCollectorConfig(const String& path);
