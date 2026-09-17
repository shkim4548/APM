#pragma once
#include "pch.h"

// 2026-09-14(§6 후속) : Collector가 새 지표를 저장할 때마다 로컬로 붙어있는 구독자(Qt)에게
// "뭔가 새로 생겼다"는 핑만 보낸다(발행-구독, 페이로드 없음 - 실제 값은 구독자가 기존
// SQL 조회로 알아서 다시 읽는다). Unix domain socket, 여러 구독자 동시 지원.
class MetricsBroadcastServer
{
public:
    explicit MetricsBroadcastServer(asio::io_context& ioContext, const String& socketPath);
    ~MetricsBroadcastServer();

    void Start();

    // 연결된 모든 구독자에게 핑 한 줄을 보낸다. 반드시 io_context 스레드에서 호출해야
    // 한다(Asio 소켓은 스레드 안전하지 않음) - 다른 스레드에서 부를 땐 asio::post로 넘길 것
    // (Collector/main.cpp 참고).
    void Notify();

private:
    void AcceptNext();

private:
    asio::io_context& _ioContext;
    String _socketPath;
    asio::local::stream_protocol::acceptor _acceptor;
    std::vector<std::shared_ptr<asio::local::stream_protocol::socket>> _subscribers;
    bool _running = false;
};
