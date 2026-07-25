#include "pch.h"
#include "AriaCipher.h"
#include <openssl/evp.h>
#include <openssl/rand.h>

AriaCipher::AriaCipher(const Key& key)
	: _key(key)
{
}

std::vector<BYTE> AriaCipher::Encrypt(const String& plaintext, Iv& outIv)
{
	if (RAND_bytes(outIv.data(), static_cast<int32>(outIv.size())) != 1)
		throw std::runtime_error("AriaCipher::Encrypt - IV 생성 실패");

	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	if (ctx == nullptr)
		throw std::runtime_error("AriaCipher::Encrypt - EVP_CIPHER_CTX_new 실패");

	std::vector<BYTE> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
	int32 outLen1 = 0;
	int32 outLen2 = 0;

	if (EVP_EncryptInit_ex(ctx, EVP_aria_256_cbc(), nullptr, _key.data(), outIv.data()) != 1 ||
		EVP_EncryptUpdate(ctx, ciphertext.data(), &outLen1,
			reinterpret_cast<const BYTE*>(plaintext.data()), static_cast<int32>(plaintext.size())) != 1 ||
		EVP_EncryptFinal_ex(ctx, ciphertext.data() + outLen1, &outLen2) != 1)
	{
		EVP_CIPHER_CTX_free(ctx);
		throw std::runtime_error("AriaCipher::Encrypt - 암호화 실패");
	}

	EVP_CIPHER_CTX_free(ctx);
	ciphertext.resize(outLen1 + outLen2);
	return ciphertext;
}

String AriaCipher::Decrypt(const std::vector<BYTE>& ciphertext, const Iv& iv)
{
	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	if (ctx == nullptr)
		throw std::runtime_error("AriaCipher::Decrypt - EVP_CIPHER_CTX_new 실패");

	std::vector<BYTE> plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
	int32 outLen1 = 0;
	int32 outLen2 = 0;

	if (EVP_DecryptInit_ex(ctx, EVP_aria_256_cbc(), nullptr, _key.data(), iv.data()) != 1 ||
		EVP_DecryptUpdate(ctx, plaintext.data(), &outLen1,
			ciphertext.data(), static_cast<int32>(ciphertext.size())) != 1 ||
		EVP_DecryptFinal_ex(ctx, plaintext.data() + outLen1, &outLen2) != 1)
	{
		EVP_CIPHER_CTX_free(ctx);
		// 패딩 검증 실패 등은 여기서 걸림 - 정상 동작(잘못된 키/변조된 데이터를 걸러냄)
		throw std::runtime_error("AriaCipher::Decrypt - 복호화 실패 (키 불일치 또는 데이터 변조)");
	}

	EVP_CIPHER_CTX_free(ctx);
	plaintext.resize(outLen1 + outLen2);
	return String(plaintext.begin(), plaintext.end());
}
