#include "pch.h"
#include "ResilientSender.h"
#include "PacketHandler.h"

ResilientSender::ResilientSender(asio::io_context& ioContext, asio::ssl::context& sslContext,
	String host, unsigned short port,
	SealerFactory sealerFactory, size_t maxQueueSize)
	: _ioContext(ioContext), _sslContext(sslContext), _host(std::move(host)), _port(port)
	, _sealerFactory(std::move(sealerFactory)), _maxQueueSize(maxQueueSize), _reconnectTimer(ioContext)
{
	Connect();
}

void ResilientSender::Connect()
{
	auto resolver = std::make_shared<asio::ip::tcp::resolver>(_ioContext);
	resolver->async_resolve(_host, std::to_string(_port),
		[this, resolver](const asio::error_code& ec, asio::ip::tcp::resolver::results_type endpoints)
		{
			if (ec)
			{
				std::cerr << "[ResilientSender] resolve failed: " << ec.message() << std::endl;
				ScheduleReconnect();
				return;
			}

			auto socket = std::make_shared<asio::ip::tcp::socket>(_ioContext);
			asio::async_connect(*socket, endpoints,
				[this, socket](const asio::error_code& ec, const asio::ip::tcp::endpoint&)
				{
					if (ec)
					{
						std::cerr << "[ResilientSender] connect failed: " << ec.message() << std::endl;
						ScheduleReconnect();
						return;
					}

					std::cout << "[ResilientSender] connected" << std::endl;

					_session = std::make_shared<ApmSession>(std::move(*socket), _sslContext, SessionMode::Client, _sealerFactory());
					_session->Start(
						[this]()
						{
							std::cout << "[ResilientSender] TLS handshake complete" << std::endl;
							_connected = true;
							FlushNext();
						},
						[this]()
						{
							OnSessionDisconnected();
						},
						&PacketHandler::Dispatch);
				});
		});
}

void ResilientSender::ScheduleReconnect()
{
	_reconnectTimer.expires_after(std::chrono::seconds(5));
	_reconnectTimer.async_wait(
		[this](const asio::error_code& ec)
		{
			if (!ec)
				Connect();
		});
}

void ResilientSender::OnSessionDisconnected()
{
	std::cerr << "[ResilientSender] disconnected, will retry" << std::endl;
	_connected = false;
	_sending = false;
	_session = nullptr;
	ScheduleReconnect();
}

void ResilientSender::EnqueueRaw(uint16 id, String payload)
{
	if (_queue.size() >= _maxQueueSize)
	{
		std::cerr << "[ResilientSender] queue full, dropping oldest buffered item" << std::endl;
		_queue.pop_front();
	}

	_queue.push_back(QueuedPacket{ id, std::move(payload) });
	std::cout << "[ResilientSender] queued (size=" << _queue.size() << ")" << std::endl;

	if (_connected && !_sending)
		FlushNext();
}

void ResilientSender::FlushNext()
{
	if (_queue.empty() || !_connected || _sending || !_session)
		return;

	_sending = true;
	QueuedPacket packet = _queue.front();

	_session->Send(packet.id, packet.payload,
		[this](bool success)
		{
			_sending = false;

			if (success)
			{
				if (!_queue.empty())
					_queue.pop_front();
				FlushNext();
			}
			// 실패하면 큐에 그대로 남겨둠 - OnSessionDisconnected가 별도로 호출되어
			// 재연결을 예약하고, 재연결 성공 시 FlushNext()가 이 항목부터 다시 재시도함.
		});
}

TcpConnectionInfo ResilientSender::GetConnectionInfo() const
{
	if (_connected && _session)
		return _session->GetConnectionInfo();

	return TcpConnectionInfo{};
}
