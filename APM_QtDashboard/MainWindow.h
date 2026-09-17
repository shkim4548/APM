#pragma once
#include <QMainWindow>
#include <QVector>
#include <QThread>

#include "MetricsRepository.h"

class QLabel;
class QPushButton;
class QTableView;
class QTimer;
class MetricsWorker;
class MetricsTableModel;
class MetricsChartWidget;
class MetricsPushClient;

// step 2~5 (2026-09-14 재작업) : 대상 DB를 APM_Console(중앙)에서 Collector(그 장비
// 로컬)로 교체 + QTimer 자동 갱신 + 실시간 차트(MetricsChartWidget) 추가.
// 알림/Agent 제어(§6~7)는 별도로 검토 중이라 이번엔 손대지 않음.
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

private:
    void SetStatus(const QString& text);

private:
    QThread _workerThread;
    MetricsWorker* _worker = nullptr;
    QTimer* _refreshTimer = nullptr;           // 이제 "안전망"(주 트리거는 _pushClient)
    MetricsPushClient* _pushClient = nullptr;  // 2026-09-14 : Collector 푸시 구독, 주 트리거

    MetricsTableModel* _metricsModel = nullptr;
    QTableView* _metricsView = nullptr;
    MetricsChartWidget* _chartWidget = nullptr;
    QPushButton* _refreshButton = nullptr;
    QLabel* _statusLabel = nullptr;
};