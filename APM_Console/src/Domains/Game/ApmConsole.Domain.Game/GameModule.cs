using ApmConsole.Contracts;
using Microsoft.AspNetCore.Routing;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;

namespace ApmConsole.Domain.Game;

public class GameModule : IDomainModule
{
	public string DomainName => "Game";

	public void RegisterServices(IServiceCollection services, IConfiguration configuration)
	{
		// TODO: GameDbContext(신규 스키마), ASP.NET Core Identity 등록 - 다음 단계
	}

	public void MapEndpoints(IEndpointRouteBuilder endpoints)
	{
		// TODO: GameServer(C++)가 호출할 로그인/MMR API 라우트 - 다음 단계
	}
}
