using System.Reflection;
using System.Runtime.Loader;
using ApmConsole.Contracts;
using Microsoft.Extensions.FileProviders;

var builder = WebApplication.CreateBuilder(args);

var enabledDomains = builder.Configuration.GetSection("EnabledDomains").Get<string[]>() ?? Array.Empty<string>();
var modulesRoot = Path.Combine(AppContext.BaseDirectory, "Modules");

var mvcBuilder = builder.Services.AddControllersWithViews();
var loadedModules = new List<IDomainModule>();
// 도메인 DLL에 내장된 정적 파일(wwwroot)을 서빙하기 위한 파일 프로바이더 모음 -
// 도메인마다 있을 수도, 없을 수도 있음(임베디드 매니페스트 자체가 없는 도메인은 건너뜀).
var domainStaticFileProviders = new List<IFileProvider>();

foreach (var domain in enabledDomains)
{
    var dllPath = Path.Combine(modulesRoot, domain, $"ApmConsole.Domain.{domain}.dll");
    if (!File.Exists(dllPath))
        throw new InvalidOperationException($"'{domain}' 도메인이 활성화됐지만 DLL을 찾을 수 없습니다: {dllPath}");

    var loadContext = new DomainLoadContext(dllPath);
    var assembly = loadContext.LoadFromAssemblyPath(dllPath);
    mvcBuilder.AddApplicationPart(assembly);

    try
    {
        domainStaticFileProviders.Add(new ManifestEmbeddedFileProvider(assembly, "wwwroot"));
    }
    catch (InvalidOperationException)
    {
        // 이 도메인 어셈블리엔 임베디드 wwwroot 매니페스트가 없음(정적 자산이 없는 도메인) - 정상.
    }

    var moduleType = assembly.GetTypes()
        .SingleOrDefault(t => typeof(IDomainModule).IsAssignableFrom(t) && !t.IsAbstract && !t.IsInterface);

    if (moduleType is null)
        throw new InvalidOperationException($"'{domain}' 도메인 어셈블리에서 IDomainModule 구현체를 찾을 수 없습니다: {dllPath}");

    var module = (IDomainModule)Activator.CreateInstance(moduleType)!;
    module.RegisterServices(builder.Services, builder.Configuration);
    loadedModules.Add(module);

    Console.WriteLine($"[ApmConsole.Host] 도메인 로드됨: {module.DomainName}");
}

var app = builder.Build();

// Host 자체의 물리 wwwroot + 각 도메인의 임베디드 wwwroot를 하나로 합쳐서 서빙 -
// 이러면 도메인 DLL 안의 정적 파일도 물리 파일과 똑같은 상대 경로(/lib/...)로 접근 가능.
var compositeProvider = new CompositeFileProvider(
    new IFileProvider[] { app.Environment.WebRootFileProvider }.Concat(domainStaticFileProviders));
app.UseStaticFiles(new StaticFileOptions { FileProvider = compositeProvider });

app.UseRouting();
app.MapControllers();

foreach (var module in loadedModules)
    module.MapEndpoints(app);

app.Run();

// 도메인 DLL 옆의 .deps.json을 읽어 관리 어셈블리/네이티브 라이브러리를
// 전부 그 도메인 폴더 기준으로 정확히 찾아주는 로드 컨텍스트.
// Microsoft 공식 플러그인 패턴(AssemblyDependencyResolver) 그대로 적용.
internal class DomainLoadContext : AssemblyLoadContext
{
    private readonly AssemblyDependencyResolver _resolver;

    public DomainLoadContext(string pluginPath)
    {
        _resolver = new AssemblyDependencyResolver(pluginPath);
    }

    protected override Assembly? Load(AssemblyName assemblyName)
    {
        // 호스트(Default ALC)가 이미 로드해 둔 어셈블리는 그대로 재사용 -
        // IDomainModule/IServiceCollection처럼 호스트와 도메인이 공유하는 계약 타입의
        // 아이덴티티가 갈라지는 것을 막기 위함(같은 이름이라도 ALC가 다르면 다른 타입 취급됨).
        // EF Core/Npgsql/SQLite처럼 호스트가 모르는 도메인 전용 패키지는 여기 안 걸리고
        // 아래 resolver를 통해 도메인 폴더에서 정상적으로 로드됨.
        var existing = AssemblyLoadContext.Default.Assemblies
            .FirstOrDefault(a => a.GetName().Name == assemblyName.Name);
        if (existing != null)
            return existing;

        var assemblyPath = _resolver.ResolveAssemblyToPath(assemblyName);
        return assemblyPath != null ? LoadFromAssemblyPath(assemblyPath) : null;
    }

    protected override IntPtr LoadUnmanagedDll(string unmanagedDllName)
    {
        var libraryPath = _resolver.ResolveUnmanagedDllToPath(unmanagedDllName);
        return libraryPath != null ? LoadUnmanagedDllFromPath(libraryPath) : IntPtr.Zero;
    }
}