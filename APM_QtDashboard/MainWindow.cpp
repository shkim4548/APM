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

#include "MetricsChartWidget.h"
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

// Agent의 수집 주기(APM_Agent/Agent/main.cpp의 MetricScheduler)와 맞춘다 -
// 더 뜸하면 새 데이터 반영이 늦고, 더 잦으면 같은 데이터를 헛되이 반복 조회한다.
constexpr int kRefreshIntervalMs = 5000;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("APM Qt Dashboard - step 2~5 (Collector 로컬 DB + QtCharts)");

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    _refreshButton = new QPushButton("새로고침", central);
    _statusLabel = new QLabel("초기화 중...", central);

    _chartWidget = new MetricsChartWidget(central);

    _metricsModel = new MetricsTableModel(this);
    _metricsView = new QTableView(central);
    _metricsView->setModel(_metricsModel);
    _metricsView->horizontalHeader()->setStretchLastSection(true);
    _metricsView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    layout->addWidget(_refreshButton);
    layout->addWidget(_statusLabel);
    layout->addWidget(_chartWidget);
    layout->addWidget(_metricsView);
    setCentralWidget(central);

    connect(_refreshButton, &QPushButton::clicked, this, &MainWindow::OnRefreshClicked);

    // 워커를 만들고 워커 스레드로 옮긴다. 이 시점 이후 워커의 슬롯은 워커 스레드에서 실행된다.
    _worker = new MetricsWorker(kCollectorDbPath);
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
    connect(_worker, &MetricsWorker::RefreshFailed, this, &MainWindow::OnRefreshFailed);

    _refreshTimer = new QTimer(this);
    _refreshTimer->setInterval(kRefreshIntervalMs);
    connect(_refreshTimer, &QTimer::timeout, this, &MainWindow::OnRefreshClicked);

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

void MainWindow::OnWorkerInitialized(bool ok)
{
    if (!ok)
    {
        SetStatus("DB 열기 실패 - stderr 확인");
        return;
    }
    SetStatus("연결됨");
    emit RefreshRequested();
    _refreshTimer->start();
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
