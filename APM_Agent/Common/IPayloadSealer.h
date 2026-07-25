#pragma once
#include "pch.h"

/*------------------
    IPayloadSealer
--------------------*/
// ApmSession/ResilientSender가 특정 암호 방식에 고정되지 않도록 하는 인터페이스.
// SecurePayload(ARIA-CBC+HMAC, Agent<->Collector)와 AesGcmPayload(AES-256-GCM,
// Collector<->WebServer) 둘 다 이 인터페이스를 구현 - 세션 생성 시점에 어느 걸 쓸지만 결정하면 됨.

class IPayloadSealer
{
public:
    virtual ~IPayloadSealer() = default;
    virtual std::vector<BYTE> Seal(const String& plaintext) = 0;
    virtual String Open(const std::vector<BYTE>& wire) = 0;
};