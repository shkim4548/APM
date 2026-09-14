#pragma once
#include "pch.h"
#include "MetricScheduler.h"
#include "ResilientSender.h"

// step Phase A : Qt가 로컬 소켓으로 보내는 제어 명령을 받아 처리한다.
// 프로토콜: 줄바꿈으로 구분된 JSON 한 줄 = 명령 하나, 응답도 같은 형식 한 줄.
//   {"cmd":"start"} / {"cmd":"stop"} / {"cmd":"reconnect"} / {"cmd":"set_log_level","level":3}
//   -> {"ok":true} 또는 {"ok":false,"error":"..."}
// Unix domain socket만 지원(Linux/WSL 우선 - Docs/QT_MFC_PORTFOLIO_PLAN.md §8,
// Windows 네임드파이프는 스트레치 목표로 미착수).
class AgentControlServer
{
public:
    AgentControlServer(asio::io_context& ioContext, const String& socketPath,
        MetricScheduler& scheduler, ResilientSender& sender);
    ~AgentControlServer();

    void Start();

private:
    void AcceptNext();
    void HandleConnection(std::shared_ptr<asio::local::stream_protocol::socket> socket);
    String Dispatch(const String& line);

private:
    asio::io_context& _ioContext;
    String _socketPath;
    MetricScheduler& _scheduler;
    ResilientSender& _sender;
    asio::local::stream_protocol::acceptor _acceptor;
    bool _running = false;
};
