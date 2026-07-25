using System.Buffers.Binary;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Apm;
using ApmConsole.Domain.Apm.Hubs;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using Microsoft.AspNetCore.SignalR;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;

namespace ApmConsole.Domain.Apm.Infrastructure;

/*------------------------
    MetricsReceiverService
--------------------------*/
// Collector가 주기적으로(또는 CLI 트리거로) 보내는 메트릭을 받는 TLS 리스너.
// PacketHeader{size,id} 프레이밍 + AES-256-GCM 복호화 + Protobuf 역직렬화를 직접 구현 -
// C++ Collector가 쓰는 것과 같은 와이어 포맷(APM_Agent의 ApmSession.cpp/AesGcmPayload.cpp
// 참고)을 그대로 맞춰야 함. 저장 직후 SignalR로 즉시 푸시(폴링 없음).

public class MetricsReceiverService : BackgroundService
{
    internal const int NonceSize = 12;
    internal const int TagSize = 16;

    private readonly int _port;
    private readonly byte[] _aesKey;
    private readonly X509Certificate2 _serverCert;
    private readonly IServiceScopeFactory _scopeFactory;
    private readonly IHubContext<MetricsHub> _hubContext;

    public MetricsReceiverService(IConfiguration configuration, IServiceScopeFactory scopeFactory, IHubContext<MetricsHub> hubContext)
    {
        _port = int.Parse(configuration["Apm:ReceiverPort"] ?? "9100");

        var keyPath = configuration["Apm:WebServerAesKeyPath"]
            ?? throw new InvalidOperationException("Apm:WebServerAesKeyPath 설정이 필요합니다.");
        _aesKey = Convert.FromHexString(File.ReadAllText(keyPath).Trim());

        var certPath = configuration["Apm:ReceiverCertPath"]
            ?? throw new InvalidOperationException("Apm:ReceiverCertPath 설정이 필요합니다.");
        var certKeyPath = configuration["Apm:ReceiverKeyPath"]
            ?? throw new InvalidOperationException("Apm:ReceiverKeyPath 설정이 필요합니다.");
        _serverCert = X509Certificate2.CreateFromPemFile(certPath, certKeyPath);

        _scopeFactory = scopeFactory;
        _hubContext = hubContext;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using (var scope = _scopeFactory.CreateScope())
        {
            var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();
            db.Database.EnsureCreated();
        }

        var listener = new TcpListener(IPAddress.Any, _port);
        listener.Start();
        Console.WriteLine($"[MetricsReceiverService] {_port}번 포트에서 Collector 연결 대기 중 (TLS)");

        try
        {
            while (!stoppingToken.IsCancellationRequested)
            {
                var client = await listener.AcceptTcpClientAsync(stoppingToken);
                _ = HandleClientAsync(client, stoppingToken);
            }
        }
        finally
        {
            listener.Stop();
        }
    }

    private async Task HandleClientAsync(TcpClient client, CancellationToken stoppingToken)
    {
        using (client)
        using (var sslStream = new SslStream(client.GetStream(), leaveInnerStreamOpen: false))
        {
            try
            {
                await sslStream.AuthenticateAsServerAsync(_serverCert, clientCertificateRequired: false,
                    checkCertificateRevocation: false);

                Console.WriteLine("[MetricsReceiverService] Collector 연결됨");

                while (!stoppingToken.IsCancellationRequested)
                {
                    var metric = await ReadOnePacketAsync(sslStream, stoppingToken);
                    if (metric == null)
                        break;

                    await StoreAndBroadcastAsync(metric);
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[MetricsReceiverService] 연결 처리 중 오류: {ex.Message}");
            }
        }
    }

    // internal(private 아님) - ReadExactAsync/DecryptAndParse는 순수 로직이라 테스트 대상으로 삼음
    // (ApmConsole.Domain.Apm.Tests에 InternalsVisibleTo로 접근 허용, AssemblyInfo.cs 참고).
    internal static async Task<byte[]?> ReadExactAsync(Stream stream, int length, CancellationToken ct)
    {
        var buffer = new byte[length];
        var offset = 0;
        while (offset < length)
        {
            var read = await stream.ReadAsync(buffer.AsMemory(offset, length - offset), ct);
            if (read == 0)
                return null;   // 연결 종료
            offset += read;
        }
        return buffer;
    }

    private async Task<Metric?> ReadOnePacketAsync(Stream stream, CancellationToken ct)
    {
        // PacketHeader{ uint16 size; uint16 id; } - C++와 동일하게 4바이트, 리틀엔디안.
        var header = await ReadExactAsync(stream, 4, ct);
        if (header == null)
            return null;

        ushort totalSize = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(0, 2));
        // id(header[2..4])는 지금 메시지 타입이 Metric 하나뿐이라 별도 분기 없이 무시.

        var sealedPayload = await ReadExactAsync(stream, totalSize - 4, ct);
        if (sealedPayload == null)
            return null;

        return DecryptAndParse(sealedPayload, _aesKey);
    }

    // Span<T>(ref struct)를 async 메서드 안에서 지역 변수로 두면 .NET 8/C# 12 기준
    // "ref and unsafe in async and iterator methods"가 preview 전용이라 컴파일 에러(CS8652) -
    // 이 로직만 별도의 동기 메서드로 분리해서 async 메서드 밖에 둠(preview 언어 기능 의존 회피).
    // static + aesKey를 파라미터로 받도록 함 - _aesKey(인스턴스 필드, 생성자가 파일 I/O로 채움) 대신
    // 순수 입력만으로 테스트할 수 있게(서비스 전체를 DI로 구성하지 않아도 됨).
    internal static Metric DecryptAndParse(byte[] sealedPayload, byte[] aesKey)
    {
        // AesGcmPayload::Seal()의 와이어 포맷: [Nonce(12B)][ciphertext(가변)][Tag(16B)]
        var nonce = sealedPayload.AsSpan(0, NonceSize);
        var tag = sealedPayload.AsSpan(sealedPayload.Length - TagSize, TagSize);
        var ciphertext = sealedPayload.AsSpan(NonceSize, sealedPayload.Length - NonceSize - TagSize);

        var plaintext = new byte[ciphertext.Length];
        using var aesGcm = new AesGcm(aesKey, TagSize);
        aesGcm.Decrypt(nonce, ciphertext, tag, plaintext);

        return Metric.Parser.ParseFrom(plaintext);
    }

    private async Task StoreAndBroadcastAsync(Metric metric)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        var record = new MetricRecord
        {
            Ts = DateTimeOffset.UtcNow,
            CpuUsagePercent = metric.CpuUsagePercent,
            MemUsedBytes = (long)metric.MemUsedBytes,
            MemTotalBytes = (long)metric.MemTotalBytes,
            DiskUsedBytes = (long)metric.DiskUsedBytes,
            DiskTotalBytes = (long)metric.DiskTotalBytes,
            NetRxBytesPerSec = (long)metric.NetRxBytesPerSec,
            NetTxBytesPerSec = (long)metric.NetTxBytesPerSec,
            TcpRttUs = (int)metric.TcpRttUs,
            TcpRttVarUs = (int)metric.TcpRttVarUs,
            TcpRetransmits = (int)metric.TcpRetransmits,
            TcpTotalRetrans = (int)metric.TcpTotalRetrans,
            TcpSndCwnd = (int)metric.TcpSndCwnd,
        };

        db.Metrics.Add(record);
        await db.SaveChangesAsync();

        Console.WriteLine($"[MetricsReceiverService] 저장 완료: cpu={record.CpuUsagePercent}%");

        await _hubContext.Clients.All.SendAsync("NewMetric", record);
    }
}