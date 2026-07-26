using ApmConsole.Domain.Apm.Infrastructure;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using ApmConsole.Domain.Apm.Models;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace ApmConsole.Domain.Apm.Controllers;

[Area("Apm")]
[Route("apm/traces")]
public class TracesController : Controller
{
    private readonly ApmDbContext _db;

    public TracesController(ApmDbContext db)
    {
        _db = db;
    }

    // window: "1h" | "24h" | "7d" - 기본 1시간(2026-07-26 5순위 결정: 사용자가 링크로 선택).
    // 인식 못 하는 값은 "1h"로 취급(방어적) - 쿼리스트링을 직접 조작해도 안전하게 기본값으로 수렴.
    [HttpGet]
    public async Task<IActionResult> Index(string window = "1h")
    {
        window = window is "24h" or "7d" ? window : "1h";

        var lookback = window switch
        {
            "24h" => TimeSpan.FromHours(24),
            "7d" => TimeSpan.FromDays(7),
            _ => TimeSpan.FromHours(1),
        };
        var cutoff = DateTimeOffset.UtcNow - lookback;

        // 조회 시점에 계산(사전 집계 테이블 없음, 2026-07-26 5순위 결정) - GroupBy를 SQL로
        // 번역시키지 않고 필요한 컬럼만 뽑아 메모리로 가져온 뒤 C#에서 묶음 - SQLite/TimescaleDB
        // 백엔드 차이(SQLite는 PERCENTILE_CONT 같은 SQL 백분위 함수가 없음)를 아예 우회.
        // OperationName+Ts 복합 인덱스(ApmDbContext, 4순위)가 이 WHERE+묶음에 그대로 맞음.
        var spans = await _db.TransactionSpans
            .AsNoTracking()
            .Where(s => s.Ts >= cutoff)
            .Select(s => new { s.OperationName, s.DurationUs, s.Success })
            .ToListAsync();

        var rows = spans
            .GroupBy(s => s.OperationName)
            .Select(g =>
            {
                var sorted = g.Select(s => s.DurationUs).OrderBy(us => us).ToList();
                return new OperationStatsRow(
                    OperationName: g.Key,
                    Count: sorted.Count,
                    P50Ms: PercentileCalculator.Compute(sorted, 50) / 1000.0,
                    P95Ms: PercentileCalculator.Compute(sorted, 95) / 1000.0,
                    P99Ms: PercentileCalculator.Compute(sorted, 99) / 1000.0,
                    SuccessRatePercent: g.Count(s => s.Success) * 100.0 / g.Count());
            })
            .OrderByDescending(r => r.Count)
            .ToList();

        return View(new TracesViewModel(window, rows));
    }
}
