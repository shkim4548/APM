#pragma once
#include <QWidget>
#include <QVector>

#include "MetricsRepository.h"

class QChartView;
class QLineSeries;
class QValueAxis;

// step 5 : CPU/메모리 사용률 실시간 시계열 차트.
// QChart 자체는 QGraphicsWidget이라 화면에 못 올린다 - QChartView(QWidget 파생)에 얹는다.
class MetricsChartWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MetricsChartWidget(QWidget* parent = nullptr);

public slots:
    // MetricsWorker::MetricsReady에 그대로 연결한다. samples[0]이 최신값
    // (FetchLatestMetrics가 "ORDER BY rowid DESC"이므로)이라 그 한 점만 누적하고,
    // kMaxPoints를 넘으면 가장 오래된 점을 버린다.
    void AppendLatest(const QVector<MetricsSample>& samples);

private:
    QChartView* _chartView;
    QLineSeries* _cpuSeries;
    QLineSeries* _memSeries;
    QValueAxis* _axisX;
    QValueAxis* _axisY;
    qint64 _nextX = 0;
};
