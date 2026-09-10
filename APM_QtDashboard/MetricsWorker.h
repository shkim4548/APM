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
    // 최신 지표/열린 알림 조회해서 결과를 시그널로 남긴다
    void Refresh();

signals:
    void Initialized(bool ok);
    void MetricsReady(const QVector<MetricsSample>& samples);
    void AlertsReady(const QVector<AlertSample>& alerts);
    void RefreshFailed(const QString& reason);

private:
    MetricsRepository _repository;
    bool _ready = false;  
};