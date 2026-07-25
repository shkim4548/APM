#include "pch.h"
#include "ApmSession.h"

#ifdef _WIN32
#include <mstcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace
{
	constexpr size_t HEADER_SIZE = sizeof(PacketHeader);
}

ApmSession::ApmSession(asio::ip::tcp::socket socket, asio::ssl::context& sslContext, SessionMode mode,
	std::unique_ptr<IPayloadSealer> sealer)
	: _sslStream(std::move(socket), sslContext), _mode(mode), _payloadSealer(std::move(sealer))
{
}

void ApmSession::Start(ReadyCallback onReady, DisconnectedCallback onDisconnected, PacketCallback onPacket)
{
	_onReady = std::move(onReady);
	_onDisconnected = std::move(onDisconnected);
	_onPacket = std::move(onPacket);
	DoHandshake();
}

void ApmSession::DoHandshake()
{
	auto self = shared_from_this();
	auto handshakeType = (_mode == SessionMode::Server)
		? asio::ssl::stream_base::server
		: asio::ssl::stream_base::client;

	_sslStream.async_handshake(handshakeType,
		[this, self](const asio::error_code& ec)
		{
			if (ec)
			{
				std::cerr << "[ApmSession] handshake failed: " << ec.message() << std::endl;
				NotifyDisconnected();
				return;
			}

			if (_onReady)
				_onReady();

			RegisterRecv();
		});
}

void ApmSession::RegisterRecv()
{
	auto self = shared_from_this();
	_sslStream.async_read_some(asio::buffer(_recvBuffer),
		[this, self](const asio::error_code& ec, size_t bytes)
		{
			if (ec)
			{
				std::cerr << "[ApmSession] recv failed: " << ec.message() << std::endl;
				NotifyDisconnected();
				return;
			}

			_accumulated.insert(_accumulated.end(), _recvBuffer.begin(), _recvBuffer.begin() + bytes);
			ProcessAccumulated();

			RegisterRecv();
		});
}

void ApmSession::ProcessAccumulated()
{
	size_t processed = 0;

	while (true)
	{
		size_t remaining = _accumulated.size() - processed;
		if (remaining < HEADER_SIZE)
			break;

		const PacketHeader* header = reinterpret_cast<const PacketHeader*>(_accumulated.data() + processed);
		if (header->size < HEADER_SIZE)
		{
			std::cerr << "[ApmSession] invalid packet header, dropping connection" << std::endl;
			_accumulated.clear();
			NotifyDisconnected();
			return;
		}

		if (remaining < header->size)
			break;   // 아직 페이로드까지 다 안 모임 - 다음 recv를 기다림

		uint16 sealedSize = header->size - static_cast<uint16>(HEADER_SIZE);
		std::vector<BYTE> sealed(
			_accumulated.data() + processed + HEADER_SIZE,
			_accumulated.data() + processed + HEADER_SIZE + sealedSize);

		try
		{
			String payload = _payloadSealer->Open(sealed);

			if (_onPacket)
				_onPacket(header->id, payload);
		}
		catch (const std::exception& e)
		{
			// MAC 불일치/변조/키 불일치 - 이 연결을 더 신뢰할 수 없으므로 끊음.
			// 실패 사유를 상대에게 응답하지 않음(정보 노출로 공격 표면을 넓히지 않기 위함).
			std::cerr << "[ApmSession] payload verify/decrypt failed: " << e.what() << " - dropping connection" << std::endl;
			_accumulated.clear();
			NotifyDisconnected();
			return;
		}

		processed += header->size;
	}

	if (processed > 0)
		_accumulated.erase(_accumulated.begin(), _accumulated.begin() + processed);
}

void ApmSession::Send(uint16 id, const String& payload, SendCallback onComplete)
{
	auto self = shared_from_this();

	std::vector<BYTE> sealed = _payloadSealer->Seal(payload);

	if (sealed.size() + HEADER_SIZE > 0xFFFF)
		throw std::runtime_error("ApmSession::Send - sealed payload too large for uint16 size field");

	uint16 totalSize = static_cast<uint16>(HEADER_SIZE + sealed.size());
	auto buffer = std::make_shared<std::vector<BYTE>>(totalSize);

	PacketHeader header{ totalSize, id };
	std::memcpy(buffer->data(), &header, HEADER_SIZE);
	std::memcpy(buffer->data() + HEADER_SIZE, sealed.data(), sealed.size());

	asio::async_write(_sslStream, asio::buffer(*buffer),
		[this, self, buffer, onComplete](const asio::error_code& ec, size_t /*bytes*/)
		{
			if (ec)
			{
				std::cerr << "[ApmSession] send failed: " << ec.message() << std::endl;
				NotifyDisconnected();
				if (onComplete)
					onComplete(false);
				return;
			}

			if (onComplete)
				onComplete(true);
		});
}

void ApmSession::NotifyDisconnected()
{
	if (_disconnectedNotified)
		return;

	_disconnectedNotified = true;
	if (_onDisconnected)
		_onDisconnected();
}

TcpConnectionInfo ApmSession::GetConnectionInfo()
{
	TcpConnectionInfo info;

#ifdef _WIN32
	TCP_INFO_v0 tcpInfo{};
	DWORD tcpInfoVersion = 0;
	DWORD bytesReturned = 0;
	SOCKET sock = _sslStream.lowest_layer().native_handle();

	if (::WSAIoctl(sock, SIO_TCP_INFO, &tcpInfoVersion, sizeof(tcpInfoVersion),
		&tcpInfo, sizeof(tcpInfo), &bytesReturned, nullptr, nullptr) == 0)
	{
		info.rttMicros = tcpInfo.RttUs;
		info.sndCwnd = tcpInfo.Cwnd;
		// Windows TCP_INFO_v0에는 Linux tcpi_rttvar(RTT 변동성)에 대응하는 필드가 없고,
		// tcpi_retransmits(현재 미확인 재전송 세그먼트 수)도 없음 - BytesRetrans는 세그먼트가
		// 아닌 누적 바이트 수라 단위가 달라 그대로 매핑하지 않음. rttVarMicros/retransmits/
		// totalRetrans는 Windows API 자체의 한계로 0 유지(호출자는 이미 "실패 시 0" 계약을
		// 알고 있어 그대로 안전).
	}
#else
	struct tcp_info tcpInfo;
	socklen_t len = sizeof(tcpInfo);
	int fd = _sslStream.lowest_layer().native_handle();

	if (::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &tcpInfo, &len) == 0)
	{
		info.rttMicros = tcpInfo.tcpi_rtt;
		info.rttVarMicros = tcpInfo.tcpi_rttvar;
		info.retransmits = tcpInfo.tcpi_retransmits;
		info.totalRetrans = tcpInfo.tcpi_total_retrans;
		info.sndCwnd = tcpInfo.tcpi_snd_cwnd;
	}
#endif

	return info;
}
