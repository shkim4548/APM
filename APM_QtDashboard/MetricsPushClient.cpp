#include "MetricsPushClient.h"

#include <QLocalSocket>
#include <QTimer>

namespace
{
    constexpr int kReconnectIntervalMs = 3000;
}

MetricsPushClient::MetricsPushClient(const QString& socketPath, QObject* parent)
    : QObject(parent), _socketPath(socketPath)
{
    _socket = new QLocalSocket(this);
    connect(_socket, &QLocalSocket::readyRead, this, &MetricsPushClient::OnReadyRead);
    connect(_socket, &QLocalSocket::disconnected, this, &MetricsPushClient::OnDisconnected);

    _reconnectTimer = new QTimer(this);
    _reconnectTimer->setInterval(kReconnectIntervalMs);
    connect(_reconnectTimer, &QTimer::timeout, this, &MetricsPushClient::TryConnect);
}

void MetricsPushClient::Start()
{
    TryConnect();
    _reconnectTimer->start();
}

void MetricsPushClient::TryConnect()
{
    if (_socket->state() == QLocalSocket::ConnectedState)
        return;

    _socket->connectToServer(_socketPath);
}

void MetricsPushClient::OnReadyRead()
{
    // 줄 단위로 읽되 내용은 실제로 안 본다 - "뭔가 왔다"는 사실 자체가 신호다.
    while (_socket->canReadLine())
    {
        _socket->readLine();
        emit NewMetricAvailable();
    }
}

void MetricsPushClient::OnDisconnected()
{
    // 별도 처리 없음 - _reconnectTimer가 주기적으로 TryConnect()를 계속 시도한다.
}
