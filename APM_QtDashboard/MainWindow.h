#pragma once
#include <QMainWindow>
#include <QVector>

#include "MetricsRepository.h"

class QPushButton;
class QTableWidget;

// 검증용 최소 창 : QPushButton::clicked 신호를 이 클래스의 슬롯에 연결
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void OnRefreshClicked();

private:
    void LoadData();
    void PopulateMetricsTable(const QVector<MetricsSample>& samples);
    void PopulateAlertTable(const QVector<AlertSample>& alerts);

private:
    MetricsRepository _repository;
    QTableWidget* _metricsTable;
    QTableWidget* _alertsTable;
    QPushButton* _refreshButton;
};