#pragma once

/*--------------
	NetAddress
----------------*/

class NetAddress
{
public:
	NetAddress() = default;
	NetAddress(asio::ip::tcp::endpoint endpoint);
	NetAddress(string ip, uint16_t port);

	asio::ip::tcp::endpoint		GetEndPoint() { return _endpoint; }
	string						GetIpAddress() { return _endpoint.address().to_string(); }
	uint16_t					GetPort() { return _endpoint.port(); }

private:
	asio::ip::tcp::endpoint _endpoint;
};

