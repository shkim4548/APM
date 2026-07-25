#include "pch.h"
#include "NetAddress.h"

/*---------------
	NetAddress
-----------------*/

NetAddress::NetAddress(asio::ip::tcp::endpoint endpoint)
	: _endpoint(endpoint)
{
}

NetAddress::NetAddress(string ip, uint16_t port)
{
	_endpoint = asio::ip::tcp::endpoint(asio::ip::make_address(ip), port);
}
