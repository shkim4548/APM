#pragma once
#include "pch.h"

/*----------------
	PacketHandler
------------------*/
// 패킷 ID -> 처리 함수 디스패치. ApmSession::Start()에 &PacketHandler::Dispatch를 그대로 넘긴다.
// Register<PacketType>(handler): ID 결정(descriptor()->index())과 역직렬화(ParseFromString)를
// 전부 자동으로 처리하고, 호출자의 handler는 이미 파싱된 강타입 메시지를 받는다.
// Collector/Agent가 각자 별도 프로세스라 정적(전역) 테이블로도 충돌 없음.

class PacketHandler
{
public:
	using RawHandler = std::function<void(const String& payload)>;

	template<typename PacketType>
	using TypedHandler = std::function<void(const PacketType& pkt)>;

	template<typename PacketType>
	static void Register(TypedHandler<PacketType> handler)
	{
		uint16 id = static_cast<uint16>(PacketType::descriptor()->index());

		Handlers()[id] = [handler](const String& payload)
		{
			PacketType pkt;
			if (!pkt.ParseFromString(payload))
			{
				std::cerr << "[PacketHandler] failed to parse packet (id mismatch or corrupted data)" << std::endl;
				return;
			}

			handler(pkt);
		};
	}

	static void Dispatch(uint16 id, const String& payload);

private:
	static std::unordered_map<uint16, RawHandler>& Handlers();
};
