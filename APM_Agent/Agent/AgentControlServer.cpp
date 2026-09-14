#include "pch.h"
#include "AgentControlServer.h"
#include "LogLevel.h"
#include <nlohmann/json.hpp>

// TODO(Windows 지원 시): ::unlink는 POSIX 전용 - 이 파일 전체가 지금은 Linux/WSL만
// 대상으로 한다(§8). Windows 네임드파이프로 갈 땐 이 파일을 통째로 갈아끼워야 함.
#include <unistd.h>

using json = nlohmann::json;

AgentControlServer::AgentControlServer(asio::io_context& ioContext, const String& socketPath,
    MetricScheduler& scheduler, ResilientSender& sender)
    : _ioContext(ioContext), _socketPath(socketPath), _scheduler(scheduler), _sender(sender)
    , _acceptor(ioContext)
{
    // 이전 실행이 비정상 종료돼 소켓 파일이 남아있으면 bind가 "Address already in use"로
    // 실패하므로 미리 지운다 - 흔한 Unix domain socket 관용구.
    ::unlink(_socketPath.c_str());

    asio::local::stream_protocol::endpoint endpoint(_socketPath);
    _acceptor.open(endpoint.protocol());
    _acceptor.bind(endpoint);
    _acceptor.listen();
}

AgentControlServer::~AgentControlServer()
{
    ::unlink(_socketPath.c_str());
}

void AgentControlServer::Start()
{
    _running = true;
    AcceptNext();
}

void AgentControlServer::AcceptNext()
{
    auto socket = std::make_shared<asio::local::stream_protocol::socket>(_ioContext);
    _acceptor.async_accept(*socket,
        [this, socket](const asio::error_code& ec)
        {
            if (!ec)
                HandleConnection(socket);
            if (_running)
                AcceptNext();
        });
}

void AgentControlServer::HandleConnection(std::shared_ptr<asio::local::stream_protocol::socket> socket)
{
    auto buffer = std::make_shared<asio::streambuf>();

    asio::async_read_until(*socket, *buffer, '\n',
        [this, socket, buffer](const asio::error_code& ec, size_t /*bytesTransferred*/)
        {
            if (ec)
                return;

            std::istream is(buffer.get());
            String line;
            std::getline(is, line);

            String response = Dispatch(line) + "\n";
            asio::async_write(*socket, asio::buffer(response),
                [socket](const asio::error_code&, size_t) {});
        });
}

String AgentControlServer::Dispatch(const String& line)
{
    try
    {
        json request = json::parse(line);
        String cmd = request.at("cmd").get<String>();

        if (cmd == "start")
        {
            _scheduler.Start();
            _sender.Resume();
        }
        else if (cmd == "stop")
        {
            _scheduler.Stop();
            _sender.Pause();
        }
        else if (cmd == "reconnect")
        {
            _sender.ForceReconnect();
        }
        else if (cmd == "set_log_level")
        {
            SetLogLevel(request.at("level").get<int>());
        }
        else
        {
            return json{ {"ok", false}, {"error", "unknown command: " + cmd} }.dump();
        }

        return json{ {"ok", true} }.dump();
    }
    catch (const std::exception& e)
    {
        return json{ {"ok", false}, {"error", String(e.what())} }.dump();
    }
}
