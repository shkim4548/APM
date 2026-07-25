#include "pch.h"
#include "Listener.h"
#include "Session.h"
#include "Service.h"

Listener::~Listener()
{
	CloseSocket();
}

bool Listener::StartAccept(shared_ptr<ServerService> service)
{
	_service = service;
	if (_service == nullptr)
	{
		return false;
	}

	asio::io_context& ioc = service->GetIoCore()->GetContext();
	asio::ip::tcp::endpoint endpoint = service->GetNetAddress().GetEndPoint();

	asio::error_code ec;

	_acceptor.emplace(ioc);
	_acceptor->open(endpoint.protocol(), ec);
	if (ec)
	{
		return false;
	}

	_acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
	if (ec)
	{
		return false;
	}

	_acceptor->bind(endpoint, ec);
	if (ec)
	{
		return false;
	}

	_acceptor->listen(asio::socket_base::max_listen_connections, ec);
	if (ec) 
		return false;

	RegisterAccept();
	return true;
}

void Listener::CloseSocket()
{
	if (_acceptor.has_value() && _acceptor->is_open())
	{
		asio::error_code ec;
		_acceptor->close();
	}
}

void Listener::RegisterAccept()
{
	if (!_acceptor.has_value() || !_acceptor->is_open())
	{
		return;
	}

	SessionRef session = _service->CreateSession();

	auto self = shared_from_this();
	_acceptor->async_accept(session->GetSocket(),
		[self, session](asio::error_code ec)
		{
			self->ProcessAccept(ec, session);
		});
}

void Listener::ProcessAccept(asio::error_code ec, SessionRef session)
{
	if (!ec)
	{
		asio::error_code remoteEc;
		asio::ip::tcp::endpoint remote =
			session->GetSocket().remote_endpoint(remoteEc);

		if (!remoteEc)
			session->SetNetAddress(NetAddress(remote));

		session->ProcessConnect();
	}
	else
	{
		cout << "Accept Error : " << ec.message() << endl;
	}

	// 다음 클라이언트 대기
	RegisterAccept();
}