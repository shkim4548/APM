#include "MainWindow.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "AlertsTableModel.h"
#include "MetricsChartWidget.h"
#include "MetricsPushClient.h"
#include "MetricsTableModel.h"
#include "MetricsWorker.h"

namespace
{
// 2026-09-14 : APM_Console(중앙) 대신 Collector(그 장비 로컬)가 만드는 apm_metrics.db를
// 본다 - Qt는 이제 Console을 전혀 모른다(WORK_STATUS.md/QT_MFC_PORTFOLIO_PLAN.md §3-2
// 아키텍처 대전환 참고). Collector가 상대경로("apm_metrics.db")로 파일을 만들기 때문에
// 실제 위치는 Collector를 어느 디렉터리에서 실행했는지에 달려있다 - 이 체크아웃에서는
// APM_Agent/ 안에서 실행하는 관례(APM_Viewer의 기존 하드코딩과 동일)를 그대로 따름.
const QString kCollectorDbPath = "/home/shkim/dev/APM/APM_Agent/apm_metrics.db";

// step 6' : Agent가 만드는 agent_alerts.db(알림 + 연결 상태). Agent도 같은 디렉터리
// 관례로 실행한다고 가정.
const QString kAgentAlertsDbPath = "/home/shkim/dev/APM/APM_Agent/agent_alerts.db";

// 2026-09-14 : Collector의 MetricsBroadcastServer 소켓 경로(Collector/main.cpp의
// METRICS_PUBSUB_SOCKET_PATH와 반드시 같아야 함).
const QString kCollectorPushSocketPath = "/tmp/apm_collector.sock";

// 2026-09-14 : 이제 "주 트리거"가 아니라 "안전망" - 푸시 연결이 끊겨 있어도(또는 §6'
// 트레이드오프대로 Agent<->Collector가 끊겨 Collector가 아예 푸시를 못 하는 동안도) 이
// 주기마다는 갱신되게 한다. 너무 짧으면 안전망의 존재 의미가 없고(푸시랑 다를 바 없어짐),
// 너무 길면 푸시가 끊긴 동안 화면이 오래 정체된다 - 30초(수집 주기의 6배)로 절충.
constexpr int kFallbackRefreshIntervalMs = 30000;

// step 6' : agent_status.updated_at이 이보다 오래되면 "Agent 응답 없음"으로 본다.
// Agent 수집 주기(5초)의 3배 - 한두 번 갱신을 놓쳐도 바로 "죽음"으로 오판정하지 않기 위한 여유.
constexpr qint64 kAgentStaleSeconds = 15;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("APM Qt Dashboard - step 2~6' (Collector+Agent 로컬 DB, 발행-구독 갱신)");

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    _refreshButton = new QPushButton("새로고침", central);
    _statusLabel = new QLabel("초기화 중...", central);
    _agentStatusLabel = new QLabel("Agent 상태: 확인 중...", central);

    _chartWidget = new MetricsChartWidget(central);

    _metricsModel = new MetricsTableModel(this);
    _metricsView = new QTableView(central);
    _metricsView->setModel(_metricsModel);
    _metricsView->horizontalHeader()->setStretchLastSection(true);
    _metricsView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    _alertsModel = new AlertsTableModel(this);
    _alertsView = new QTableView(central);
    _alertsView->setModel(_alertsModel);
    _alertsView->horizontalHeader()->setStretchLastSection(true);
    _alertsView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    layout->addWidget(_refreshButton);
    layout->addWidget(_statusLabel);
    layout->addWidget(_agentStatusLabel);
    layout->addWidget(_chartWidget);
    layout->addWidget(_metricsView);
    layout->addWidget(_alertsView);
    setCentralWidget(central);

    connect(_refreshButton, &QPushButton::clicked, this, &MainWindow::OnRefreshClicked);

    // 워커를 만들고 워커 스레드로 옮긴다. 이 시점 이후 워커의 슬롯은 워커 스레드에서 실행된다.
    _worker = new MetricsWorker(kCollectorDbPath, kAgentAlertsDbPath);
    _worker->moveToThread(&_workerThread);

    // 스레드가 끝나면 워커를 그 스레드에서 안전하게 삭제한다.
    connect(&_workerThread, &QThread::finished, _worker, &QObject::deleteLater);

    // UI → 워커 (스레드가 다르므로 자동으로 Queued Connection)
    connect(this, &MainWindow::RefreshRequested, _worker, &MetricsWorker::Refresh);

    // 워커 → UI (역시 Queued. 슬롯 본문은 UI 스레드에서 실행됨)
    connect(_worker, &MetricsWorker::Initialized, this, &MainWindow::OnWorkerInitialized);
    connect(_worker, &MetricsWorker::MetricsReady, this, &MainWindow::PopulateMetricsTable);
    // 시그널 팬아웃 - 같은 MetricsReady를 차트도 받아서, 최신 값 1개만 누적한다.
    connect(_worker, &MetricsWorker::MetricsReady, _chartWidget, &MetricsChartWidget::AppendLatest);
    connect(_worker, &MetricsWorker::AlertsReady, this, &MainWindow::PopulateAlertTable);
    connect(_worker, &MetricsWorker::StatusReady, this, &MainWindow::UpdateAgentStatus);
    connect(_worker, &MetricsWorker::RefreshFailed, this, &MainWindow::OnRefreshFailed);

    _refreshTimer = new QTimer(this);
    _refreshTimer->setInterval(kFallbackRefreshIntervalMs);
    connect(_refreshTimer, &QTimer::timeout, this, &MainWindow::OnRefreshClicked);
    _refreshTimer->start();

    // 2026-09-14 : Collector 푸시 구독 - 새 지표 알림을 받으면 기존 트리거(OnRefreshClicked)를
    // 그대로 재사용한다(알림/상태도 이 트리거 한 번에 같이 조회됨 - MetricsWorker::Refresh() 참고).
    _pushClient = new MetricsPushClient(kCollectorPushSocketPath, this);
    connect(_pushClient, &MetricsPushClient::NewMetricAvailable, this, &MainWindow::OnRefreshClicked);
    _pushClient->Start();

    _workerThread.start();

    // 워커가 워커 스레드로 옮겨진 뒤 Initialize()가 그 스레드에서 실행되도록 큐에 넣는다.
    QMetaObject::invokeMethod(_worker, "Initialize", Qt::QueuedConnection);
}

MainWindow::~MainWindow()
{
    // 워커 스레드의 이벤트 루프를 멈추고, 실제로 끝날 때까지 기다린다.
    // 이걸 빼면 프로세스 종료 시 "QThread: Destroyed while thread is still running" 경고/크래시.
    _workerThread.quit();
    _workerThread.wait();
}

void MainWindow::OnRefreshClicked()
{
    SetStatus("불러오는 중...");
    emit RefreshRequested();
}

void MainWindow::OnWorkerInitialized(bool metricsOk)
{
    if (!metricsOk)
    {
        SetStatus("DB 열기 실패 - stderr 확인");
        return;
    }
    SetStatus("연결됨");
    emit RefreshRequested();
}

void MainWindow::OnRefreshFailed(const QString& reason)
{
    SetStatus("조회 실패: " + reason);
}

void MainWindow::SetStatus(const QString& text)
{
    _statusLabel->setText(text);
}

void MainWindow::PopulateMetricsTable(const QVector<MetricsSample>& samples)
{
    _metricsModel->SetSamples(samples);
    SetStatus(QString("갱신 완료 %1").arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
}

void MainWindow::PopulateAlertTable(const QVector<AlertSample>& alerts)
{
    _alertsModel->SetAlerts(alerts);
}

void MainWindow::UpdateAgentStatus(const AgentStatus& status)
{
    if (!status.valid)
    {
        _agentStatusLabel->setText("Agent 상태: 알 수 없음(agent_alerts.db에 상태 없음)");
        return;
    }

    qint64 ageSeconds = QDateTime::currentSecsSinceEpoch() - status.updatedAt;

    if (ageSeconds > kAgentStaleSeconds)
        _agentStatusLabel->setText(QString("Agent 상태: 응답 없음 (마지막 갱신 %1초 전)").arg(ageSeconds));
    else if (status.connected)
        _agentStatusLabel->setText("Agent 상태: Collector에 연결됨");
    else
        _agentStatusLabel->setText("Agent 상태: Collector 연결 끊김(재시도 중)");
}
