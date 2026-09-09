#include "MetricsRepository.h"

#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>

namespace
{
    constexpr const char* kConnectionName = "apm_console_ro";
}

MetricsRepository::MetricsRepository(const QString& dbPath)
    : _dbPath(dbPath), _connectionName(kConnectionName)
{
}

MetricsRepository::~MetricsRepository()
{
    // QSqlDatabase는 이름으로 전역 레지스트리에 등록된다, 이 객체는 소멸자에서 명시적으로 소멸시켜야한다
    if(QSqlDatabase::contains(_connectionName))
    {
        QSqlDatabase::removeDatabase(_connectionName);
    }
}

bool MetricsRepository::Open()
{
    auto db = QSqlDatabase::addDatabase("QSQLITE", _connectionName);
    db.setDatabaseName(_dbPath);

    // 기존 코어 무수정 원칙 구현 : 여기서 Insert/Update 쿼리를 막아버린다
    // 필요없는 체계의 훼손을 막는다
    db.setConnectOptions("QSQLITE_OPEN_READONLY");

    if(!db.open())
    {
        qWarning() << "MetricsRepository : failed to Open" << _dbPath << db.lastError().text();
        return false;
    }

    return true;
}

bool MetricsRepository::IsOpen() const
{
    return QSqlDatabase::database(_connectionName, false).isOpen();
}

QVector<MetricsSample> MetricsRepository::FetchLatestMetrics(int limit) const
{
    QVector<MetricsSample> result;

    QSqlQuery query(QSqlDatabase::database(_connectionName));
    // Ts는 .NET DataTimeOffset의 TEXT 직렬화라 소숫점 자리가 행마다 다르다.
    // 따라서 문자열 정렬 기준으로 쓰면 같은 초 안에서 순서가 어긋날 수 있다.

    query.prepare(
        "SELECT Id, Ts, CpuUsagePercent, MemUsedBytes, MemTotalBytes, "
        "DiskUsedBytes, DiskTotalBytes, NetRxBytesPerSec, NetTxBytesPerSec "
        "FROM Metrics ORDER BY Id DESC LIMIT ?");
    query.addBindValue(limit);

    if(!query.exec())
    {
        qWarning() << "MetricsRepository::FetchLatestMetrics failed : " << query.lastError().text();
        return result;
    }

    // 리눅스에서 터미널 명령어를 이용해 결과 메시지를 받아올 때랑 같은 이유로 while을 사용
    // query.next()에서 가져온 결과 메시지의 다음줄이 있는지를 확인 있으면 반복하는 것
    while (query.next())
    {
        MetricsSample sample;
        sample.id = query.value(0).toLongLong();
        sample.ts = query.value(1).toString();
        sample.cpuUsagePercent = query.value(2).toDouble();
        sample.memUsedBytes = query.value(3).toLongLong();
        sample.memTotalBytes = query.value(4).toLongLong();
        sample.diskUsedBytes = query.value(5).toLongLong();
        sample.diskTotalBytes = query.value(6).toLongLong();
        sample.netRxBytesPerSec = query.value(7).toLongLong();
        sample.netTxBytesPerSec = query.value(8).toLongLong();
        result.push_back(sample);
    }
    return result;
}

QVector<AlertSample> MetricsRepository::FetchOpenAlerts(int limit) const
{
    QVector<AlertSample> result;

    QSqlQuery query(QSqlDatabase::database(_connectionName));
    query.prepare(
        "SELECT Id, MetricType, ThresholdValue, TriggerValue, OpenedAt "
        "FROM AlertRecords WHERE ClosedAt IS NULL ORDER BY Id DESC LIMIT ?");
    query.addBindValue(limit);

    if(!query.exec())
    {
        qWarning() << "MetricsRepository::FetchOpenAlerts failed : " << query.lastError().text();
        return result;
    }

    while(query.next())
    {
        AlertSample sample;
        sample.id = query.value(0).toLongLong();
        sample.metricType = query.value(1).toInt();
        sample.thresholdValue = query.value(2).toDouble();
        sample.triggerValue = query.value(3).toDouble();
        sample.openedAt = query.value(4).toString();
        result.push_back(sample);
    }
    return result;
}