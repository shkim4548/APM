#include "pch.h"
#include "MetricsBroadcastServer.h"

// TODO(Windows 지원 시): ::unlink는 POSIX 전용(Agent의 AgentControlServer.cpp와 같은 제약,
// Linux/WSL 우선 - Docs/QT_MFC_PORTFOLIO_PLAN.md §8).
#include <unistd.h>

MetricsBroadcastServer::MetricsBroadcastServer(asio::io_context& ioContext, const String& socketPath)
    : _ioContext(ioContext), _socketPath(socketPath), _acceptor(ioContext)
{
    ::unlink(_socketPath.c_str());

    asio::local::stream_protocol::endpoint endpoint(_socketPath);
    _acceptor.open(endpoint.protocol());
    _acceptor.bind(endpoint);
    _acceptor.listen();
}

MetricsBroadcastServer::~MetricsBroadcastServer()
{
    ::unlink(_socketPath.c_str());
}

void MetricsBroadcastServer::Start()
{
    _running = true;
    AcceptNext();
}

void MetricsBroadcastServer::AcceptNext()
{
    auto socket = std::make_shared<asio::local::stream_protocol::socket>(_ioContext);
    _acceptor.async_accept(*socket,
        [this, socket](const asio::error_code& ec)
        {
            if (!ec)
            {
                std::cout << "[MetricsBroadcastServer] subscriber connected (총 "
                    << (_subscribers.size() + 1) << "개)" << std::endl;
                _subscribers.push_back(socket);
            }
            if (_running)
                AcceptNext();
        });
}

void MetricsBroadcastServer::Notify()
{
    static const String kPingLine = "{\"event\":\"new_metric\"}\n";

    // 끊긴 구독자는 지워가면서 순회 - erase-remove 관용구.
    for (auto it = _subscribers.begin(); it != _subscribers.end(); )
    {
        auto socket = *it;
        asio::error_code ec;
        asio::write(*socket, asio::buffer(kPingLine), ec);

        if (ec)
            it = _subscribers.erase(it);
        else
            ++it;
    }
}
