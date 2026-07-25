#pragma once
#include "pch.h"

/*-----------
	HmacUtil
-------------*/
// HMAC-SHA256 계산/검증 (Encrypt-then-MAC 패턴의 MAC 부분).
// AriaCipher의 암호화 결과(IV+ciphertext)에 대해 이 태그를 계산해서 같이 전송해야
// 무결성이 보장됨 — AriaCipher 단독 사용 금지 원칙과 쌍을 이룸.

class HmacUtil
{
public:
    static constexpr int32 KEY_SIZE = 32; // HMAC 키 (SHA-256 출력 크기와 동일하게 권장)
    static constexpr int32 TAG_SIZE = 32; // SHA-256 출력 크기

    using Key = std::array<BYTE, KEY_SIZE>;
    using Tag = std::array<BYTE, TAG_SIZE>;

    static Tag Compute(const Key& key, const std::vector<BYTE>& data);

    // 상수 시간 비교로 검증
    // 타이밍 공격 방지 : 절대 Tag끼리 ==로 비교해선 안된다.
    static bool Verify(const Key& key, const std::vector<BYTE>& data, const Tag& tag);
};