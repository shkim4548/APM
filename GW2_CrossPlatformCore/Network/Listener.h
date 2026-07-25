#pragma once
#include <optional>

class ServerService;

/*--------------
	Listener
---------------*/

class Listener : public enable_shared_from_this<Listener>
{
public:
	Listener() = default;
	virtual ~Listener();

public:
	bool StartAccept(shared_ptr<ServerService> service);
	void CloseSocket();

private:
	void RegisterAccept();
	void ProcessAccept(asio::error_code ec, SessionRef session);

private:
	optional<asio::ip::tcp::acceptor> _acceptor;
	shared_ptr<ServerService> _service;
};

