#pragma once
#include <QMetaType>
#include <QString>
#include <QVector>
#include <qglobal.h>

#include "MetricsRepository.h"   // AlertSample 재사용

// step 6' : Agent의 agent_alerts.db(local_alerts + agent_status)를 읽는다.
// MetricsRepository(Collector의 apm_metrics.db)와는 완전히 다른 파일/커넥션 -
// 두 리포지토리를 하나로 합치지 않는다(프로세스 경계가 다르므로 파일도 다름).
struct AgentStatus
{
    bool connected = false;
    qlonglong updatedAt = 0;   // epoch seconds
    bool valid = false;        // agent_status에 행이 아직 없으면 false(Agent가 한 번도 안 씀)
};

Q_DECLARE_METATYPE(AgentStatus)

class AgentAlertRepository
{
public:
    explicit AgentAlertRepository(const QString& dbPath);
    ~AgentAlertRepository();

    bool Open();
    bool IsOpen() const;

    QVector<AlertSample> FetchOpenAlerts(int limit) const;
    AgentStatus FetchStatus() const;

private:
    QString _dbPath;
    QString _connectionName;
};
