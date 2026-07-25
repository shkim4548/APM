using ApmConsole.Contracts;
using ApmConsole.Domain.Apm.Hubs;
using ApmConsole.Domain.Apm.Infrastructure;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Routing;
using Microsoft.AspNetCore.SignalR;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;

namespace ApmConsole.Domain.Apm;

public class ApmModule : IDomainModule
{
	public string DomainName => "Apm";

	public void RegisterServices(IServiceCollection services, IConfiguration configuration)
	{
		var backend = configuration["Apm:StorageBackend"] ?? "SQLite";
		var connectionString = configuration["Apm:ConnectionString"]
			?? throw new InvalidOperationException("appsettings.json에 Apm:ConnectionString 설정이 필요합니다.");

		services.AddDbContext<ApmDbContext>(options =>
		{
			if (backend == "TimescaleDB")
				options.UseNpgsql(connectionString);
			else
				options.UseSqlite(connectionString);
		});

		services.AddSignalR();
		services.AddHostedService<MetricsReceiverService>();
	}

	public void MapEndpoints(IEndpointRouteBuilder endpoints)
	{
		endpoints.MapHub<MetricsHub>("/apm/hub/metrics");
	}
}