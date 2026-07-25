#pragma once
#include "pch.h"
#include <array>

/*--------------
    AesGcmCipher
----------------*/
// AES-256-GCM 암복호화 (OpenSSL EVP). GCM은 AEAD라 암호화와 무결성 검증을 한 번에
// 처리함 - ARIA-CBC와 달리 별도 HMAC 조합이 필요 없고, 패딩이 없어 패딩 오라클
// 공격면 자체가 존재하지 않음. Collector<->WebServer 구간 전용(C++<->.NET 언어 경계,
// 양쪽 다 표준 라이브러리로 지원하는 알고리즘을 씀).

class AesGcmCipher
{
public:
    static constexpr int32 KEY_SIZE = 32; // AES-256
    static constexpr int32 NONCE_SIZE = 12; // GCM 표준 nonce(96비트)
    static constexpr int32 TAG_SIZE = 16;   // GCM 인증 태그 크기

    using Key = std::array<BYTE, KEY_SIZE>;
    using Nonce = std::array<BYTE, NONCE_SIZE>;
    using Tag = std::array<BYTE, TAG_SIZE>;

    explicit AesGcmCipher(const Key& key);

    // 매 호출마다 새 nonce를 내부에서 생성해서 outNonce에 채움(nonce 재사용 절대 금지 -
    // CBC의 IV 재사용보다 GCM의 nonce 재사용이 더 치명적, 인증 자체가 깨질 수 있음).
    // outTag에는 GCM 인증 태그가 채워짐 - 이 태그가 없으면 Decrypt에서 검증 불가.
    std::vector<BYTE> Encrypt(const String& plaintext, Nonce& outNonce, Tag& outTag);

    // nonce/tag는 호출자가 별도 보관해둔 값을 그대로 넘겨받아 복호화 + 태그 검증을
    // 한 번에 수행(EVP_DecryptFinal_ex 안에서 태그 불일치 시 실패 반환).
    String Decrypt(const std::vector<BYTE>& ciphertext, const Nonce& nonce, const Tag& tag);

private:
    Key _key;
};