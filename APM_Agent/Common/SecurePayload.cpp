#include "pch.h"
#include "SecurePayload.h"

SecurePayload::SecurePayload(const AriaCipher::Key &encKey, const HmacUtil::Key &macKey)
    : _cipher(encKey), _macKey(macKey)
{
}

std::vector<BYTE> SecurePayload::Seal(const String &plaintext)
{
    AriaCipher::Iv iv{};
    std::vector<BYTE> ciphertext = _cipher.Encrypt(plaintext, iv);

    std::vector<BYTE> ivAndCipher;
    ivAndCipher.reserve(iv.size() + ciphertext.size());
    ivAndCipher.insert(ivAndCipher.end(), iv.begin(), iv.end());
    ivAndCipher.insert(ivAndCipher.end(), ciphertext.begin(), ciphertext.end());

    HmacUtil::Tag tag = HmacUtil::Compute(_macKey, ivAndCipher);

    std::vector<BYTE> wire = std::move(ivAndCipher);
    wire.insert(wire.end(), tag.begin(), tag.end());
    return wire;
}

String SecurePayload::Open(const std::vector<BYTE> &wire)
{
    constexpr size_t MIN_SIZE = AriaCipher::IV_SIZE + HmacUtil::TAG_SIZE;
    if (wire.size() < MIN_SIZE)
        throw std::runtime_error("SecurePayload::Open - 데이터 크기가 너무 작음(변조 의심)");

    size_t cipherLen = wire.size() - AriaCipher::IV_SIZE - HmacUtil::TAG_SIZE;

	std::vector<BYTE> ivAndCipher(wire.begin(), wire.begin() + AriaCipher::IV_SIZE + cipherLen);
	HmacUtil::Tag tag{};
	std::copy(wire.end() - HmacUtil::TAG_SIZE, wire.end(), tag.begin());

	// *** MAC을 먼저 검증 - 복호화는 검증 통과 후에만 수행 (순서 절대 바꾸지 말 것) ***
	if (!HmacUtil::Verify(_macKey, ivAndCipher, tag))
		throw std::runtime_error("SecurePayload::Open - 무결성 검증 실패(변조 또는 잘못된 키)");

	AriaCipher::Iv iv{};
	std::copy(ivAndCipher.begin(), ivAndCipher.begin() + AriaCipher::IV_SIZE, iv.begin());
	std::vector<BYTE> ciphertext(ivAndCipher.begin() + AriaCipher::IV_SIZE, ivAndCipher.end());

	return _cipher.Decrypt(ciphertext, iv);
}
