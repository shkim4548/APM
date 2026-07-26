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
using System.Text.Json.Serialization;

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

		// AlertRecord/AlertThreshold의 MetricType enum을 JSON에서 정수가 아니라 이름
		// 문자열로 내려줌 - JS 쪽에서 지표별 한글 라벨을 매핑할 때 enum 값 순서(0,1,2,3)에
		// 의존하지 않고 이름으로 매핑할 수 있게(2026-07-26, 알림 기능 추가).
		services.AddSignalR()
			.AddJsonProtocol(options =>
				options.PayloadSerializerOptions.Converters.Add(new JsonStringEnumConverter()));
		services.AddHostedService<MetricsReceiverService>();
		services.AddHostedService<RetentionService>();
	}

	public void MapEndpoints(IEndpointRouteBuilder endpoints)
	{
		endpoints.MapHub<MetricsHub>("/apm/hub/metrics");
	}
}