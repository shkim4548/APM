#pragma once
#include <QMetaType>
#include <QString>
#include <QVector>
#include <qglobal.h>

// Metrics 테이블 한 행 - Collector의 apm_metrics.db(metrics 테이블)과 1:1 대응.
// 2026-09-14 : 대상 DB를 APM_Console(중앙)에서 Collector(그 장비 로컬)로 교체 -
// 필드 구성 자체는 그대로 재사용 가능해서 이 struct는 안 바뀜(쿼리 쪽만 바뀜, .cpp 참고).
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

// 이 구조체는 원래 APM_Console의 AlertRecords 열을 재모델링한 것이었다.
// 2026-09-14 : Collector의 apm_metrics.db엔 알림 테이블이 없어서(알림 판단은 이제 Agent가
// 로컬로 함, agent_alerts.db) 지금은 이 struct를 채워주는 조회 함수가 없다(§6 단계 검토 중 -
// Agent의 agent_alerts.db를 보는 별도 리포지토리로 다시 연결할 예정). 구조 자체는
// local_alerts 스키마와 모양이 비슷해 그대로 재사용될 가능성이 높아 지우지 않고 남겨둠.
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

private:
    QString _dbPath;
    QString _connectionName;
};
