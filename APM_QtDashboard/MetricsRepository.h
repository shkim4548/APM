#pragma once
#include <QMetaType>
#include <QString>
#include <QVector>
#include <qglobal.h>

// Metrics 테이블 한 행 - APM 콘솔의 한 열과 1:1 대응
struct MetricsSample
{
    qlonglong id = 0;
    QString ts;
    double cpuUsagePercent = 0.0;
    qlonglong memUsedBytes = 0;
    qlonglong memTotalBytes = 0;
    qlonglong diskUsedBytes = 0;
    qlonglong diskTotalBytes = 0;
    qlonglong netRxBytesPerSec = 0;
    qlonglong netTxBytesPerSec = 0;
};

// 이 구조체는 사실상 WebConsole에 보이는 열을 구조체로 재 모델링
struct AlertSample
{
    qlonglong id = 0;
    int metricType = 0;
    double thresholdValue = 0.0;
    double triggerValue = 0.0;
    QString openedAt;
};

// 워커 쓰레드 -> UI쓰레드로 QVector를 큐 연결로 넘기기 위해선 메타타입 등록이 필요하다
Q_DECLARE_METATYPE(MetricsSample)
Q_DECLARE_METATYPE(AlertSample)

class MetricsRepository
{
public:
    // 생성자에 explicit을 사용하는 이유가 뭐지?
    explicit MetricsRepository(const QString& dbPath);
    ~MetricsRepository();

public:
    bool Open();
    bool IsOpen() const;

    QVector<MetricsSample> FetchLatestMetrics(int limit) const;
    QVector<AlertSample> FetchOpenAlerts(int limit) const;

private:
    QString _dbPath;
    QString _connectionName;
};