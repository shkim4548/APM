#include "MetricsWorker.h"

#include <QDebug>

namespace
{
    constexpr int kFetchLimit = 20;
}

MetricsWorker::MetricsWorker(const QString& metricsDbPath, const QString& alertsDbPath, QObject* parent)
    : QObject(parent), _repository(metricsDbPath), _alertRepository(alertsDbPath)
{
}

void MetricsWorker::Initialize()
{
    // QSqlDatabase 연결은 만든 쓰레드에서만 쓸 수 있으므로 Open()을 이 슬롯에서 호출한다
    _ready = _repository.Open();

    // 알림 DB는 지표 DB와 별개 파일이라 실패해도 지표 표시 자체는 계속 동작해야 한다
    // (Agent가 아직 안 떠 있어도 Collector 지표는 볼 수 있어야 함) - 그래서 실패해도
    // Initialized(false)로 전체를 막지 않고 경고만 남긴다.
    if (!_alertRepository.Open())
        qWarning() << "MetricsWorker: alert repository open failed (지표는 계속 조회됨)";

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

    // 2026-09-17(검증 중 발견) : _alertRepository.IsOpen()으로 감싸면, Agent가 한 번도
    // 안 떠서 agent_alerts.db 자체가 없는 경우 Open()이 영원히 실패해 이 시그널이 한 번도
    // 안 나가고 UpdateAgentStatus()도 안 불려서 라벨이 초기 문구("확인 중...")에 멈춰버림
    // ("알 수 없음"으로 안 바뀜 - 설계 의도와 다름). FetchOpenAlerts/FetchStatus는 연결이
    // 안 열려 있어도 QSqlQuery::exec() 실패를 내부에서 잡아 빈 결과/valid=false를 반환하므로
    // 가드 없이 항상 조회를 시도하는 쪽이 의도한 동작과 맞다.
    emit AlertsReady(_alertRepository.FetchOpenAlerts(kFetchLimit));
    emit StatusReady(_alertRepository.FetchStatus());
}