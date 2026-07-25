#pragma once
#include "pch.h"
#include "AriaCipher.h"
#include "HmacUtil.h"
#include "IPayloadSealer.h"

/*------------------
    SecurePayLoad
--------------------*/
/*
    [참고용 - 2026-07-20부로 Agent/Collector 실행 경로에서 더 이상 사용하지 않음]
    Agent<->Collector 구간도 AesGcmPayload(AES-256-GCM)로 통일 - ARIA-CBC+HMAC
    Encrypt-then-MAC 구현/검증 경험 자체는 Docs/ARIA_TO_AES_MIGRATION.md에 남겨둠.
    이 클래스는 GTest(tests/)로 자동 검증되지 않았음 - 마이그레이션 사유 중 하나.

    AriaCipher(기밀성) + HmacUtil(무결성)을 결합한 Encrypt-then-MAC 래퍼
    와이어 포맷 : [IV(168)][ciphertext(가변)][HMAC tag(32B)]
    Open()은 반드시 MAC을 검증하고, 통과한 경우에만 복호화, 순서를 바꾸면 안된다.
*/

class SecurePayload : public IPayloadSealer
{
public:
    SecurePayload(const AriaCipher::Key& encKey, const HmacUtil::Key& macKey);

    std::vector<BYTE> Seal(const String& plaintext) override;
    String Open(const std::vector<BYTE>& wire) override;

private:
    AriaCipher _cipher;
    HmacUtil::Key _macKey;
};