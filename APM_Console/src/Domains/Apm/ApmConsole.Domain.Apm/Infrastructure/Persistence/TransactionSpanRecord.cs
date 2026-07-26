namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

// 함수/트랜잭션 1건 계측 결과 - Collector(C++, 네트워크로 수신)와 Console(.NET, 같은 프로세스라
// 직접 저장) 양쪽에서 다 이 테이블로 모임. Source로만 출처를 구분(2026-07-26 4순위 설계).
public class TransactionSpanRecord
{
    public int Id { get; set; }
    public DateTimeOffset Ts { get; set; }
    public string Source { get; set; } = "";          // "Collector" | "Console"
    public string OperationName { get; set; } = "";
    public long DurationUs { get; set; }
    public bool Success { get; set; }
}
