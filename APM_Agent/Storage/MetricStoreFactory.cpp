#include "pch.h"
#include "MetricStoreFactory.h"

#if defined(APM_STORAGE_SQLITE)
#include "SqliteMetricStore.h"
#elif defined(APM_STORAGE_TIMESCALEDB)
#include "TimescaleMetricStore.h"
#else
#error "APM_STORAGE_SQLITE 또는 APM_STORAGE_TIMESCALEDB 중 하나가 정의되어야 함 - CMakeLists.txt의 APM_STORAGE_BACKEND 확인"
#endif

std::unique_ptr<IMetricStore> CreateMetricStore(const String& connectionInfo)
{
#if defined(APM_STORAGE_SQLITE)
	return std::make_unique<SqliteMetricStore>(connectionInfo);
#elif defined(APM_STORAGE_TIMESCALEDB)
	return std::make_unique<TimescaleMetricStore>(connectionInfo);
#endif
}