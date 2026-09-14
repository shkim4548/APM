#pragma once
#include "pch.h"

// step Phase A : Agent가 스스로 알림을 판단하기 위한 임계치 보관소.
// 지금은 하드코딩 값으로 1회 초기화만 하고 Update()는 아무도 안 부른다(Phase B 시임 —
// Console→Agent 통지 구현 시 그 핸들러가 이 메서드를 호출하도록 연결하면 됨).
// Console의 AlertMetricType과 값 의미를 그대로 맞춤(0=Cpu, 1=Memory, 2=Disk, 3=TcpRttUs).
enum class AlertMetricType : int
{
    CpuPercent = 0,
    MemoryPercent = 1,
    DiskPercent = 2,
    TcpRttUs = 3,
};

class ThresholdSet
{
public:
    ThresholdSet();

    double Get(AlertMetricType type) const;

    // Phase B 시임 - 지금은 어디서도 호출하지 않는다.
    void Update(AlertMetricType type, double value);

private:
    mutable std::mutex _mutex;
    std::array<double, 4> _values;
};
