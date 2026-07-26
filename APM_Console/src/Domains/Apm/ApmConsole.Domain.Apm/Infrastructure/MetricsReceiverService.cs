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
using Microsoft.EntityFrameworkCore;
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
                    var packet = await ReadOnePacketAsync(sslStream, stoppingToken);
                    if (packet == null)
                        break;

                    var (id, sealedPayload) = packet.Value;

                    if (id == (ushort)Metric.Descriptor.Index)
                        await StoreAndBroadcastAsync(DecryptAndParse(sealedPayload, _aesKey));
                    else if (id == (ushort)TransactionSpan.Descriptor.Index)
                        await StoreSpanAsync(DecryptAndParseSpan(sealedPayload, _aesKey));
                    else
                        Console.WriteLine($"[MetricsReceiverService] 알 수 없는 패킷 id={id}, 무시");
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

    // 헤더만 읽고 id/암호화된 payload를 그대로 반환 - 어느 메시지 타입인지는 호출자
    // (HandleClientAsync)가 id로 분기해서 결정(2026-07-26 4순위, 메시지 타입이 2개가 되면서
    // 예전처럼 "무조건 Metric으로 파싱"할 수 없게 됨).
    private async Task<(ushort Id, byte[] SealedPayload)?> ReadOnePacketAsync(Stream stream, CancellationToken ct)
    {
        // PacketHeader{ uint16 size; uint16 id; } - C++와 동일하게 4바이트, 리틀엔디안.
        var header = await ReadExactAsync(stream, 4, ct);
        if (header == null)
            return null;

        ushort totalSize = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(0, 2));
        ushort id = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(2, 2));

        var sealedPayload = await ReadExactAsync(stream, totalSize - 4, ct);
        if (sealedPayload == null)
            return null;

        return (id, sealedPayload);
    }

    // Span<T>(ref struct)를 async 메서드 안에서 지역 변수로 두면 .NET 8/C# 12 기준
    // "ref and unsafe in async and iterator methods"가 preview 전용이라 컴파일 에러(CS8652) -
    // 이 로직만 별도의 동기 메서드로 분리해서 async 메서드 밖에 둠(preview 언어 기능 의존 회피).
    // static + aesKey를 파라미터로 받도록 함 - _aesKey(인스턴스 필드, 생성자가 파일 I/O로 채움) 대신
    // 순수 입력만으로 테스트할 수 있게(서비스 전체를 DI로 구성하지 않아도 됨).
    internal static Metric DecryptAndParse(byte[] sealedPayload, byte[] aesKey)
    {
        var plaintext = Unseal(sealedPayload, aesKey);
        return Metric.Parser.ParseFrom(plaintext);
    }

    // DecryptAndParse와 완전히 같은 와이어 포맷(AesGcmPayload::Seal 기준)에 메시지 타입만 다름 -
    // Unseal()로 복호화 로직(신경 써야 할 crypto 슬라이싱 부분)만 공유하고, 기존 DecryptAndParse의
    // 시그니처/테스트(DecryptAndParseTests.cs)는 그대로 둠(2026-07-26 4순위 설계 - 제네릭화 대신
    // 이 방식을 택한 이유는 기존 테스트 영향 없이 가장 작은 변경으로 끝내기 위함).
    internal static TransactionSpan DecryptAndParseSpan(byte[] sealedPayload, byte[] aesKey)
    {
        var plaintext = Unseal(sealedPayload, aesKey);
        return TransactionSpan.Parser.ParseFrom(plaintext);
    }

    private static byte[] Unseal(byte[] sealedPayload, byte[] aesKey)
    {
        // AesGcmPayload::Seal()의 와이어 포맷: [Nonce(12B)][ciphertext(가변)][Tag(16B)]
        var nonce = sealedPayload.AsSpan(0, NonceSize);
        var tag = sealedPayload.AsSpan(sealedPayload.Length - TagSize, TagSize);
        var ciphertext = sealedPayload.AsSpan(NonceSize, sealedPayload.Length - NonceSize - TagSize);

        var plaintext = new byte[ciphertext.Length];
        using var aesGcm = new AesGcm(aesKey, TagSize);
        aesGcm.Decrypt(nonce, ciphertext, tag, plaintext);

        return plaintext;
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

        await EvaluateAlertsAsync(db, record);
    }

    // 저장된 메트릭 1건에 대해 활성화된 임계치를 전부 평가 - 상태 전이(Opened/Resolved)가
    // 있을 때만 AlertRecord를 기록하고 SignalR로 알림(2026-07-26, WORK_STATUS.md 2순위).
    private async Task EvaluateAlertsAsync(ApmDbContext db, MetricRecord record)
    {
        var memPercent = record.MemTotalBytes > 0 ? record.MemUsedBytes * 100.0 / record.MemTotalBytes : 0;
        var diskPercent = record.DiskTotalBytes > 0 ? record.DiskUsedBytes * 100.0 / record.DiskTotalBytes : 0;

        var currentValues = new Dictionary<AlertMetricType, double>
        {
            [AlertMetricType.CpuPercent] = record.CpuUsagePercent,
            [AlertMetricType.MemoryPercent] = memPercent,
            [AlertMetricType.DiskPercent] = diskPercent,
            [AlertMetricType.TcpRttUs] = record.TcpRttUs,
        };

        var thresholds = await db.AlertThresholds.AsNoTracking().Where(t => t.Enabled).ToListAsync();

        foreach (var threshold in thresholds)
        {
            var currentValue = currentValues[threshold.MetricType];

            var openAlert = await db.AlertRecords
                .Where(a => a.MetricType == threshold.MetricType && a.ClosedAt == null)
                .OrderByDescending(a => a.Id)
                .FirstOrDefaultAsync();

            var transition = AlertEvaluator.Evaluate(currentValue, threshold.Value, openAlert != null);

            if (transition == AlertTransition.Opened)
            {
                var opened = new AlertRecord
                {
                    MetricType = threshold.MetricType,
                    ThresholdValue = threshold.Value,
                    TriggerValue = currentValue,
                    OpenedAt = DateTimeOffset.UtcNow,
                };
                db.AlertRecords.Add(opened);
                await db.SaveChangesAsync();

                Console.WriteLine($"[MetricsReceiverService] 알림 발생: {threshold.MetricType}={currentValue:F1} (임계치 {threshold.Value})");
                await _hubContext.Clients.All.SendAsync("AlertOpened", opened);
            }
            else if (transition == AlertTransition.Resolved && openAlert != null)
            {
                openAlert.ResolvedValue = currentValue;
                openAlert.ClosedAt = DateTimeOffset.UtcNow;
                await db.SaveChangesAsync();

                Console.WriteLine($"[MetricsReceiverService] 알림 해제: {threshold.MetricType}={currentValue:F1}");
                await _hubContext.Clients.All.SendAsync("AlertResolved", openAlert);
            }
        }
    }

    // Collector가 보낸 span을 저장만 함(대시보드 실시간 갱신은 이번 범위 밖 - 5순위에서
    // 집계 뷰를 만들 때 같이 고려, 2026-07-26 4순위 설계).
    private async Task StoreSpanAsync(TransactionSpan span)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        var record = new TransactionSpanRecord
        {
            Ts = DateTimeOffset.UtcNow,
            Source = "Collector",
            OperationName = span.OperationName,
            DurationUs = (long)span.DurationUs,
            Success = span.Success,
        };

        db.TransactionSpans.Add(record);
        await db.SaveChangesAsync();

        Console.WriteLine($"[MetricsReceiverService] span 저장: {record.OperationName} ({record.DurationUs}us, success={record.Success})");
    }
}