using System.Security.Cryptography;
using Apm;
using ApmConsole.Domain.Apm.Infrastructure;
using Google.Protobuf;

namespace ApmConsole.Domain.Apm.Tests.Infrastructure;

public class DecryptAndParseTests
{
    private static readonly byte[] Key = RandomNumberGenerator.GetBytes(32); // AES-256

    // AesGcmPayload::Seal()(C++, APM_Agent)과 같은 와이어 포맷으로 직접 봉인 -
    // [Nonce(12B)][ciphertext(가변)][Tag(16B)]. 프로덕션 코드(Collector)를 흉내내는 테스트 헬퍼.
    private static byte[] Seal(byte[] plaintext, byte[] key)
    {
        var nonce = RandomNumberGenerator.GetBytes(MetricsReceiverService.NonceSize);
        var ciphertext = new byte[plaintext.Length];
        var tag = new byte[MetricsReceiverService.TagSize];

        using var aesGcm = new AesGcm(key, MetricsReceiverService.TagSize);
        aesGcm.Encrypt(nonce, plaintext, ciphertext, tag);

        var wire = new byte[nonce.Length + ciphertext.Length + tag.Length];
        nonce.CopyTo(wire, 0);
        ciphertext.CopyTo(wire, nonce.Length);
        tag.CopyTo(wire, nonce.Length + ciphertext.Length);
        return wire;
    }

    private static Metric SampleMetric() => new()
    {
        CpuUsagePercent = 12.5,
        MemUsedBytes = 1024,
        MemTotalBytes = 8192,
        DiskUsedBytes = 100,
        DiskTotalBytes = 500,
        NetRxBytesPerSec = 111,
        NetTxBytesPerSec = 222,
        TcpRttUs = 333,
        TcpRttVarUs = 44,
        TcpRetransmits = 1,
        TcpTotalRetrans = 2,
        TcpSndCwnd = 10,
    };

    [Fact]
    public void 정상_페이로드는_복호화후_원래_필드값을_그대로_복원한다()
    {
        var original = SampleMetric();
        var wire = Seal(original.ToByteArray(), Key);

        var decrypted = MetricsReceiverService.DecryptAndParse(wire, Key);

        Assert.Equal(original.CpuUsagePercent, decrypted.CpuUsagePercent);
        Assert.Equal(original.MemUsedBytes, decrypted.MemUsedBytes);
        Assert.Equal(original.TcpRttUs, decrypted.TcpRttUs);
        Assert.Equal(original.TcpTotalRetrans, decrypted.TcpTotalRetrans);
        Assert.Equal(original.TcpSndCwnd, decrypted.TcpSndCwnd);
    }

    [Fact]
    public void 태그가_변조되면_복호화를_거부한다()
    {
        var wire = Seal(SampleMetric().ToByteArray(), Key);
        wire[^1] ^= 0xFF; // 마지막 바이트(Tag의 일부)를 뒤집어서 변조

        Assert.Throws<AuthenticationTagMismatchException>(() => MetricsReceiverService.DecryptAndParse(wire, Key));
    }

    [Fact]
    public void 암호문이_변조되면_복호화를_거부한다()
    {
        var wire = Seal(SampleMetric().ToByteArray(), Key);
        wire[MetricsReceiverService.NonceSize] ^= 0xFF; // ciphertext 첫 바이트 변조(Tag는 그대로)

        Assert.Throws<AuthenticationTagMismatchException>(() => MetricsReceiverService.DecryptAndParse(wire, Key));
    }

    [Fact]
    public void 다른_키로는_복호화되지_않는다()
    {
        var wire = Seal(SampleMetric().ToByteArray(), Key);
        var wrongKey = RandomNumberGenerator.GetBytes(32);

        Assert.Throws<AuthenticationTagMismatchException>(() => MetricsReceiverService.DecryptAndParse(wire, wrongKey));
    }
}
