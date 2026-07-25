#pragma once
#include "pch.h"
#include "IMetricStore.h"
#include <memory>

// 컴파일 타임에 선택된 백엔드에 맞는 IMetricStore 구현체를 생성.
// connectionInfo의 의미는 백엔드마다 다름 (SQLite: 파일 경로, TimescaleDB: libpq 연결 문자열).
std::unique_ptr<IMetricStore> CreateMetricStore(const String& connectionInfo);