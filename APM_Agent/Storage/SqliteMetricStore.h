#pragma once
#include "pch.h"
#include "IMetricStore.h"
#include <sqlite3.h>

/*-------------------
	SqliteMetricStore
---------------------*/
// 파일 하나로 동작하는 임베디드 저장소 - 서버 프로세스/네트워크 설정 불필요.

class SqliteMetricStore : public IMetricStore
{
public:
	explicit SqliteMetricStore(const String& dbPath);
	~SqliteMetricStore() override;

	void Store(const apm::Metric& metric) override;

private:
	sqlite3* _db = nullptr;
	sqlite3_stmt* _insertStmt = nullptr;
};
