#pragma once
#include "pch.h"

/*-------------------
    LoadTesterConfig
---------------------*/
// 커맨드라인 인자를 파싱해서 LoadTester 실행에 필요한 파라미터로 변환.

struct LoadTesterConfig
{
    int agentCount = 10;                                  // --agents
    std::chrono::milliseconds sendInterval{ 100 };        // --interval-ms
    std::chrono::seconds duration{ 60 };                   // --duration-sec
    std::chrono::milliseconds rampUp{ 0 };                 // --ramp-up-ms (0=전체 동시 connect)
    size_t queueSize = 10000;                              // --queue-size (ResilientSender maxQueueSize 오버라이드)
    String collectorHost = "127.0.0.1";                    // --collector-host
    unsigned short collectorPort = 9000;                   // --collector-port
    String csvOutputPath = "load_test_result.csv";         // --csv-out
    String keyFilePath = "certs/agent_collector_aes.key";  // --key-file (Agent/main.cpp와 동일 키 - 다르면 Collector가 복호화 실패)

    // argv 파싱. 실패(알 수 없는 인자/잘못된 값) 시 std::runtime_error -
    // main()에서 사용법 출력 후 종료하는 용도.
    static LoadTesterConfig Parse(int argc, char** argv);
};
