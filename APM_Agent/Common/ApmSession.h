#pragma once
#include "pch.h"
#include "IPayloadSealer.h"

/*-------------
	ApmSession
---------------*/
// GW2_CrossPlatformCore의 Session과는 독립된, APM_Agent 전용 TLS 세션.
// Session은 이 작업에서 수정하지 않음 (설계 결정: SESSION_LOG.md 2026-07-09 참고).
// 암복호화 방식은 IPayloadSealer로 분리되어 있음 - Agent<->Collector는 SecurePayload
// (ARIA+HMAC), Collector<->WebServer는 AesGcmPayload(AES-GCM)를 넘겨서 재사용함.

enum class SessionMode
{
	Server,	// Collector 쪽 - accept로 받은 연결, 서버로서 핸드셰이크
	Client,	// Agent 쪽 - 밖으로 connect한 연결, 클라이언트로서 핸드셰이크
};

// 패킷 헤더: GW2_ServerCore의 PacketHeader와 동일한 발상(size, id)
struct PacketHeader
{
	uint16 size;   // 헤더 포함 전체 패킷 크기(IPayloadSealer로 감싼 이후 크기 기준)
	uint16 id;     // Protobuf 메시지의 descriptor()->index() 값
};

// TCP_INFO에서 뽑아온 연결 품질 지표 (TLS 레이어 아래, 순수 TCP 계층 상태).
struct TcpConnectionInfo
{
	uint32 rttMicros = 0;       // 왕복 시간(RTT), 마이크로초
	uint32 rttVarMicros = 0;    // RTT 변동성(지터에 해당), 마이크로초
	uint32 retransmits = 0;     // 현재 미확인 재전송 횟수
	uint32 totalRetrans = 0;    // 연결 시작 이후 누적 재전송 총량
	uint32 sndCwnd = 0;         // 혼잡 윈도우(세그먼트 단위)
};

class ApmSession : public std::enable_shared_from_this<ApmSession>
{
public:
	using ReadyCallback = std::function<void()>;
	using DisconnectedCallback = std::function<void()>;
	using SendCallback = std::function<void(bool success)>;
	using PacketCallback = std::function<void(uint16 id, const String& payload)>;

	// sealer: 이 세션이 쓸 암복호화 방식(SecurePayload=ARIA+HMAC 또는 AesGcmPayload=AES-GCM).
	// 세션이 소유권을 가짐 - 호출자는 만들어서 넘기기만 하면 됨.
	ApmSession(asio::ip::tcp::socket socket, asio::ssl::context& sslContext, SessionMode mode,
		std::unique_ptr<IPayloadSealer> sealer);

	// onReady: 핸드셰이크 완료 시 1회. onDisconnected: 연결 끊김 감지 시 1회(중복 방지).
	// onPacket: 완전한 패킷 하나가 도착할 때마다 호출(경계 처리+복호화는 ApmSession이 전부 흡수).
	void Start(ReadyCallback onReady = nullptr, DisconnectedCallback onDisconnected = nullptr, PacketCallback onPacket = nullptr);

	// 저수준 전송 - id/payload를 이미 알고 있을 때(주로 SendPacket<T>/ResilientSender 내부에서 사용).
	// payload는 IPayloadSealer::Seal()로 암호화+무결성 태그가 붙은 뒤 전송됨(호출자는 신경 안 써도 됨).
	void Send(uint16 id, const String& payload, SendCallback onComplete = nullptr);

	// 고수준 전송 - Protobuf 메시지 타입을 그대로 넘기면 ID 결정+직렬화를 자동으로 처리.
	// PacketType은 반드시 Protobuf 생성 클래스여야 함(SerializeToString/descriptor() 필요).
	template<typename PacketType>
	void SendPacket(const PacketType& pkt, SendCallback onComplete = nullptr)
	{
		String payload;
		if (!pkt.SerializeToString(&payload))
			throw std::runtime_error("ApmSession::SendPacket - serialization failed");

		uint16 id = static_cast<uint16>(PacketType::descriptor()->index());
		Send(id, payload, std::move(onComplete));
	}

	// 이 세션이 감싸고 있는 TCP 소켓의 현재 연결 품질을 커널에서 직접 조회.
	// 실패(getsockopt 오류) 시 전부 0인 기본값 반환 - 호출자가 예외 처리를 안 해도 되게.
	TcpConnectionInfo GetConnectionInfo();

private:
	void DoHandshake();
	void RegisterRecv();
	void ProcessAccumulated();
	void NotifyDisconnected();

private:
	asio::ssl::stream<asio::ip::tcp::socket> _sslStream;
	std::array<BYTE, 4096> _recvBuffer;
	std::vector<BYTE> _accumulated;
	SessionMode _mode;
	std::unique_ptr<IPayloadSealer> _payloadSealer;
	ReadyCallback _onReady;
	DisconnectedCallback _onDisconnected;
	PacketCallback _onPacket;
	bool _disconnectedNotified = false;
};
