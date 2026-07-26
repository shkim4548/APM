#pragma once
#include "pch.h"
#include "IMetricStore.h"
#include <memory>

// 컴파일 타임에 선택된 백엔드에 맞는 IMetricStore 구현체를 생성.
// connectionInfo의 의미는 백엔드마다 다름 (SQLite: 파일 경로, TimescaleDB: libpq 연결 문자열).
// retentionDays는 SqliteMetricStore는 그냥 들고 있다가 나중에 Prune() 호출 때 쓰고,
// TimescaleMetricStore는 생성자에서 곧바로 네이티브 보존 정책 등록에 씀(2026-07-26 3순위 설계).
std::unique_ptr<IMetricStore> CreateMetricStore(const String& connectionInfo, int retentionDays);