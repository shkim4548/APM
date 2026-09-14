#pragma once
#include "pch.h"
#include "ThresholdSet.h"
#include "../Storage/AgentAlertStore.h"
#include "../Protocol/Metric.pb.h"

// step Phase A : Console의 AlertEvaluator.Evaluate()(C#, AlertEvaluator.cs)를 C++로 그대로 이식.
// Agent가 Collector로 보내기 전, 수집한 그 자리에서 스스로 판단한다 - Agent<->Collector
// 연결이 끊겨도 로컬 알림은 계속 동작해야 하기 때문(2026-09-14 설계 결정).
enum class AlertTransition { None, Opened, Resolved };

// 순수 함수 - DB/상태 없음, 유닛테스트 대상(GoogleTest, tests/ 관례 그대로).
// currentValue >= threshold가 breach, 상태 전이(Opened/Resolved)일 때만 알림
// (Zabbix/Nagios/Alertmanager 관례 - Console의 AlertEvaluator.Evaluate()와 동일 판정).
AlertTransition EvaluateTransition(double currentValue, double threshold, bool currentlyOpen);

class LocalAlertEvaluator
{
public:
    LocalAlertEvaluator(const ThresholdSet& thresholds, AgentAlertStore& store);

    // MetricScheduler 콜백에서, Collector 전송 전에 호출한다.
    void OnNewMetric(const apm::Metric& metric);

private:
    void EvaluateOne(AlertMetricType type, double currentValue);

private:
    const ThresholdSet& _thresholds;
    AgentAlertStore& _store;
};
