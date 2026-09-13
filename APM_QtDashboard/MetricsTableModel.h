#pragma once
#include <QAbstractTableModel>
#include <QVector>

#include "MetricsRepository.h"

// step 4 : QTableWidget의 setItem() 대신 데이터(모델)와 표시(뷰)를 분리한다.
// 이 모델은 QVector<MetricsSample>만 들고 있고, QTableView가 data()/headerData()로
// 필요한 값을 그때그때 물어본다 - 셀마다 위젯 아이템 객체를 만들지 않는다.
class MetricsTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit MetricsTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

public slots:
    void SetSamples(const QVector<MetricsSample>& samples);

private:
    QVector<MetricsSample> _samples;
};
