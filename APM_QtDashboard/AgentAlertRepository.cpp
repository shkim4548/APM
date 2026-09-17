#include "AgentAlertRepository.h"

#include <QDateTime>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>

namespace
{
    constexpr const char* kConnectionName = "apm_agent_alerts_ro";
}

AgentAlertRepository::AgentAlertRepository(const QString& dbPath)
    : _dbPath(dbPath), _connectionName(kConnectionName)
{
}

AgentAlertRepository::~AgentAlertRepository()
{
    if (QSqlDatabase::contains(_connectionName))
    {
        QSqlDatabase::removeDatabase(_connectionName);
    }
}

bool AgentAlertRepository::Open()
{
    auto db = QSqlDatabase::addDatabase("QSQLITE", _connectionName);
    db.setDatabaseName(_dbPath);
    db.setConnectOptions("QSQLITE_OPEN_READONLY");

    if (!db.open())
    {
        qWarning() << "AgentAlertRepository : failed to Open" << _dbPath << db.lastError().text();
        return false;
    }

    return true;
}

bool AgentAlertRepository::IsOpen() const
{
    return QSqlDatabase::database(_connectionName, false).isOpen();
}

QVector<AlertSample> AgentAlertRepository::FetchOpenAlerts(int limit) const
{
    QVector<AlertSample> result;

    QSqlQuery query(QSqlDatabase::database(_connectionName));
    query.prepare(
        "SELECT id, metric_type, threshold_value, trigger_value, opened_at "
        "FROM local_alerts WHERE closed_at IS NULL ORDER BY id DESC LIMIT ?");
    query.addBindValue(limit);

    if (!query.exec())
    {
        qWarning() << "AgentAlertRepository::FetchOpenAlerts failed : " << query.lastError().text();
        return result;
    }

    while (query.next())
    {
        AlertSample sample;
        sample.id = query.value(0).toLongLong();
        sample.metricType = query.value(1).toInt();
        sample.thresholdValue = query.value(2).toDouble();
        sample.triggerValue = query.value(3).toDouble();
        qint64 epochSeconds = query.value(4).toLongLong();
        sample.openedAt = QDateTime::fromSecsSinceEpoch(epochSeconds).toString("yyyy-MM-dd HH:mm:ss");
        result.push_back(sample);
    }
    return result;
}

AgentStatus AgentAlertRepository::FetchStatus() const
{
    AgentStatus status;

    QSqlQuery query(QSqlDatabase::database(_connectionName));
    query.prepare("SELECT connected, updated_at FROM agent_status WHERE id = 1");

    if (!query.exec())
    {
        qWarning() << "AgentAlertRepository::FetchStatus failed : " << query.lastError().text();
        return status;
    }

    if (query.next())
    {
        status.connected = query.value(0).toInt() != 0;
        status.updatedAt = query.value(1).toLongLong();
        status.valid = true;
    }
    // 행이 없으면(Agent가 아직 한 번도 UpdateStatus를 안 부름) valid=false인 채로 반환.

    return status;
}
