#include <gtest/gtest.h>
#include "AesGcmCipher.h"
#include "AesGcmPayload.h"
#include "IPayloadSealer.h"

namespace
{
    AesGcmCipher::Key MakeKey(BYTE fill = 0x42)
    {
        AesGcmCipher::Key key{};
        key.fill(fill);
        return key;
    }
}

// ------------------------
//   AesGcmCipher (저수준)
// ------------------------

TEST(AesGcmCipher, 왕복하면_원본_평문을_그대로_복원한다)
{
    AesGcmCipher cipher(MakeKey());
    String plaintext = "hello apm metric payload";

    AesGcmCipher::Nonce nonce{};
    AesGcmCipher::Tag tag{};
    std::vector<BYTE> ciphertext = cipher.Encrypt(plaintext, nonce, tag);
    String decrypted = cipher.Decrypt(ciphertext, nonce, tag);

    EXPECT_EQ(decrypted, plaintext);
}

TEST(AesGcmCipher, 호출할때마다_다른_nonce를_생성한다)
{
    AesGcmCipher cipher(MakeKey());
    AesGcmCipher::Nonce nonce1{}, nonce2{};
    AesGcmCipher::Tag tag1{}, tag2{};

    cipher.Encrypt("same plaintext", nonce1, tag1);
    cipher.Encrypt("same plaintext", nonce2, tag2);

    EXPECT_NE(nonce1, nonce2); // nonce 재사용은 GCM에서 치명적 - 매번 새로 생성돼야 함
}

TEST(AesGcmCipher, 태그가_변조되면_복호화가_실패한다)
{
    AesGcmCipher cipher(MakeKey());
    AesGcmCipher::Nonce nonce{};
    AesGcmCipher::Tag tag{};
    std::vector<BYTE> ciphertext = cipher.Encrypt("secret metric", nonce, tag);

    tag[0] ^= 0xFF; // 태그 변조

    EXPECT_THROW(cipher.Decrypt(ciphertext, nonce, tag), std::runtime_error);
}

TEST(AesGcmCipher, 암호문이_변조되면_복호화가_실패한다)
{
    AesGcmCipher cipher(MakeKey());
    AesGcmCipher::Nonce nonce{};
    AesGcmCipher::Tag tag{};
    std::vector<BYTE> ciphertext = cipher.Encrypt("secret metric", nonce, tag);

    ciphertext[0] ^= 0xFF; // 암호문 변조(태그는 그대로)

    EXPECT_THROW(cipher.Decrypt(ciphertext, nonce, tag), std::runtime_error);
}

TEST(AesGcmCipher, 다른_키로는_복호화되지_않는다)
{
    AesGcmCipher encryptCipher(MakeKey(0x01));
    AesGcmCipher decryptCipher(MakeKey(0x02)); // 다른 키

    AesGcmCipher::Nonce nonce{};
    AesGcmCipher::Tag tag{};
    std::vector<BYTE> ciphertext = encryptCipher.Encrypt("secret metric", nonce, tag);

    EXPECT_THROW(decryptCipher.Decrypt(ciphertext, nonce, tag), std::runtime_error);
}

// ------------------------
//   AesGcmPayload (와이어 포맷 - IPayloadSealer)
// ------------------------

TEST(AesGcmPayload, Seal_Open_왕복하면_원본_평문을_그대로_복원한다)
{
    AesGcmPayload sealer(MakeKey());
    String plaintext = "apm::Metric serialized bytes (stand-in)";

    std::vector<BYTE> wire = sealer.Seal(plaintext);
    String opened = sealer.Open(wire);

    EXPECT_EQ(opened, plaintext);
}

TEST(AesGcmPayload, 와이어_포맷은_Nonce12B_ciphertext_Tag16B_순서다)
{
    AesGcmPayload sealer(MakeKey());
    String plaintext = "fixed length check";

    std::vector<BYTE> wire = sealer.Seal(plaintext);

    // [Nonce(12B)][ciphertext(plaintext와 같은 길이 - GCM은 스트림 암호라 패딩 없음)][Tag(16B)]
    EXPECT_EQ(wire.size(), AesGcmCipher::NONCE_SIZE + plaintext.size() + AesGcmCipher::TAG_SIZE);
}

TEST(AesGcmPayload, 변조된_와이어는_Open에서_거부된다)
{
    AesGcmPayload sealer(MakeKey());
    std::vector<BYTE> wire = sealer.Seal("tamper me");

    wire.back() ^= 0xFF; // 마지막 바이트(Tag의 일부) 변조

    EXPECT_THROW(sealer.Open(wire), std::runtime_error);
}

TEST(AesGcmPayload, IPayloadSealer_인터페이스로도_동일하게_동작한다)
{
    // ApmSession/ResilientSender가 실제로 쓰는 방식 그대로 - 구체 타입이 아니라
    // 인터페이스 포인터를 통해 Seal/Open이 호출돼도 문제없는지 확인.
    std::unique_ptr<IPayloadSealer> sealer = std::make_unique<AesGcmPayload>(MakeKey());
    String plaintext = "via interface";

    std::vector<BYTE> wire = sealer->Seal(plaintext);
    String opened = sealer->Open(wire);

    EXPECT_EQ(opened, plaintext);
}
