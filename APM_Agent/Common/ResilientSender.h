#pragma once
#include "pch.h"
#include "ApmSession.h"

/*-------------------
	ResilientSender
---------------------*/
// 상대(Collector 또는 WebServer)에 대한 연결을 유지/재연결하고, 연결이 끊긴 동안
// 보낼 데이터를 로컬 큐(메모리)에 쌓아뒀다가 재연결되면 순서대로 재전송한다.
// 큐가 가득 차면 가장 오래된 항목부터 버린다(최신 데이터가 더 중요하다는 가정).
// 주의: 메모리 큐라 프로세스 자체가 죽으면 큐 내용도 사라짐 (디스크 영속화는 범위 밖).

struct QueuedPacket
{
	uint16 id;
	String payload;
};

class ResilientSender
{
public:
	// sealerFactory: 재연결마다 새 ApmSession에 넘길 IPayloadSealer를 새로 만들어주는 함수.
	// (SecurePayload/AesGcmCipher 둘 다 키만 들고 있는 상태 없는 객체라 매번 새로 만들어도
	// 비용이 거의 없음 - 재연결마다 깨끗한 상태로 시작한다는 걸 보장하는 쪽을 택함.)
	// Agent->Collector는 [] { return std::make_unique<SecurePayload>(encKey, macKey); } 형태로,
	// Collector->WebServer는 [] { return std::make_unique<AesGcmPayload>(key); } 형태로 넘김.
	using SealerFactory = std::function<std::unique_ptr<IPayloadSealer>()>;

	ResilientSender(asio::io_context& ioContext, asio::ssl::context& sslContext,
		String host, unsigned short port,
		SealerFactory sealerFactory, size_t maxQueueSize = 100);

	// Protobuf 메시지를 직접 받아서 직렬화 후 큐에 넣음 - 타입 안전 진입점.
	template<typename PacketType>
	void Enqueue(const PacketType& pkt)
	{
		String payload;
		if (!pkt.SerializeToString(&payload))
		{
			std::cerr << "[ResilientSender] serialize failed" << std::endl;
			return;
		}

		uint16 id = static_cast<uint16>(PacketType::descriptor()->index());
		EnqueueRaw(id, std::move(payload));
	}

	// 현재 연결의 TCP 품질 조회. 연결 안 된 상태면 전부 0인 기본값.
	TcpConnectionInfo GetConnectionInfo() const;

private:
	void EnqueueRaw(uint16 id, String payload);
	void Connect();
	void ScheduleReconnect();
	void FlushNext();
	void OnSessionDisconnected();

private:
	asio::io_context& _ioContext;
	asio::ssl::context& _sslContext;
	String _host;
	unsigned short _port;
	SealerFactory _sealerFactory;
	size_t _maxQueueSize;

	std::deque<QueuedPacket> _queue;
	std::shared_ptr<ApmSession> _session;
	asio::steady_timer _reconnectTimer;
	bool _connected = false;
	bool _sending = false;
};
