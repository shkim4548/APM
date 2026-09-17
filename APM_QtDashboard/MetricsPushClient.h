#pragma once
#include <QObject>
#include <QString>

class QLocalSocket;
class QTimer;

// MetricBroadcast Server의 수신자
class MetricsPushClient : public QObject
{
    Q_OBJECT
public:
    explicit MetricsPushClient(const QString& socketPath, QObject* parent = nullptr);

public slots:
    void Start();

signals:
    void NewMetricAvailable();

private slots:
    void OnReadyRead();
    void OnDisconnected();
    void TryConnect();

private:
    QString _socketPath;
    QLocalSocket* _socket = nullptr;
    QTimer* _reconnectTimer = nullptr;
};