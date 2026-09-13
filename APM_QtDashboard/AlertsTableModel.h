#pragma once
#include <QAbstractTableModel>
#include <QVector>

#include "MetricsRepository.h"

// MetricsTableModel과 같은 이유 - AlertRecords 조회 결과 전용 모델.
class AlertsTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit AlertsTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

public slots:
    void SetAlerts(const QVector<AlertSample>& alerts);

private:
    QVector<AlertSample> _alerts;
};
