#include "pch.h"
#include "AesGcmPayload.h"

AesGcmPayload::AesGcmPayload(const AesGcmCipher::Key &key)
    : _cipher(key)
{
}

std::vector<BYTE> AesGcmPayload::Seal(const String &plaintext)
{
    AesGcmCipher::Nonce nonce{};
    AesGcmCipher::Tag tag{};
    std::vector<BYTE> ciphertext = _cipher.Encrypt(plaintext, nonce, tag);

    std::vector<BYTE> wire;
    wire.reserve(nonce.size() + ciphertext.size() + tag.size());
    wire.insert(wire.end(), nonce.begin(), nonce.end());
    wire.insert(wire.end(), ciphertext.begin(), ciphertext.end());
    wire.insert(wire.end(), tag.begin(), tag.end());

    return wire;
}

String AesGcmPayload::Open(const std::vector<BYTE> &wire)
{
    constexpr size_t MIN_SIZE = AesGcmCipher::NONCE_SIZE + AesGcmCipher::TAG_SIZE;
    if(wire.size() < MIN_SIZE)
        throw std::runtime_error("AesGcmPayload::Open - 데이터 크기가 너무 작음(변조 의심)");

    size_t cipherLen = wire.size() - MIN_SIZE;

    AesGcmCipher::Nonce nonce {};
    std::copy(wire.begin(), wire.begin() + AesGcmCipher::NONCE_SIZE, nonce.begin());
    std::vector<BYTE> ciphertext(wire.begin() + AesGcmCipher::NONCE_SIZE, wire.begin() + AesGcmCipher::NONCE_SIZE + cipherLen);

    AesGcmCipher::Tag tag {};
    std::copy(wire.end() - AesGcmCipher::TAG_SIZE, wire.end(), tag.begin());

    return _cipher.Decrypt(ciphertext, nonce, tag);
}
