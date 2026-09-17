#include "AgentControlClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

AgentControlClient::AgentControlClient(const QString& socketPath, QObject* parent)
    : QObject(parent), _socketPath(socketPath)
{
}

void AgentControlClient::Start() { SendCommand(QStringLiteral(R"({"cmd":"start"})")); }
void AgentControlClient::Stop() { SendCommand(QStringLiteral(R"({"cmd":"stop"})")); }
void AgentControlClient::Reconnect() { SendCommand(QStringLiteral(R"({"cmd":"reconnect"})")); }

void AgentControlClient::SetLogLevel(int level)
{
    QJsonObject obj{ {"cmd", "set_log_level"}, {"level", level} };
    SendCommand(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

void AgentControlClient::SendCommand(const QString& cmdLine)
{
    // 명령마다 새 소켓 - "배경"에 적은 대로 AgentControlServer는 연결 하나당 명령 하나만
    // 처리하고 끝난다. this를 부모로 둬서, MainWindow가 먼저 파괴되면 Qt 부모-자식 체계가
    // 이 소켓들도 같이 정리해준다.
    auto* socket = new QLocalSocket(this);

    connect(socket, &QLocalSocket::connected, socket, [socket, cmdLine]()
    {
        socket->write((cmdLine + "\n").toUtf8());
    });

    connect(socket, &QLocalSocket::readyRead, this, [this, socket]()
    {
        if (!socket->canReadLine())
            return;

        QByteArray line = socket->readLine().trimmed();
        socket->disconnectFromServer();
        socket->deleteLater();

        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        {
            emit CommandResult(false, "invalid response: " + QString::fromUtf8(line));
            return;
        }

        QJsonObject obj = doc.object();
        emit CommandResult(obj.value("ok").toBool(false), obj.value("error").toString());
    });

    connect(socket, &QLocalSocket::errorOccurred, this,
        [this, socket](QLocalSocket::LocalSocketError)
        {
            emit CommandResult(false, socket->errorString());
            socket->deleteLater();
        });

    socket->connectToServer(_socketPath);
}
