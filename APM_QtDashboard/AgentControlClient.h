#pragma once
#include <QObject>
#include <QString>

// step 7' : Agent의 AgentControlServer(APM_Agent/Agent/AgentControlServer.h/.cpp)에
// 제어 명령을 보내는 전용 클라이언트. 서버가 연결 하나당 명령 하나만 처리하고 끝나므로
// (HandleConnection이 한 줄 읽고 응답 쓰고 끝 - 루프 없음), 매 호출마다 새로 연결한다.
// MetricsPushClient(발행-구독, 계속 열어두는 구독 소켓)와는 성격이 달라 별개 클래스로 둠.
class AgentControlClient : public QObject
{
    Q_OBJECT
public:
    explicit AgentControlClient(const QString& socketPath, QObject* parent = nullptr);

public slots:
    void Start();
    void Stop();
    void Reconnect();
    void SetLogLevel(int level);

signals:
    // 명령 1개에 대한 결과 - AgentControlServer의 {"ok":true}/{"ok":false,"error":"..."} 그대로.
    // 명령 종류는 안 실어보낸다 - MainWindow가 호출 시점에 이미 어떤 명령인지 알고 있고,
    // 버튼 4개가 동시에 눌릴 일이 없다는 전제(버튼 클릭 -> 즉시 이 신호 하나로 결과 표시).
    void CommandResult(bool ok, const QString& error);

private:
    void SendCommand(const QString& cmdLine);

private:
    QString _socketPath;
};
