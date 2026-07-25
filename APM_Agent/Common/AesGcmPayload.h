#pragma once
#include "pch.h"
#include "AesGcmCipher.h"
#include "IPayloadSealer.h"

/*------------------
    AesGcmPayload
--------------------*/
// AES-256-GCM 기반 AEAD 래퍼. SecurePayload(ARIA+HMAC)와 달리 별도 MAC 키/조합이
// 필요 없음 - GCM 자체가 암호화와 인증을 한 번에 처리(Encrypt-then-MAC 조합 순서를
// 잘못 짜서 패딩 오라클이 재발할 여지가 구조적으로 없음).
// 와이어 포맷 : [Nonce(12B)][ciphertext(가변)][Tag(16B)]
// Collector<->WebServer 구간 전용(신규, C++<->.NET 언어 경계).

class AesGcmPayload : public IPayloadSealer
{
public:
    explicit AesGcmPayload(const AesGcmCipher::Key& key);

    std::vector<BYTE> Seal(const String& plaintext) override;
    String Open(const std::vector<BYTE>& wire) override;

private:
    AesGcmCipher _cipher;
};