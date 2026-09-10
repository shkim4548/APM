#include "MetricsWorker.h"

namespace
{
    constexpr int kFetchLimit = 20;
}

MetricsWorker::MetricsWorker(const QString& dbPath, QObject* parent)
    : QObject(parent), _repository(dbPath)
{
}

void MetricsWorker::Initialize()
{
    // QSqlDatabase 연결은 만든 쓰레드에서만 쓸 수 있으므로 Open()을 이 슬롯에서 호출한다
    _ready = _repository.Open();
    emit Initialized(_ready);
}

void MetricsWorker::Refresh()
{
    if(!_ready)
    {
        emit RefreshFailed("Repository not open");
        return;
    }

    emit MetricsReady(_repository.FetchLatestMetrics(kFetchLimit));
    emit AlertsReady(_repository.FetchOpenAlerts(kFetchLimit));
}