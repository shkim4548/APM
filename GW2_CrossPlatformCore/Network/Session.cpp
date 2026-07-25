#include "pch.h"
#include "Session.h"
#include "IoCore.h"
#include "CoreGlobal.h"
#include "Service.h"
#include "SendBuffer.h"

/*-----------
	Session
-------------*/

Session::Session() 
	: _socket(GIoCore->GetContext()), _recvBuffer(BUFFER_SIZE)
{

}

Session::~Session()
{

}

void Session::Send(SendBufferRef sendBuffer)
{
	if (!_connected)
	{
		return;
	}

	bool registerSend = false;
	{
		WRITE_LOCK;
		_sendQueue.push(sendBuffer);
		if (_sendRegistered.exchange(true) == false)
			registerSend = true;

		if (registerSend)
			RegisterSend();
	}
}

bool Session::Connect()
{
	return RegisterConnect();
}

void Session::Disconnect(const string &cause)
{
	if (_connected.exchange(false) == false)
		return;

	cout << "Disconnect : " << cause << endl;

	asio::error_code ec;
	_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
	_socket.close(ec);

	ProcessDisconnect();
}

bool Session::RegisterConnect()
{
	auto service = GetService();
	if (!service)
		return false;

	NetAddress destAddr = service->GetNetAddress();

	auto self = GetSessionRef();
	_socket.async_connect(destAddr.GetEndPoint(),
		[this, self](const asio::error_code& ec)
		{
			if (!ec)
				ProcessConnect();
			else
				HandleError(ec);
		});

	return true;
}

void Session::RegisterRecv()
{
	if (!_connected)
	{
		return;
	}

	auto self = GetSessionRef();
	_socket.async_read_some(
		asio::buffer(_recvBuffer.WritePos(), _recvBuffer.FreeSize()),
		[this, self](const asio::error_code& ec, size_t numOfBytes)
		{
			if (!ec)
				ProcessRecv(static_cast<int32>(numOfBytes));
			else
				HandleError(ec);
		}
	);
}

void Session::RegisterSend()
{
	if (!_connected)
	{
		return;
	}

	{
		WRITE_LOCK;
		while (!_sendQueue.empty())
		{
			_sendingBuffers.push_back(_sendQueue.front());
			_sendQueue.pop();
		}
	}

	vector<asio::const_buffer> buffers;
	for (auto& buf : _sendingBuffers)
	{
		buffers.push_back(asio::buffer(buf->Buffer(), buf->WriteSize()));
	}

	auto self = GetSessionRef();
	asio::async_write(_socket, buffers,
		[this, self](const asio::error_code& ec, size_t numOfBytes)
		{
			if (!ec)
				ProcessSend(static_cast<int32>(numOfBytes));
			else
				HandleError(ec);
		});
}

void Session::ProcessConnect()
{
	_connected = true;
	GetService()->AddSession(GetSessionRef());

	OnConnected();
	RegisterRecv();
}

void Session::ProcessDisconnect()
{
	GetService()->ReleaseSession(GetSessionRef());
	OnDisconnected();
}

void Session::ProcessRecv(int32 numOfBytes)
{
	if (numOfBytes == 0)
	{
		Disconnect("Recv 0");
		return;
	}

	if (!_recvBuffer.OnWrite(numOfBytes))
	{
		Disconnect("OnWrite Overflow");
		return;
	}

	int32 dataSize = _recvBuffer.DataSize();
	int32 processLen = OnRecv(_recvBuffer.ReadPos(), dataSize);

	if (processLen < 0 || dataSize < processLen || !_recvBuffer.OnRead(processLen))
	{
		Disconnect("OnRead Overflow");
		return;
	}

	_recvBuffer.Clean();
	RegisterRecv();
}

void Session::ProcessSend(int32 numOfBytes)
{
	if (numOfBytes == 0)
	{
		Disconnect("Send 0");
		return;
	}

	_sendingBuffers.clear();
	OnSend(numOfBytes);

	bool registerSend = false;
	{
		WRITE_LOCK;
		if (!_sendQueue.empty())
		{
			if (_sendRegistered.exchange(true) == false)
				registerSend = true;
		}
		else
		{
			_sendRegistered.store(false);
		}
	}
	if (registerSend)
		RegisterSend();
}

void Session::HandleError(const asio::error_code& ec)
{
	if (ec == asio::error::eof || ec == asio::error::connection_reset)
		Disconnect("Disconnected");

	else if (ec != asio::error::operation_aborted)
		cout << "Session Error : " << ec.message() << endl;
}

/*-----------------
	PacketSession
-------------------*/

PacketSession::PacketSession() : Session()
{

}

PacketSession::~PacketSession()
{
}

int32 PacketSession::OnRecv(BYTE* buffer, int32 len)
{
	int32 processLen = 0;

	while (true)
	{
		int32 dataSize = len - processLen;
		if (dataSize < sizeof(PacketHeader))
			break;

		PacketHeader* header = reinterpret_cast<PacketHeader*>(&buffer[processLen]);
		if (dataSize < header->size)
			break;

		OnRecvPacket(&buffer[processLen], header->size);
		processLen += header->size;
	}

	return processLen;
}