#include "MainWindow.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
// APM_Console의 appsettings.json Apm:ConnectionString과 같은 파일을 가리켜야 한다.
// 이 체크아웃 기준 절대경로를 하드코딩 - Console 쪽도 지금 절대경로 하드코딩
// 상태라 이식성 문제가 새로 생기는 건 아니다. 배포판을 만들 때(§6 8단계)
// 커맨드라인 인자나 설정 파일로 뺄 것.
const QString kConsoleDbPath = "/home/shkim/dev/APM/APM_Console/webserver_apm.db";

// AlertRecords.MetricType 값 순서는 APM_Console의 AlertMetricType enum과 반드시
// 일치해야 한다(AlertThreshold.cs) - 두 프로젝트 사이의 암묵적 데이터 계약.
QString MetricTypeToString(int metricType)
{
    switch (metricType)
    {
    case 0: return "CPU";
    case 1: return "Memory";
    case 2: return "Disk";
    case 3: return "TCP RTT";
    default: return QString("Unknown(%1)").arg(metricType);
    }
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , _repository(kConsoleDbPath)
{
    setWindowTitle("APM Qt Dashboard - step 2 (SQLite initial load)");

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    _refreshButton = new QPushButton("새로고침", central);

    _metricsTable = new QTableWidget(central);
    _metricsTable->setColumnCount(6);
    _metricsTable->setHorizontalHeaderLabels(
        {"Id", "Ts", "CPU %", "Mem %", "Disk %", "Net Rx/Tx (B/s)"});
    _metricsTable->horizontalHeader()->setStretchLastSection(true);
    _metricsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

    _alertsTable = new QTableWidget(central);
    _alertsTable->setColumnCount(5);
    _alertsTable->setHorizontalHeaderLabels(
        {"Id", "Metric", "Threshold", "Trigger", "Opened At"});
    _alertsTable->horizontalHeader()->setStretchLastSection(true);
    _alertsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

    layout->addWidget(_refreshButton);
    layout->addWidget(_metricsTable);
    layout->addWidget(_alertsTable);
    setCentralWidget(central);

    connect(_refreshButton, &QPushButton::clicked, this, &MainWindow::OnRefreshClicked);

    if (!_repository.Open())
    {
        setWindowTitle(windowTitle() + " - DB open failed (see stderr)");
        return;
    }
    LoadData();
}

void MainWindow::OnRefreshClicked()
{
    LoadData();
}

void MainWindow::LoadData()
{
    PopulateMetricsTable(_repository.FetchLatestMetrics(20));
    PopulateAlertTable(_repository.FetchOpenAlerts(20));
}

void MainWindow::PopulateMetricsTable(const QVector<MetricsSample>& samples)
{
    _metricsTable->setRowCount(samples.size());
    for (int row = 0; row < samples.size(); ++row)
    {
        const MetricsSample& s = samples[row];
        const double memPercent = s.memTotalBytes > 0
            ? s.memUsedBytes * 100.0 / s.memTotalBytes
            : 0.0;
        const double diskPercent = s.diskTotalBytes > 0
            ? s.diskUsedBytes * 100.0 / s.diskTotalBytes
            : 0.0;

        _metricsTable->setItem(row, 0, new QTableWidgetItem(QString::number(s.id)));
        _metricsTable->setItem(row, 1, new QTableWidgetItem(s.ts));
        _metricsTable->setItem(row, 2, new QTableWidgetItem(QString::number(s.cpuUsagePercent, 'f', 1)));
        _metricsTable->setItem(row, 3, new QTableWidgetItem(QString::number(memPercent, 'f', 1)));
        _metricsTable->setItem(row, 4, new QTableWidgetItem(QString::number(diskPercent, 'f', 1)));
        _metricsTable->setItem(row, 5, new QTableWidgetItem(
            QString("%1 / %2").arg(s.netRxBytesPerSec).arg(s.netTxBytesPerSec)));
    }
}

void MainWindow::PopulateAlertTable(const QVector<AlertSample>& alerts)
{
    _alertsTable->setRowCount(alerts.size());
    for (int row = 0; row < alerts.size(); ++row)
    {
        const AlertSample& a = alerts[row];
        _alertsTable->setItem(row, 0, new QTableWidgetItem(QString::number(a.id)));
        _alertsTable->setItem(row, 1, new QTableWidgetItem(MetricTypeToString(a.metricType)));
        _alertsTable->setItem(row, 2, new QTableWidgetItem(QString::number(a.thresholdValue, 'f', 1)));
        _alertsTable->setItem(row, 3, new QTableWidgetItem(QString::number(a.triggerValue, 'f', 1)));
        _alertsTable->setItem(row, 4, new QTableWidgetItem(a.openedAt));
    }
}
