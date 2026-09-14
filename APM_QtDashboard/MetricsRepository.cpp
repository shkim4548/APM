#include "MetricsRepository.h"

#include <QDateTime>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>

namespace
{
    constexpr const char* kConnectionName = "apm_collector_ro";
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
    // 2026-09-14 : Collector의 apm_metrics.db는 스네이크케이스 컬럼 + epoch 정수 ts라
    // APM_Console 스키마(PascalCase, DateTimeOffset 문자열)와 다르다. ts 자체는 이제
    // 진짜 정수라 ORDER BY ts DESC도 안전하지만, 이 저장소 전반의 관례(rowid/Id 같은
    // 자동증가 정수로 삽입 순서 정렬 - Console 쪽 Ts 정렬 버그를 피하려던 것과 같은 이유)를
    // 그대로 따라 rowid로 정렬한다. metrics 테이블엔 명시적 PK가 없어 SQLite의 암묵적
    // rowid를 그대로 쓴다.
    query.prepare(
        "SELECT rowid, ts, cpu_usage_percent, mem_used_bytes, mem_total_bytes, "
        "disk_used_bytes, disk_total_bytes, net_rx_bytes_per_sec, net_tx_bytes_per_sec "
        "FROM metrics ORDER BY rowid DESC LIMIT ?");
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
        // ts는 epoch(초) 정수로 저장돼 있다 - 표시용 문자열로 여기서 변환해둔다
        // (MetricsTableModel 등 하위 소비자는 여전히 QString ts를 그대로 받아 쓰면 됨).
        qint64 epochSeconds = query.value(1).toLongLong();
        sample.ts = QDateTime::fromSecsSinceEpoch(epochSeconds).toString("yyyy-MM-dd HH:mm:ss");
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
