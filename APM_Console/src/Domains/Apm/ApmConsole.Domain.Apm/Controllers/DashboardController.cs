using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace ApmConsole.Domain.Apm.Controllers;

[Area("Apm")]
[Route("apm/dashboard")]
public class DashboardController : Controller
{
	private readonly ApmDbContext _db;

	public DashboardController(ApmDbContext db)
	{
		_db = db;
	}

	[HttpGet]
	public async Task<IActionResult> Index()
	{
		// OrderByDescending(m => m.Ts)는 SQLite가 DateTimeOffset을 ORDER BY에서
		// 직접 비교하지 못해 NotSupportedException을 던짐 - Id(auto-increment 정수,
		// 삽입 순서와 항상 일치)로 정렬하면 같은 결과를 얻으면서 이 제약을 피함.
		var recent = await _db.Metrics
			.AsNoTracking()
			.OrderByDescending(m => m.Id)
			.Take(20)
			.ToListAsync();

		return View(recent);
	}
}