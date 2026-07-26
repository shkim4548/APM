using Microsoft.EntityFrameworkCore;

namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

/*---------------
    ApmDbContext
-----------------*/
// WebServer 자체 소유의 메트릭 저장소(SQLite/TimescaleDB 선택 가능) - Collector가 네트워크로
// 보내주는 데이터를 MetricsReceiverService가 이 컨텍스트를 통해 씀. 예전에는 Collector의
// SQLite 파일을 직접 읽기만 했지만(같은 장비 전제), 이제 완전히 독립된 저장소라 스키마도
// EF Core 기본 관례를 그대로 씀(수동 컬럼명 매핑/keyless 설정 불필요).

public class ApmDbContext : DbContext
{
    public ApmDbContext(DbContextOptions<ApmDbContext> options) : base(options) { }

    public DbSet<MetricRecord> Metrics => Set<MetricRecord>();
    public DbSet<AlertThreshold> AlertThresholds => Set<AlertThreshold>();
    public DbSet<AlertRecord> AlertRecords => Set<AlertRecord>();
    public DbSet<TransactionSpanRecord> TransactionSpans => Set<TransactionSpanRecord>();

    // EnsureCreated()가 스키마를 처음 만들 때 이 시드 데이터도 같이 넣어줌(마이그레이션 없이도
    // 동작 - MetricsReceiverService가 EnsureCreated()를 쓰는 기존 방식 그대로 유지).
    // CPU/메모리/디스크는 90%, RTT는 200ms(200,000us) - 로컬 테스트 환경 기준 보수적인 기본값.
    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        modelBuilder.Entity<AlertThreshold>().HasData(
            new AlertThreshold { Id = 1, MetricType = AlertMetricType.CpuPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 2, MetricType = AlertMetricType.MemoryPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 3, MetricType = AlertMetricType.DiskPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 4, MetricType = AlertMetricType.TcpRttUs, Value = 200_000, Enabled = true }
        );

        // RetentionService가 매시간 Ts/ClosedAt 기준으로 WHERE 삭제를 돌리므로 인덱스가 없으면
        // 테이블이 커질수록 이 삭제 자체가 풀스캔이 됨(2026-07-26 3순위 설계) - AlertRecords.ClosedAt
        // 인덱스는 EvaluateAlertsAsync/AlertsController의 "현재 열린 알림" 조회에도 같이 도움됨.
        // 주의: EnsureCreated()는 신규 DB 파일에만 이 인덱스를 만듦 - 이미 존재하는 apm_metrics.db/
        // webserver_apm.db 파일에는 소급 적용 안 됨(마이그레이션 시스템이 아니라 EnsureCreated라
        // 스키마 변경이 자동 반영되지 않음 - 기존 파일은 수동 CREATE INDEX 또는 파일 재생성 필요).
        modelBuilder.Entity<MetricRecord>().HasIndex(m => m.Ts);
        modelBuilder.Entity<AlertRecord>().HasIndex(a => a.ClosedAt);

        // 5순위(백분위 통계)가 "특정 OperationName의 최근 N분 구간" 같은 쿼리를 돌릴 걸 감안한
        // 복합 인덱스 - RetentionService의 Ts 기준 삭제에도 같이 도움됨(2026-07-26 4순위 설계).
        modelBuilder.Entity<TransactionSpanRecord>().HasIndex(s => new { s.OperationName, s.Ts });
    }
}