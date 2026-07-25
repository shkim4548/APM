#pragma once
#include "pch.h"
#include "IMetricStore.h"
#include "DBConnection.h"   // GW2_CrossPlatformCore/DB - 구 Phase 4에서 만든 libpq 래퍼 재사용

/*----------------------
	TimescaleMetricStore
------------------------*/
// TimescaleDB(Postgres 확장) 백엔드. 연결/쿼리 실행은 기존 DBConnection에 위임.

class TimescaleMetricStore : public IMetricStore
{
public:
	explicit TimescaleMetricStore(const String& connectionString);

	void Store(const apm::Metric& metric) override;

private:
	DBConnection _connection;
};