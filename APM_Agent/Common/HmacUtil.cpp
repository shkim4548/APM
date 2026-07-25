#include "pch.h"
#include "HmacUtil.h"
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>

namespace
{
	HmacUtil::Tag ComputeInternal(const HmacUtil::Key& key, const std::vector<BYTE>& data)
	{
		EVP_MAC* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
		if (mac == nullptr)
			throw std::runtime_error("HmacUtil - EVP_MAC_fetch 실패");

		EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
		EVP_MAC_free(mac);
		if (ctx == nullptr)
			throw std::runtime_error("HmacUtil - EVP_MAC_CTX_new 실패");

		char digestName[] = "SHA256";
		OSSL_PARAM params[] = {
			OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digestName, 0),
			OSSL_PARAM_construct_end()
		};

		HmacUtil::Tag tag{};
		size_t tagLen = 0;

		if (EVP_MAC_init(ctx, key.data(), key.size(), params) != 1 ||
			EVP_MAC_update(ctx, data.data(), data.size()) != 1 ||
			EVP_MAC_final(ctx, tag.data(), &tagLen, tag.size()) != 1)
		{
			EVP_MAC_CTX_free(ctx);
			throw std::runtime_error("HmacUtil - HMAC 계산 실패");
		}

		EVP_MAC_CTX_free(ctx);
		return tag;
	}
}

HmacUtil::Tag HmacUtil::Compute(const Key& key, const std::vector<BYTE>& data)
{
	return ComputeInternal(key, data);
}

bool HmacUtil::Verify(const Key& key, const std::vector<BYTE>& data, const Tag& tag)
{
	Tag computed = ComputeInternal(key, data);
	return CRYPTO_memcmp(computed.data(), tag.data(), computed.size()) == 0;
}
