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

    // retentionDays보다 오래된 행 삭제 - 백엔드별 구현 방식이 다름(SqliteMetricStore는
    // 직접 DELETE, TimescaleMetricStore는 생성자에서 등록한 네이티브 보존 정책이 백그라운드로
    // 알아서 처리하므로 이 함수는 사실상 no-op, 2026-07-26 3순위 설계).
    virtual void Prune(int retentionDays) = 0;
};