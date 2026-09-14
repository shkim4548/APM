#pragma once
#include <QObject>
#include <QString>
#include <QVector>

#include "MetricsRepository.h"

// Sqlite3 조회를 UI 스레드에서 분리한다
class MetricsWorker : public QObject
{
    Q_OBJECT
public :
    explicit MetricsWorker(const QString& dbPath, QObject* parent = nullptr);

public slots:
    // move to thread 이후 워커쓰레드에서 실행한다.
    void Initialize();
    // 최신 지표를 조회해서 결과를 시그널로 남긴다.
    // 2026-09-14 : 알림 조회(AlertsReady)는 뺐다 - Collector의 apm_metrics.db엔 알림
    // 테이블이 없음(알림 판단은 이제 Agent가 로컬로 함, agent_alerts.db). §6 단계에서
    // 그 DB를 보는 별도 워커/리포지토리로 다시 연결할 예정.
    void Refresh();

signals:
    void Initialized(bool ok);
    void MetricsReady(const QVector<MetricsSample>& samples);
    void RefreshFailed(const QString& reason);

private:
    MetricsRepository _repository;
    bool _ready = false;  
};