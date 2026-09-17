#pragma once
#include <QMainWindow>
#include <QVector>
#include <QThread>

#include "AgentAlertRepository.h"
#include "MetricsRepository.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTableView;
class QTimer;
class MetricsWorker;
class MetricsTableModel;
class AlertsTableModel;
class MetricsChartWidget;
class MetricsPushClient;
class AgentControlClient;

// step 2~7' (2026-09-17 재작업) : Collector 로컬 DB(지표) + Agent 로컬 DB(알림/상태) 조회
// + Agent 로컬 소켓으로 제어 명령(시작/중지/재연결/로그레벨) 전송까지 전부 갖춘다.
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
    void OnStartAgentClicked();
    void OnStopAgentClicked();
    void OnReconnectClicked();
    void OnSetLogLevelClicked();
    void OnControlCommandResult(bool ok, const QString& error);

private:
    void SetStatus(const QString& text);

private:
    QThread _workerThread;
    MetricsWorker* _worker = nullptr;
    QTimer* _refreshTimer = nullptr;           // 안전망(주 트리거는 _pushClient)
    MetricsPushClient* _pushClient = nullptr;  // Collector 푸시 구독, 주 트리거
    AgentControlClient* _controlClient = nullptr;  // step 7' : Agent 제어 명령 전용

    MetricsTableModel* _metricsModel = nullptr;
    AlertsTableModel* _alertsModel = nullptr;
    QTableView* _metricsView = nullptr;
    QTableView* _alertsView = nullptr;
    MetricsChartWidget* _chartWidget = nullptr;
    QPushButton* _refreshButton = nullptr;
    QLabel* _statusLabel = nullptr;
    QLabel* _agentStatusLabel = nullptr;

    // step 7' : Agent 제어판.
    QPushButton* _startAgentButton = nullptr;
    QPushButton* _stopAgentButton = nullptr;
    QPushButton* _reconnectButton = nullptr;
    QComboBox* _logLevelCombo = nullptr;
    QPushButton* _setLogLevelButton = nullptr;
    QLabel* _controlStatusLabel = nullptr;
};
