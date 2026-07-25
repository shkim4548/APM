using Microsoft.AspNetCore.Routing;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;

namespace ApmConsole.Contracts;

/*----------------
	IDomainModule
------------------*/
// Host가 런타임에 로드한 도메인 어셈블리에서 리플렉션으로 찾아 호출하는 진입점.
// Host는 이 인터페이스만 알고, 구체 타입(ApmModule/GameModule)은 전혀 모른다 -
// 컴파일 타임 참조가 없어야 "도메인 DLL 유무만으로 켜고 끈다"는 설계가 성립함.

public interface IDomainModule
{
	string DomainName { get; }

	void RegisterServices(IServiceCollection services, IConfiguration configuration);

	void MapEndpoints(IEndpointRouteBuilder endpoints);
}
