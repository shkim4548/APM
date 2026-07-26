namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

// 알림을 걸 지표 종류. int로 DB에 저장됨(EF Core enum 기본 매핑) - 값 순서를 바꾸면
// 기존 DB의 저장된 정수와 어긋나므로, 항목을 추가할 땐 반드시 끝에만 추가할 것.
public enum AlertMetricType
{
    CpuPercent,
    MemoryPercent,
    DiskPercent,
    TcpRttUs,
}

// 지표별 임계치 설정 - /apm/alerts에서 편집 가능(DB에 저장, appsettings.json 아님 -
// 재시작 없이 바꿀 수 있어야 한다는 요구사항 때문, 2026-07-26 결정).
public class AlertThreshold
{
    public int Id { get; set; }
    public AlertMetricType MetricType { get; set; }
    public double Value { get; set; }
    public bool Enabled { get; set; } = true;
}
