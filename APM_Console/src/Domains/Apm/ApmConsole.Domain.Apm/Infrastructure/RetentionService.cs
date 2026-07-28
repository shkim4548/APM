using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;

namespace ApmConsole.Domain.Apm.Infrastructure;

/*------------------
    RetentionService
--------------------*/
// Metrics/AlertRecords가 무한정 쌓이지 않도록 오래된 행을 주기적으로 삭제(2026-07-26, 3순위).
// 시작 직후 1회 실행 + 이후 1시간 간격 반복 - 엔티티를 메모리로 로드하지 않고 EF Core의
// ExecuteDeleteAsync(단일 DELETE ... WHERE 문으로 변환됨)로 서버 사이드에서 바로 삭제.
public class RetentionService : BackgroundService
{
    private readonly int _metricsRetentionDays;
    private readonly int _alertRetentionDays;
    private readonly IServiceScopeFactory _scopeFactory;

    public RetentionService(IConfiguration configuration, IServiceScopeFactory scopeFactory)
    {
        _metricsRetentionDays = int.Parse(configuration["Apm:MetricsRetentionDays"] ?? "30");
        _alertRetentionDays = int.Parse(configuration["Apm:AlertRetentionDays"] ?? "180");
        _scopeFactory = scopeFactory;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromHours(1));

        do
        {
            await PruneAsync(stoppingToken);
        }
        while (await timer.WaitForNextTickAsync(stoppingToken));
    }

    private async Task PruneAsync(CancellationToken ct)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        // SQLite EF Core 프로바이더가 DateTimeOffset 비교를 LINQ로 SQL 번역하지 못함
        // (InvalidOperationException, 2026-07-29 첫 실행 시 발견 - ExecuteDeleteAsync는 물론
        // 일반 Where(...).ToListAsync()조차 번역 실패. 그동안 빌드/단위테스트만 검증했지
        // 실제 실행은 안 해봐서 못 잡았던 버그). LINQ 번역을 아예 거치지 않는 파라미터화
        // raw SQL DELETE(ExecuteSqlInterpolatedAsync, 값은 파라미터 바인딩이라 인젝션 안전)로 우회.
        var metricsCutoff = DateTimeOffset.UtcNow.AddDays(-_metricsRetentionDays);
        var metricsDeleted = await db.Database.ExecuteSqlInterpolatedAsync(
            $"DELETE FROM Metrics WHERE Ts < {metricsCutoff}", ct);

        // 진행 중인 알림(ClosedAt == null)은 기간과 무관하게 항상 보존 - 오래됐다고 지우면
        // 활성 알림 목록에서 사라지는 버그가 됨(AlertsController.Index의 active 조회 기준과 동일).
        var alertsCutoff = DateTimeOffset.UtcNow.AddDays(-_alertRetentionDays);
        var alertsDeleted = await db.Database.ExecuteSqlInterpolatedAsync(
            $"DELETE FROM AlertRecords WHERE ClosedAt IS NOT NULL AND ClosedAt < {alertsCutoff}", ct);

        // TransactionSpans도 Metrics와 같은 고빈도 원본 데이터라 같은 보존 기간을 적용
        // (2026-07-26 4순위 설계 - 새 설정값을 따로 만들지 않고 기존 정책을 자연스럽게 확장).
        var spansDeleted = await db.Database.ExecuteSqlInterpolatedAsync(
            $"DELETE FROM TransactionSpans WHERE Ts < {metricsCutoff}", ct);

        if (metricsDeleted > 0 || alertsDeleted > 0 || spansDeleted > 0)
            Console.WriteLine($"[RetentionService] 정리 완료: metrics {metricsDeleted}건, alerts {alertsDeleted}건, spans {spansDeleted}건 삭제");
    }
}
