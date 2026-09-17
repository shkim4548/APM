#pragma once
#include "pch.h"
#include <sqlite3.h>
#include <optional>
#include "../Agent/ThresholdSet.h"

// step Phase A : Agent 전용 알림 이력 저장소. apm_metrics.db(Collector 소유, 지표)와는
// 완전히 별개의 파일이다 - Agent는 Collector의 DB에 절대 손대지 않는다(프로세스 경계 존중,
// 2026-09-14 사용자 지적: "Collector가 알림판단까지 하는건 Agent의 역할을 침해하는 것").
struct LocalAlertRecord
{
    long long id = 0;
    int metricType = 0;
    double thresholdValue = 0.0;
    double triggerValue = 0.0;
    long long openedAt = 0;     // epoch seconds
};

class AgentAlertStore
{
public:
    explicit AgentAlertStore(const String& dbPath);
    ~AgentAlertStore();

    // 열림 상태의 알림이 있으면 그 레코드를, 없으면 nullopt를 반환.
    std::optional<LocalAlertRecord> FindOpen(AlertMetricType type) const;
    // 새 알림을 연다.
    void Open(AlertMetricType type, double thresholdValue, double triggerValue);
    // id로 지정된 알림을 닫는다(resolved_value 기록).
    void Resolve(long long id, double resolvedValue);

    // step 6' : Agent<->Collector 연결 상태를 1행짜리 테이블에 기록한다(Qt가 이 파일을
    // 읽어서 "연결됨/끊김" 표시를 하기 위함 - Collector가 죽어도 Agent는 로컬로 계속 돌므로
    // 이 상태는 Agent가 직접 남겨야 함, Collector에 물어볼 수 없음).
    void UpdateStatus(bool connected);

private:
    sqlite3* _db = nullptr;
    sqlite3_stmt* _findOpenStmt = nullptr;
    sqlite3_stmt* _openStmt = nullptr;
    sqlite3_stmt* _resolveStmt = nullptr;
    sqlite3_stmt* _updateStatusStmt = nullptr;
};
