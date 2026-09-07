# APM_Console 실행 가이드

`Host`/`Contracts`/`Domain.Apm`/`Domain.Game` 플러그인 구조를 빌드하고 실행하는 방법. 설계 이유는 `WORK_STATUS.md`의 "APM_Console 계획" 섹션, 코드 전문은 `Docs/SESSION_LOG.md`의 "APM_Console 초기 스캐폴드"/"APM_Console 스캐폴드 — 직접 적용" 항목 참고.

---

## 1. Linux (WSL) 에서 실행

```bash
cd APM_Console
dotnet build ApmConsole.sln
cd src/ApmConsole.Host
dotnet run --urls "http://localhost:5299"
```
콘솔에 `[ApmConsole.Host] 도메인 로드됨: Apm` / `... Game` 두 줄이 뜨면 정상. 브라우저로 `http://localhost:5299/apm/dashboard`, `http://localhost:5299/game/dashboard` 접속해서 서로 다른 placeholder 화면이 뜨는지 확인.

이 WSL 환경엔 .NET 8 SDK가 기본 설치되어 있지 않아서(2026-07-17 확인), 없다면:
```bash
curl -sSL https://dot.net/v1/dotnet-install.sh -o dotnet-install.sh
bash dotnet-install.sh --channel 8.0 --install-dir "$HOME/.dotnet"
export PATH="$HOME/.dotnet:$PATH"
export DOTNET_ROOT="$HOME/.dotnet"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1   # libicu 미설치 시 필요(sudo 없이 우회)
```

---

## 2. Windows에서 실행 — 겪은 문제와 해결 (2026-07-17)

### ⚠️ 절대 WSL 경로(UNC 경로)에서 직접 빌드하지 말 것

`\\wsl.localhost\Ubuntu\...` 같은 경로에서 바로 `dotnet build`/`dotnet run`을 실행하면 **응답 없이 멈추는 문제**가 실제로 발생했다(1분+ 아무 출력 없음, 서버도 안 뜸 - `ERR_CONNECTION_REFUSED`로 확인).

**원인**: UNC 경로는 네트워크 프로토콜(9P)을 거치는 접근이라 로컬 드라이브보다 훨씬 느리고, MSBuild처럼 파일을 많이 건드리는 도구가 사실상 멈춘 것처럼 보일 정도로 느려질 수 있음. (UNC = Universal Naming Convention, `\\서버\공유\경로` 형식으로 네트워크 자원을 드라이브 문자 없이 가리키는 방식 - `wsl.localhost`가 서버, `Ubuntu`가 공유 이름 역할을 하며 WSL2 안의 Linux 파일시스템을 네트워크 공유처럼 접근함.)

**해결**: 반드시 Windows 로컬 드라이브로 복사한 뒤 그 경로에서 빌드/실행.

```powershell
robocopy \\wsl.localhost\Ubuntu\home\shkim\dev\gw2-cross\APM_Console C:\dev\APM_Console /E /XD bin obj
cd C:\dev\APM_Console
```
`/XD bin obj`는 WSL 쪽에서 빌드된 산출물을 제외하는 옵션(Windows에서 새로 빌드하므로 불필요). `robocopy`는 성공해도 0이 아닌 종료 코드를 낼 수 있음(정상 동작).

### ⚠️ 로컬로 복사한 뒤 `Assembly.LoadFrom`이 차단되는 문제 (Mark of the Web)

로컬 복사 후 빌드는 몇 초 만에 성공했지만, 실행 시 아래 예외가 발생했다:
```
Unhandled exception. System.IO.FileLoadException: Could not load file or assembly
'...\Modules\Game\ApmConsole.Domain.Game.dll'. 애플리케이션 제어 정책에서 이 파일을 차단했습니다. (0x800711C7)
```
(두 도메인 중 하나만 이 에러가 나고 나머지는 정상 로드되는 등 파일별로 들쭉날쭉하게 나타날 수 있음.)

**원인**: Windows는 "신뢰할 수 없는 출처(네트워크 등)"에서 온 파일에 **Mark of the Web(MOTW)** 표식(`Zone.Identifier` 대체 데이터 스트림)을 붙인다. UNC 경로에서 `robocopy`로 복사해온 파일도 이 표식이 붙고, `Assembly.LoadFrom()`으로 어셈블리를 리플렉션 로드할 때 이 표식이 확인되어 차단됨.

**해결**: 표식을 재귀적으로 제거.
```powershell
Get-ChildItem -Path C:\dev\APM_Console -Recurse | Unblock-File
```
이후 다시 실행하면 정상 동작.

### 실행
```powershell
cd C:\dev\APM_Console
dotnet build ApmConsole.sln
cd src\ApmConsole.Host
dotnet run --urls "http://localhost:5299"
```
Linux와 동일하게 콘솔 로그 2줄 + `/apm/dashboard`, `/game/dashboard` 두 화면이 다르게 뜨는지 확인.

### 참고 — 실제 배포 시에도 재발할 수 있는 문제
이번에 겪은 MOTW 차단은 "WSL→로컬 복사" 상황에서만 생기는 게 아니라, **CI/CD 아티팩트 다운로드나 네트워크 공유를 통한 배포에서도 동일하게 재발할 수 있다.** 근본적인 해결은 `Program.cs`의 로딩 방식을 `Assembly.LoadFrom(path)`(파일 경로 기반, zone 체크 대상)에서 `Assembly.Load(File.ReadAllBytes(path))`(바이트 배열 기반, zone 체크 안 거침)로 바꾸는 것 — 아직 미적용, 실제 배포 파이프라인을 설계하는 시점에 반영 검토.

### ⚠️ Smart App Control(SAC)이 실행 자체를 차단 (2026-07-22, 다른 PC/경로)

WSL 경로가 아니라 처음부터 Windows 로컬 드라이브(`D:\Dev\...\APM_Console`)에서 `dotnet build ApmConsole.sln`을 실행하면 **빌드는 0 에러로 성공**한다(관리 코드라 vcpkg 같은 네이티브 의존성 이슈가 없음 — `APM_Agent`보다 훨씬 간단). 문제는 실행 시점:

```
dotnet run --urls "http://localhost:5299"
→ Unhandled exception: An error occurred trying to start process '...\ApmConsole.Host.exe' ...
  애플리케이션 제어 정책에서 이 파일을 차단했습니다.
```

서명된 `dotnet.exe`로 managed DLL을 직접 구동(`dotnet bin\Debug\net8.0\ApmConsole.Host.dll`)해도 우회 안 됨 — 이번엔 `ApmConsole.Contracts.dll`(Host가 직접 참조하는 핵심 어셈블리)까지 차단됨:
```
System.IO.FileLoadException: Could not load file or assembly '...\ApmConsole.Contracts.dll'.
애플리케이션 제어 정책에서 이 파일을 차단했습니다. (0x800711C7)
```

Windows 이벤트 로그(`Microsoft-Windows-CodeIntegrity/Operational`, 이벤트 ID 3077/3033/3118)로 확인한 원인은 위 MOTW 절과는 **다른 종류의 차단** — **Smart App Control**이 로컬에서 새로 빌드한(서명 안 된) 어셈블리 로딩을 코드 무결성 정책 위반으로 막는 것(정책 ID `{0283ac0f-fff1-49ae-ada1-8a933130cad6}` — `APM_Agent`의 네이티브 vcpkg DLL을 막은 것과 동일 정책). 2026-07-17 당시엔 Game 도메인 플러그인 DLL 하나만 막히고 Host/Apm 도메인은 정상 로드됐지만, 이번엔 Host의 핵심 어셈블리까지 막힘 — SAC 판정이 더 엄격해졌거나 빌드 경로 차이로 추정, 원인이 완전히 규명되진 않음.

**`APM_Agent`(네이티브 C++)와 달리 뾰족한 우회책이 없다**: C++ 쪽은 vcpkg 정적 링크로 외부 DLL 의존성 자체를 없애 우회했지만, .NET은 매니지드 어셈블리 로딩 구조상 같은 방식이 통하지 않는다(코드 서명 없이 근본 해결이 어려움). **결정(2026-07-22)**: 이번엔 빌드 성공까지로 검증을 종료 — 실행/서명 문제는 시스템 보안 설정(SAC)을 끄지 않고 실제 배포 파이프라인 설계 시점(코드 서명 인증서 도입 등)에 재검토하기로 함. 이는 2026-07-17에 이미 내렸던 결정과 동일한 원칙.
