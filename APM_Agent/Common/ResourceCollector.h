#pragma once
#include "pch.h"

/*-----------------
	SystemMetrics
-------------------*/

struct SystemMetrics
{
	double cpuUsagePercent = 0.0;   // 0~100, 최초 1회 호출은 이전 샘플이 없어 0으로 나옴
	uint64 memTotalBytes = 0;
	uint64 memUsedBytes = 0;
	uint64 diskTotalBytes = 0;
	uint64 diskUsedBytes = 0;
	uint64 netRxBytesPerSec = 0;
	uint64 netTxBytesPerSec = 0;
};

/*---------------------
	ResourceCollector
-----------------------*/
// 로컬 시스템 리소스(CPU/메모리/디스크/네트워크)를 수집한다.
// Linux: /proc 가상 파일시스템 기반. Windows: GetSystemTimes/GlobalMemoryStatusEx/
// GetDiskFreeSpaceEx/GetIfTable2(WinAPI) 기반 - 플랫폼별 구현은 .cpp에서 #ifdef _WIN32로 분기.
// CPU 사용률은 누적 값의 두 시점 간 델타로 계산하므로,
// 인스턴스 생성 후 첫 Collect() 호출은 유효한 CPU% 값을 낼 수 없음(이전 샘플 없음).
// 주기적으로 Collect()를 반복 호출하는 용도로 설계됨 (Phase 6-2 스케줄러와 결합 예정).

class ResourceCollector
{
public:
	SystemMetrics Collect();

private:
	double ComputeCpuUsage();
	uint64 GetMemTotal();
	uint64 GetMemUsed();
	void GetDiskUsage(uint64& totalBytes, uint64& usedBytes, const String& path);
	void ComputeNetworkUsage(uint64& rxBytesPerSec, uint64& txBytesPerSec);

private:
	bool _hasPrevSample = false;
	uint64 _prevIdle = 0;
	uint64 _prevTotal = 0;

	bool _hasPrevNetSample = false;
	uint64 _prevRxBytes = 0;
	uint64 _prevTxBytes = 0;
	std::chrono::steady_clock::time_point _prevNetSampleTime;
};
