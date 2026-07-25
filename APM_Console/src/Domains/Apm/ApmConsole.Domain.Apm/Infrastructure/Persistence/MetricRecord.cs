namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

// WebServer가 Collector로부터 네트워크로 받은 메트릭을 저장하는 테이블 - WebServer가
// 직접 쓰기 때문에 일반적인 EF Core 엔티티(Id PK)로 구성. 예전에는 Collector의 SQLite
// 파일을 그대로 읽기만 하는 keyless 매핑이었지만, 이제 WebServer가 이 테이블의 유일한 소유자.

public class MetricRecord
{
    public int Id { get; set; }
    public DateTimeOffset Ts { get; set; }
    public double CpuUsagePercent { get; set; }
    public long MemUsedBytes { get; set; }
    public long MemTotalBytes { get; set; }
    public long DiskUsedBytes { get; set; }
    public long DiskTotalBytes { get; set; }
    public long NetRxBytesPerSec { get; set; }
    public long NetTxBytesPerSec { get; set; }
    public int TcpRttUs { get; set; }
    public int TcpRttVarUs { get; set; }
    public int TcpRetransmits { get; set; }
    public int TcpTotalRetrans { get; set; }
    public int TcpSndCwnd { get; set; }
}