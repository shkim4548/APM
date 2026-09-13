#pragma once
#include <QMainWindow>
#include <QVector>
#include <QThread>

#include "MetricsRepository.h"

class QLabel;
class QPushButton;
class QTableView;
class MetricsWorker;
class MetricsTableModel;
class AlertsTableModel;

// step 4 : 데이터(모델)와 표시(뷰)를 분리한다. QTableWidget → QTableView + Model.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    // 워커 스레드의 Refresh() 슬롯에 큐 연결된다. emit 후 즉시 반환한다.
    void RefreshRequested();

private slots:
    void OnRefreshClicked();
    void OnWorkerInitialized(bool ok);
    void OnRefreshFailed(const QString& reason);
    void PopulateMetricsTable(const QVector<MetricsSample>& samples);
    void PopulateAlertTable(const QVector<AlertSample>& alerts);

private:
    void SetStatus(const QString& text);

private:
    QThread _workerThread;
    MetricsWorker* _worker = nullptr;

    MetricsTableModel* _metricsModel = nullptr;
    AlertsTableModel* _alertsModel = nullptr;
    QTableView* _metricsView = nullptr;
    QTableView* _alertsView = nullptr;
    QPushButton* _refreshButton = nullptr;
    QLabel* _statusLabel = nullptr;
};