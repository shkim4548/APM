#include "pch.h"
#include "AesGcmCipher.h"
#include <openssl/evp.h>
#include <openssl/rand.h>

AesGcmCipher::AesGcmCipher(const Key &key)
    : _key(key)
{
}

std::vector<BYTE> AesGcmCipher::Encrypt(const String &plaintext, Nonce &outNonce, Tag &outTag)
{
    if (RAND_bytes(outNonce.data(), static_cast<int32>(outNonce.size())) != 1)
        throw std::runtime_error("AesGcmCipher::Encrypt - nonce 생성 실패");

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr)
        throw std::runtime_error("AesGcmCipher::Encrypt - EVP_CIPHER_CTX_new 실패");
    
    std::vector<BYTE> ciphertext(plaintext.size());
    int32 outLen1 = 0;
    int32 outLen2 = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NONCE_SIZE, nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, _key.data(), outNonce.data()) != 1 ||
        EVP_EncryptUpdate(ctx, ciphertext.data(), &outLen1,
            reinterpret_cast<const BYTE*>(plaintext.data()), static_cast<int32>(plaintext.size())) != 1 ||
        EVP_EncryptFinal_ex(ctx, ciphertext.data() + outLen1, &outLen2) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, outTag.data()) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AesGcmCipher::Encrypt - 암호화 실패");
    }

    EVP_CIPHER_CTX_free(ctx);
    ciphertext.resize(outLen1 + outLen2);
    return ciphertext;
}

String AesGcmCipher::Decrypt(const std::vector<BYTE> &ciphertext, const Nonce &nonce, const Tag &tag)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr)
        throw std::runtime_error("AesGcmCipher::Decrypt - EVP_CIPHER_CTX_new 실패");

    std::vector<BYTE> plaintext(ciphertext.size());
    int32 outLen1 = 0;
    int32 outLen2 = 0;
    Tag tagCopy = tag;   // EVP_CTRL_GCM_SET_TAG가 non-const 포인터를 요구함

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NONCE_SIZE, nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, _key.data(), nonce.data()) != 1 ||
        EVP_DecryptUpdate(ctx, plaintext.data(), &outLen1,
            ciphertext.data(), static_cast<int32>(ciphertext.size())) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE, tagCopy.data()) != 1 ||
        EVP_DecryptFinal_ex(ctx, plaintext.data() + outLen1, &outLen2) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        // 태그 불일치(변조/키 불일치) 시 EVP_DecryptFinal_ex가 실패를 반환 - 정상 동작.
        throw std::runtime_error("AesGcmCipher::Decrypt - 복호화 실패(키 불일치 또는 데이터 변조)");
    }

    EVP_CIPHER_CTX_free(ctx);
    plaintext.resize(outLen1 + outLen2);
    return String(plaintext.begin(), plaintext.end());
}
