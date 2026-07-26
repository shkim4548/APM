using System.Diagnostics;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;

namespace ApmConsole.Domain.Apm.Infrastructure;

/*-----------
    TraceScope
-------------*/
// .NET 쪽 계측 SDK - IAsyncDisposable로 진입~탈출 구간을 자동 측정해서 TransactionSpanRecord로
// 직접 저장. Collector(C++)와 달리 네트워크를 거칠 필요가 없음 - Console 프로세스가 이미
// ApmDbContext(DB)를 갖고 있으므로 곧장 씀(2026-07-26 4순위 설계). Dispose가 아니라
// DisposeAsync를 쓴 이유: 저장이 SaveChangesAsync(비동기 DB I/O)라서.
public sealed class TraceScope : IAsyncDisposable
{
    private readonly ApmDbContext _db;
    private readonly string _operationName;
    private readonly Stopwatch _stopwatch;
    private bool _success = true;

    private TraceScope(ApmDbContext db, string operationName)
    {
        _db = db;
        _operationName = operationName;
        _stopwatch = Stopwatch.StartNew();
    }

    public static TraceScope Start(ApmDbContext db, string operationName) => new(db, operationName);

    // 계측 대상 코드가 실패를 명시적으로 표시할 때 호출(기본은 성공으로 간주) - 예외가
    // 스코프를 빠져나가도 자동으로 실패 처리되진 않음(과설계 방지, ScopedSpan.MarkFailed()와 동일 설계).
    public void MarkFailed() => _success = false;

    public async ValueTask DisposeAsync()
    {
        _stopwatch.Stop();

        var record = new TransactionSpanRecord
        {
            Ts = DateTimeOffset.UtcNow,
            Source = "Console",
            OperationName = _operationName,
            DurationUs = _stopwatch.ElapsedTicks * 1_000_000 / Stopwatch.Frequency,
            Success = _success,
        };

        _db.TransactionSpans.Add(record);
        await _db.SaveChangesAsync();
    }
}
