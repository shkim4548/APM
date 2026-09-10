#pragma once
#include <QMainWindow>
#include <QVector>
#include <QThread>

#include "MetricsRepository.h"

class QLabel;
class QPushButton;
class QTableWidget;
class MetricsWorker;

// 검증용 최소 창 : QPushButton::clicked 신호를 이 클래스의 슬롯에 연결
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

    //MetricsRepository _repository = nullptr;
    QTableWidget* _metricsTable = nullptr;
    QTableWidget* _alertsTable = nullptr;
    QPushButton* _refreshButton = nullptr;
    QLabel* _statusLabel = nullptr;
};