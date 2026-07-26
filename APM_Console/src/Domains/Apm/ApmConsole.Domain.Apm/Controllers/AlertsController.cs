using ApmConsole.Domain.Apm.Infrastructure;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using ApmConsole.Domain.Apm.Models;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace ApmConsole.Domain.Apm.Controllers;

[Area("Apm")]
[Route("apm/alerts")]
public class AlertsController : Controller
{
    private readonly ApmDbContext _db;

    public AlertsController(ApmDbContext db)
    {
        _db = db;
    }

    [HttpGet]
    public async Task<IActionResult> Index()
    {
        // 이 액션 자체를 계측 데모로 삼음(2026-07-26 4순위) - 실제 운영 중인 컨트롤러
        // 액션이라 "우리 APM으로 우리 자신을 모니터링"하는 스토리에 맞음.
        await using var span = TraceScope.Start(_db, "AlertsController.Index");

        var thresholds = await _db.AlertThresholds.AsNoTracking()
            .OrderBy(t => t.MetricType)
            .ToListAsync();

        var active = await _db.AlertRecords.AsNoTracking()
            .Where(a => a.ClosedAt == null)
            .OrderByDescending(a => a.Id)
            .ToListAsync();

        var history = await _db.AlertRecords.AsNoTracking()
            .Where(a => a.ClosedAt != null)
            .OrderByDescending(a => a.Id)
            .Take(20)
            .ToListAsync();

        return View(new AlertsViewModel(thresholds, active, history));
    }

    // 폼 필드명 value[{Id}]/enabled[{Id}]에 대응(Alerts/Index.cshtml 참고) - 체크 안 된
    // 체크박스는 폼에 아예 안 실려서 enabled 딕셔너리에 그 Id가 없으면 "꺼짐"으로 처리.
    [HttpPost("thresholds")]
    public async Task<IActionResult> UpdateThresholds([FromForm] Dictionary<int, double> value, [FromForm] Dictionary<int, bool> enabled)
    {
        var thresholds = await _db.AlertThresholds.ToListAsync();

        foreach (var threshold in thresholds)
        {
            if (value.TryGetValue(threshold.Id, out var newValue))
                threshold.Value = newValue;

            threshold.Enabled = enabled.ContainsKey(threshold.Id);
        }

        await _db.SaveChangesAsync();
        return RedirectToAction(nameof(Index));
    }
}
