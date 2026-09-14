#include "MetricsChartWidget.h"

#include <algorithm>

#include <QChart>
#include <QChartView>
#include <QLineSeries>
#include <QPainter>
#include <QValueAxis>
#include <QVBoxLayout>

namespace
{
// 화면에 유지할 최대 점 개수. Agent가 5초마다 수집하므로(APM_Agent/Agent/main.cpp의
// MetricScheduler), 30개면 최근 2분 30초 구간을 보여준다 - 백엔드 retention 정책과
// 같은 사고방식: "화면에 얼마나 오래 된 데이터를 보여줄 가치가 있는가"를 명시적으로 정한 값.
constexpr int kMaxPoints = 30;
}

MetricsChartWidget::MetricsChartWidget(QWidget* parent)
    : QWidget(parent)
{
    _cpuSeries = new QLineSeries(this);
    _cpuSeries->setName("CPU %");

    _memSeries = new QLineSeries(this);
    _memSeries->setName("Mem %");

    auto* chart = new QChart();
    chart->addSeries(_cpuSeries);
    chart->addSeries(_memSeries);
    chart->setTitle("실시간 사용률");

    _axisX = new QValueAxis(this);
    _axisX->setLabelFormat("%d");
    _axisX->setTitleText("샘플 순번");
    _axisX->setRange(0, kMaxPoints);

    _axisY = new QValueAxis(this);
    _axisY->setRange(0, 100);
    _axisY->setTitleText("%");

    chart->addAxis(_axisX, Qt::AlignBottom);
    chart->addAxis(_axisY, Qt::AlignLeft);
    _cpuSeries->attachAxis(_axisX);
    _cpuSeries->attachAxis(_axisY);
    _memSeries->attachAxis(_axisX);
    _memSeries->attachAxis(_axisY);

    _chartView = new QChartView(chart, this);
    _chartView->setRenderHint(QPainter::Antialiasing);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(_chartView);
    setLayout(layout);
}

void MetricsChartWidget::AppendLatest(const QVector<MetricsSample>& samples)
{
    if (samples.isEmpty())
        return;

    // samples[0]이 최신값(FetchLatestMetrics가 "ORDER BY rowid DESC"로 가져오므로).
    const MetricsSample& latest = samples.first();
    const double memPercent = latest.memTotalBytes > 0
        ? latest.memUsedBytes * 100.0 / latest.memTotalBytes
        : 0.0;

    _cpuSeries->append(_nextX, latest.cpuUsagePercent);
    _memSeries->append(_nextX, memPercent);
    ++_nextX;

    if (_cpuSeries->count() > kMaxPoints)
    {
        _cpuSeries->removePoints(0, _cpuSeries->count() - kMaxPoints);
        _memSeries->removePoints(0, _memSeries->count() - kMaxPoints);
    }

    // 스크롤하는 것처럼 보이도록 X축 범위를 항상 "최근 kMaxPoints개" 구간으로 맞춘다.
    const qint64 rangeStart = std::max<qint64>(0, _nextX - kMaxPoints);
    _axisX->setRange(rangeStart, rangeStart + kMaxPoints);
}
