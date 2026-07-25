#pragma once
#include "pch.h"

/*-------------
	AriaCipher
---------------*/
// ARIA-256-CBC 암복호화 (OpenSSL EVP). 기밀성만 제공 — 무결성은 HmacUtil과
// 결합(Encrypt-then-MAC)해야 안전. 단독 사용 금지.

class AriaCipher
{
public:
	static constexpr int32 KEY_SIZE = 32;   // ARIA-256
	static constexpr int32 IV_SIZE = 16;    // ARIA 블록 크기(AES와 동일)

	using Key = std::array<BYTE, KEY_SIZE>;
	using Iv = std::array<BYTE, IV_SIZE>;

	explicit AriaCipher(const Key& key);

	// 매 호출마다 새 IV를 내부에서 생성해서 outIv에 채움 (IV 재사용 절대 금지)
	std::vector<BYTE> Encrypt(const String& plaintext, Iv& outIv);

	// 호출자가 별도로 보관해둔 IV를 그대로 넘겨받아 복호화
	String Decrypt(const std::vector<BYTE>& ciphertext, const Iv& iv);

private:
	Key _key;
};
