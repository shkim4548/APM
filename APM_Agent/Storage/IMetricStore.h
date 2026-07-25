#pragma once
#include "pch.h"
#include "Protocol/Metric.pb.h"

/*----------------
	IMetricStore
------------------*/
// 메트릭 저장 백엔드 추상 인터페이스. 구현체는 컴파일 타임에 하나만 선택됨
// (CMakeLists.txt의 APM_STORAGE_BACKEND 옵션 참고) - Collector는 이 인터페이스만 알면 됨.
class IMetricStore
{
public:
    virtual ~IMetricStore() = default;
    virtual void Store(const apm::Metric& metric) = 0;
};