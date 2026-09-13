#include "AlertsTableModel.h"

namespace
{
constexpr int kColumnCount = 5;

// MainWindow.cpp에 있던 것과 같은 함수 - 모델로 옮겨온다.
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

AlertsTableModel::AlertsTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int AlertsTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;

    return _alerts.size();
}

int AlertsTableModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;

    return kColumnCount;
}

QVariant AlertsTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || role != Qt::DisplayRole)
        return QVariant();

    const AlertSample& a = _alerts.at(index.row());
    switch (index.column())
    {
    case 0: return QString::number(a.id);
    case 1: return MetricTypeToString(a.metricType);
    case 2: return QString::number(a.thresholdValue, 'f', 1);
    case 3: return QString::number(a.triggerValue, 'f', 1);
    case 4: return a.openedAt;
    default: return QVariant();
    }
}

QVariant AlertsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return QVariant();

    static const QStringList kHeaders = {"Id", "Metric", "Threshold", "Trigger", "Opened At"};
    if (section < 0 || section >= kHeaders.size())
        return QVariant();
    return kHeaders.at(section);
}

void AlertsTableModel::SetAlerts(const QVector<AlertSample>& alerts)
{
    beginResetModel();
    _alerts = alerts;
    endResetModel();
}
