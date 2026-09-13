#include "MetricsTableModel.h"

namespace
{
constexpr int kColumnCount = 6;
}

MetricsTableModel::MetricsTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int MetricsTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;

    return _samples.size();
}

int MetricsTableModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;

    return kColumnCount;
}

QVariant MetricsTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || role != Qt::DisplayRole)
        return QVariant();

    const MetricsSample& s = _samples.at(index.row());
    const double memPercent = s.memTotalBytes > 0
        ? s.memUsedBytes * 100.0 / s.memTotalBytes
        : 0.0;
    const double diskPercent = s.diskTotalBytes > 0
        ? s.diskUsedBytes * 100.0 / s.diskTotalBytes
        : 0.0;

    switch (index.column())
    {
    case 0: return QString::number(s.id);
    case 1: return s.ts;
    case 2: return QString::number(s.cpuUsagePercent, 'f', 1);
    case 3: return QString::number(memPercent, 'f', 1);
    case 4: return QString::number(diskPercent, 'f', 1);
    case 5: return QString("%1 / %2").arg(s.netRxBytesPerSec).arg(s.netTxBytesPerSec);
    default: return QVariant();
    }
}

QVariant MetricsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return QVariant();

    static const QStringList kHeaders = {"Id", "Ts", "CPU %", "Mem %", "Disk %", "Net Rx/Tx (B/s)"};
    if (section < 0 || section >= kHeaders.size())
        return QVariant();
    return kHeaders.at(section);
}

void MetricsTableModel::SetSamples(const QVector<MetricsSample>& samples)
{
    beginResetModel();
    _samples = samples;
    endResetModel();
}
