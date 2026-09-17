#pragma once
#include <QMainWindow>
#include <QVector>
#include <QThread>

#include "AgentAlertRepository.h"
#include "MetricsRepository.h"

class QLabel;
class QPushButton;
class QTableView;
class QTimer;
class MetricsWorker;
class MetricsTableModel;
class AlertsTableModel;
class MetricsChartWidget;
class MetricsPushClient;

// step 2~6' (2026-09-17 재작업) : Collector 로컬 DB(지표) + Agent 로컬 DB(알림/상태)
// 둘 다 읽는다. 알림/상태도 기존 지표 갱신 트리거(푸시+안전망)를 그대로 재사용 -
// 별도 푸시 채널 없음(WORK_STATUS.md/SESSION_LOG.md 2026-09-17 참고). Agent 제어(§7)는
// 아직 손대지 않음.
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
    void OnWorkerInitialized(bool metricsOk);
    void OnRefreshFailed(const QString& reason);
    void PopulateMetricsTable(const QVector<MetricsSample>& samples);
    void PopulateAlertTable(const QVector<AlertSample>& alerts);
    void UpdateAgentStatus(const AgentStatus& status);

private:
    void SetStatus(const QString& text);

private:
    QThread _workerThread;
    MetricsWorker* _worker = nullptr;
    QTimer* _refreshTimer = nullptr;           // 안전망(주 트리거는 _pushClient)
    MetricsPushClient* _pushClient = nullptr;  // Collector 푸시 구독, 주 트리거

    MetricsTableModel* _metricsModel = nullptr;
    AlertsTableModel* _alertsModel = nullptr;
    QTableView* _metricsView = nullptr;
    QTableView* _alertsView = nullptr;
    MetricsChartWidget* _chartWidget = nullptr;
    QPushButton* _refreshButton = nullptr;
    QLabel* _statusLabel = nullptr;
    QLabel* _agentStatusLabel = nullptr;
};