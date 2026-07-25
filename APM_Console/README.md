# APM_Console

[`APM_Agent`](../APM_Agent/README.md)(Collector)가 네트워크로 전송하는 텔레메트리를 수신·저장하고, 실시간 대시보드로 시각화하는 웹 서버. `APM_Agent`가 "무엇을 수집·암호화·전송하는가"를 다룬다면, `APM_Console`은 "그 데이터를 어떻게 받아서 보여주는가"를 다룬다.

> 현재 상태: TLS 리스너 → AES-256-GCM 복호화 → `PacketHeader` 프레이밍 파싱 → Protobuf 역직렬화 → 자체 SQLite 저장 → SignalR 실시간 푸시 → Chart.js 대시보드(그래프 5개 + 실시간 표)까지 전체 파이프라인이 end-to-end로 동작·검증 완료.

---

## 아키텍처

```
APM_Agent (Collector)                          APM_Console (ApmConsole.Host)
┌──────────────────────┐                       ┌────────────────────────────┐
│  ResilientSender      │   TLS + AES-256-GCM   │  MetricsReceiverService     │
│  (webserver_config)   │ ────────────────────▶ │  (TcpListener + SslStream)  │
└──────────────────────┘                       │        │                    │
                                                │        ▼                    │
                                                │  PacketHeader 파싱          │
                                                │  → AesGcm 복호화             │
                                                │  → Metric.Parser.ParseFrom   │
                                                │        │                    │
                                                │        ▼                    │
                                                │  ApmDbContext (SQLite)      │
                                                │  webserver_apm.db           │
                                                │        │                    │
                                                │        ▼                    │
                                                │  MetricsHub (SignalR)       │
                                                └────────┼────────────────────┘
                                                         ▼ 실시간 push
                                                ┌────────────────────────────┐
                                                │  브라우저 — /apm/dashboard   │
                                                │  Chart.js 5그래프 + 실시간 표 │
                                                └────────────────────────────┘
```

`Collector`와 `APM_Console` 사이는 "같은 장비에서 파일을 공유"하는 방식이 아니라 **순수 네트워크 관계**다 — 서로 다른 장비에 있어도 동작한다. 두 프로세스가 같은 디스크의 SQLite 파일을 각자 다른 시점에 여는 이전 방식(같은 장비 전제)에서, `Collector`가 능동적으로 전송하고 `APM_Console`이 자기 소유 DB에 저장하는 구조로 전환한 결과다.

---

## 보안 설계 — AES-256-GCM

`APM_Agent` 내부(Agent↔Collector)는 ARIA-256-CBC+HMAC(Encrypt-then-MAC)을 쓰지만, 이 구간(Collector↔APM_Console)은 **AES-256-GCM**을 쓴다. 두 세션 모두 C++ 쪽에서 `IPayloadSealer` 인터페이스(`Seal`/`Open`)로 추상화되어 있어, `ApmSession`/`ResilientSender`는 어느 암호 방식을 쓰는지 몰라도 된다 — 세션을 만드는 시점에 `SecurePayload`(ARIA+HMAC)와 `AesGcmPayload`(AES-GCM) 중 하나를 주입하기만 하면 된다.

GCM은 AEAD(Authenticated Encryption with Associated Data)라 별도 HMAC 조합이 필요 없다 — 암호화와 동시에 인증 태그가 계산되고, 복호화 시 태그 검증에 실패하면 평문이 아예 나오지 않는다(`SecurePayload`처럼 "MAC을 손으로 먼저 검증"하는 코드 자체가 없어도 됨). 와이어 포맷은 `[Nonce(12B)][ciphertext][Tag(16B)]`. .NET 쪽은 `System.Security.Cryptography.AesGcm`(.NET 8부터 태그 크기를 명시해야 함)으로 대칭 구현.

키는 `APM_Agent/certs/webserver_aes.key`(대칭키, `generate_webserver_key.sh`로 생성) **파일 하나**를 `Collector`와 `APM_Console`이 공유한다 — `APM_Console`에 별도 사본을 두지 않고 `appsettings.json`의 `WebServerAesKeyPath`가 그 경로를 절대경로로 그대로 가리킨다. TLS는 별도로 `APM_Console` 자체의 자체 서명 인증서(`APM_Console/certs/webserver.crt`/`.key`, `generate_webserver_cert.sh`)를 쓴다(전송 계층 보호와 페이로드 암호화를 분리하는 구조는 `APM_Agent`와 동일한 원칙).

---

## 수신 프로토콜 — 스키마 공유

`APM_Agent`가 정의한 `Metric.proto`(`apm::Metric`)를 그대로 참조한다(복사 아님) — `Grpc.Tools`+`Google.Protobuf` NuGet 패키지로 C#/C++ 양쪽이 같은 `.proto` 파일에서 코드젠하기 때문에, 스키마가 이원화되어 갈라질 위험이 없다. 실제 gRPC 서비스는 쓰지 않고(`GrpcServices="None"`) Protobuf 메시지 직렬화만 재사용한다.

프레이밍(`PacketHeader{uint16 size, uint16 id}`)도 C++ 쪽과 동일한 와이어 포맷을 수동으로 맞춰 파싱한다 — TCP는 스트림 프로토콜이라 메시지 경계가 없으므로, 부분 수신(partial read)까지 처리하는 루프(`ReadExactAsync`)로 정확히 `header.size`만큼 모일 때까지 누적한다.

---

## 실시간 갱신 — SignalR

메트릭을 저장한 직후 곧바로 SignalR(`MetricsHub`, `/apm/hub/metrics`)로 브로드캐스트한다(폴링 없음 — 저장과 푸시가 한 번의 흐름). 대시보드의 JS 클라이언트가 `NewMetric` 이벤트를 구독해 표와 그래프를 동시에 갱신한다.

---

## 대시보드

`/apm/dashboard`에 카드 레이아웃(다크모드 지원)으로 그래프 5개(CPU/메모리/디스크 사용률, 네트워크 RX·TX, TCP RTT+RTT 분산)와 최근 메트릭 표(시각/CPU/메모리/디스크/Net RX·TX/RTT/RTT 분산/재전송/총 재전송/cwnd, 11개 컬럼)를 렌더링한다. 차트는 Chart.js — 페이지 로드 시 최근 20건으로 초기화하고, 이후 SignalR 이벤트로 계속 갱신(최대 20개 유지, 오래된 점은 밀려남).

---

## 플러그인 아키텍처

`APM_Console`은 단일 프로젝트가 아니라 **도메인별 별도 프로젝트 + 런타임 플러그인 로딩** 구조다:

```
APM_Console/
├── ApmConsole.sln
├── src/
│   ├── ApmConsole.Host/            ← 실행 진입점, appsettings.json의 EnabledDomains로 도메인 온/오프
│   ├── ApmConsole.Contracts/       ← IDomainModule 인터페이스만(의존성 최소, Host/모든 도메인이 참조)
│   └── Domains/
│       ├── Apm/ApmConsole.Domain.Apm/     (Sdk="Microsoft.NET.Sdk.Razor")
│       └── Game/ApmConsole.Domain.Game/   (Sdk="Microsoft.NET.Sdk.Razor", 현재 휴면 — 아래 참고)
```

- **로딩 방식**: `Assembly.LoadFrom()`이 아니라 `AssemblyDependencyResolver` 기반의 커스텀 `AssemblyLoadContext`(`DomainLoadContext`)를 쓴다. 도메인 DLL 옆의 `.deps.json`을 읽어 관리 어셈블리·네이티브 라이브러리(예: SQLite의 `.so`)를 전부 그 도메인 폴더 기준으로 정확히 찾아준다 — Microsoft 공식 플러그인 패턴. 호스트가 이미 로드한 공유 타입(`IDomainModule`, `IServiceCollection` 등)은 별도로 다시 로드하지 않고 재사용해서, 같은 이름의 타입이 ALC가 다르다는 이유로 별개 타입 취급되는(플러그인 아키텍처의 잘 알려진 함정) 문제를 피한다.
- **정적 파일**: 도메인 DLL에 `wwwroot`를 임베디드 리소스로 내장(`ManifestEmbeddedFileProvider`), Host의 물리 `wwwroot`와 `CompositeFileProvider`로 합쳐서 서빙 — DLL 하나로 완결되는 "진짜 플러그인" 형태.
- **배포**: 도메인마다 빌드 후 `Host/bin/.../Modules/<도메인명>/`으로 산출물을 복사(MSBuild `<Copy>` 태스크, OS 무관). `appsettings.json`의 `EnabledDomains`만 바꾸면 재빌드 없이 도메인을 켜고 끌 수 있다.

**Game 도메인은 왜 있는가**: `ApmConsole.Domain.Game`은 게임 로그인/MMR(TrueSkill) 기능을 위해 스캐폴딩된 도메인이다. 현재는 설계만 확정되고 구현은 보류된 **휴면 상태**로, 이 플러그인 구조가 실제로 "여러 독립된 기능 모듈을 하나의 호스트가 온/오프 가능하게 관리"하는 아키텍처임을 보여주는 용도로 남겨뒀다. `EnabledDomains`에서 `"Game"`을 빼면 완전히 비활성화된다.

---

## 기술 스택
- .NET 8 / ASP.NET Core MVC + Razor Views(서버사이드 렌더링)
- Entity Framework Core — SQLite(기본) 또는 Npgsql(TimescaleDB)를 `appsettings.json`의 `Apm:StorageBackend`로 런타임 선택(`APM_Agent`의 컴파일 타임 백엔드 선택과 같은 설계 원칙, .NET은 두 프로바이더 다 미리 참조해둬도 빌드 불가 문제가 없어 런타임 선택으로 단순화)
- SignalR (실시간 서버→클라이언트 푸시)
- Grpc.Tools + Google.Protobuf (Protobuf 메시지 코드젠 — gRPC 서비스는 미사용)
- Chart.js (실시간 그래프)
- `AssemblyDependencyResolver` 기반 커스텀 플러그인 로딩

---

## 빌드/실행

```bash
cd APM_Console
bash certs/generate_webserver_cert.sh   # 최초 1회: TLS용 자체 서명 인증서
# APM_Agent/certs/generate_webserver_key.sh 로 생성한 webserver_aes.key를
# Collector와 공유해야 함(appsettings.json의 WebServerAesKeyPath가 그 경로를 가리킴)

dotnet build ApmConsole.sln
dotnet run --project src/ApmConsole.Host/ApmConsole.Host.csproj
```

`appsettings.json`(`ApmConsole.Host/`)에서 리시버 포트(기본 9100)·키/인증서 경로·DB 연결 문자열을 설정한다. `Collector`가 실제로 데이터를 보내야 대시보드에 값이 채워진다 — 실행 순서와 상세 시나리오는 `Docs/WEBSERVER_TEST_SCENARIO.md` 참고.

---

## 테스트

`tests/ApmConsole.Domain.Apm.Tests/`(xUnit) — `MetricsReceiverService`의 핵심 로직 2곳을 검증한다:

- `ReadExactAsyncTests.cs`(4개): 한 번에 전부 수신 / 여러 조각으로 분할 수신(커스텀 `ChunkedStream`으로 TCP의 부분 수신을 재현) / 요청 길이 전에 스트림이 끊기면 `null` / 길이 0이면 빈 배열.
- `DecryptAndParseTests.cs`(4개): 정상 왕복(필드값 복원) / 태그 변조·암호문 변조·잘못된 키 전부 `AuthenticationTagMismatchException`으로 거부.

총 8개, 전부 통과 확인(2026-07-20). 테스트 대상 메서드는 `private`이 아니라 `internal`로 열어두고 `AssemblyInfo.cs`의 `InternalsVisibleTo("ApmConsole.Domain.Apm.Tests")`로 테스트 프로젝트에만 노출한다(공개 API 표면은 그대로 유지). `DecryptAndParse`는 인스턴스 필드 대신 키를 파라미터로 받는 `static` 메서드라 DI 없이 순수 로직만 테스트한다.

```bash
dotnet test tests/ApmConsole.Domain.Apm.Tests/ApmConsole.Domain.Apm.Tests.csproj
```

C++ 쪽(`APM_Agent`, AES-GCM 암복호화 9개)은 [`APM_Agent/README.md`의 "테스트" 섹션](../APM_Agent/README.md#테스트) 참고 — 두 프로젝트 합쳐 총 17개 테스트가 전부 통과한다.

---

## 알려진 제한사항
- `Metrics` 테이블에 보관 기간/정리(retention) 정책이 없다 — 장시간 켜두면 계속 커진다(포트폴리오 데모 범위에서는 문제 없음).
- `Apm:StorageBackend=TimescaleDB` 경로는 코드는 있으나 `APM_Console` 쪽에서 실제 실행 검증은 안 됨(로컬에 TimescaleDB 확장 미설치) — SQLite 경로만 실측 검증됨.
- Windows에서 도메인 DLL을 배포할 경우 Smart App Control(서명 검증)에 막힐 수 있음 — 실제 배포 설계 시점에 다룰 과제로 별도 기록됨(`WORK_STATUS.md` 참고).
- 위 테스트는 핵심 지점(암호화 왕복/프레이밍) 위주이고 전체 커버리지를 목표로 하지 않는다 — `MetricsReceiverService`의 TCP 리스너 루프 자체, 대시보드 컨트롤러/뷰, SignalR 브로드캐스트 등은 여전히 수동 실행+로그 검증으로 대체.
