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
}