using Microsoft.AspNetCore.Mvc;

namespace ApmConsole.Domain.Game.Controllers;

[Area("Game")]
[Route("game/dashboard")]
public class DashboardController : Controller
{
	[HttpGet]
	public IActionResult Index()
	{
		return View();
	}
}
