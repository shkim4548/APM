#pragma once
#include <QObject>
#include <QString>
#include <QVector>

#include "AgentAlertRepository.h"
#include "MetricsRepository.h"

// Sqlite3 조회를 UI 스레드에서 분리한다
class MetricsWorker : public QObject
{
    Q_OBJECT
public :
    explicit MetricsWorker(const QString& metricsDbPath, const QString& alertsDbPath, QObject* parent = nullptr);

public slots:
    // move to thread 이후 워커쓰레드에서 실행한다.
    void Initialize();
    // 최신 지표 + 열린 알림 + Agent 상태를 조회해서 결과를 시그널로 남긴다.
    // 2026-09-17(§6') : 알림/상태 전용 푸시 채널을 새로 안 만들고, 기존 지표 갱신
    // 트리거(Collector 푸시 우선 + 30초 안전망)에 그대로 얹는다 - Agent가 Collector에
    // 보내기 전에 이미 로컬로 알림을 판단해두므로 대부분 시점상 최신이고, 소켓을 더
    // 늘리지 않는 쪽을 택함(단, Agent<->Collector 연결이 끊긴 동안은 알림/상태도 지표와
    // 같이 30초 안전망 주기로만 갱신됨 - 감수한 트레이드오프).
    void Refresh();

signals:
    void Initialized(bool metricsOk);
    void MetricsReady(const QVector<MetricsSample>& samples);
    void AlertsReady(const QVector<AlertSample>& alerts);
    void StatusReady(const AgentStatus& status);
    void RefreshFailed(const QString& reason);

private:
    MetricsRepository _repository;
    AgentAlertRepository _alertRepository;
    bool _ready = false;
};