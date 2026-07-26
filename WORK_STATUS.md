# APM — Work Status

> 여러 환경(Windows PC / WSL)에서 작업 이어받기용 인수인계 문서.
> 작업 시작 전 반드시 이 파일을 확인하고, 완료/중단 시 업데이트할 것.
> 상세 설계안·코드 전문은 `SESSION_LOG.md` 참고 — 이 파일은 요약만 남긴다.

---

## 저장소 성격 (2026-07-26)

이 저장소(`https://github.com/shkim4548/APM`, private)는 원래 모노레포 `/home/shkim/dev/gw2-cross`에서 포트폴리오 공개 목적으로 `GW2_CrossPlatformCore`/`APM_Agent`/`APM_Console`/`Docs`만 추출해 새로 만든 것이다. 완성 전까지 private 유지, 이후 기능 추가는 이 저장소를 기준으로 진행한다. 원 모노레포의 `WORK_STATUS.md`/`SESSION_LOG.md` 관행(상태 요약 + 코드 전문 로그 분리)을 그대로 이어받는다.

**아키텍처 제약 (착수 전 필독)**:
- 패킷 프레이밍: `PacketHeader{size,id}`, ID는 `PacketType::descriptor()->index()` — `.proto` 파일 내 선언 순서 기준. 새 메시지는 반드시 기존 메시지와 같은 `.proto` 파일에 이어 붙일 것(순서 바뀌면 ID 충돌).
- `PacketHandler::Register<T>()`/`Dispatch()` — 정적 ID→핸들러 테이블. `ResilientSender::Enqueue<T>()`/`EnqueueRaw()` — 재연결 안전 큐잉 전송기. `ApmSession::SendPacket<T>()`/`Send()` — 저수준 전송.
- Agent 쪽에 미사용 수신 경로, Collector 쪽에 세션 레지스트리 부재 — 원격 명령 실행(아래 로드맵 7순위, 현재 보류) 착수 시 채워야 함.
- `Metric.pb.h`/`.pb.cc`는 Linux protoc 생성본이 커밋되어 있음. 새 메시지 타입 추가 시 Linux에서 재생성 후 커밋 필수(Windows vcpkg protobuf 버전 다름 — 재생성 금지).
- 저장소 백엔드는 컴파일 타임 선택(`APM_STORAGE_BACKEND=SQLite|TimescaleDB`). 현재 SQLite 스키마: `metrics` 단일 테이블(`SqliteMetricStore.cpp`).

---

## 로드맵 (우선순위 순, 2026-07-26 확정)

```
1순위 : 부하/스케일 테스트 툴                         ⏸️ 구현 완료, 실측 보류
2순위 : 알림(alerting, 임계치 기반)                    ✅ 코드 적용 + 빌드/테스트 검증 완료
3순위 : 데이터 보존 정책(retention)                    ✅ 코드 적용 + 빌드 검증 완료
4순위 : 함수/트랜잭션 레벨 계측                         ✅ 코드 적용 + protoc 재생성 + 빌드/테스트 검증 완료
5순위 : 백분위/집계 통계                                ⬜ 미착수
6순위 : OpenTelemetry — 구현 보류, 면접용 답변 정리만  ⬜ 미착수
7순위 : 원격 명령 실행 기능                            ⏸️ 보류(사유 아래 참고), 착수 여부 미정
```

---

## 작업 목록

### 1순위 — 부하/스케일 테스트 툴 ⏸️ 구현 완료, 실측 보류

| # | 작업 | 상태 | 메모 |
|---|---|---|---|
| 1-1 | 기존 코드 구조 파악 | ✅ 완료 (2026-07-26) | `Agent`/`Collector` `main.cpp`, `ApmSession`, `ResilientSender`, `PacketHandler` 확인. **발견**: `Collector`가 `ioContext.run()`을 메인 스레드에서 단일 호출(단일 스레드 io_context) — 접속 수가 늘어도 복호화/파싱/SQLite 저장은 한 스레드에서 순차 처리. 스케일 병목의 1차 가설. |
| 1-2 | LoadTester 아키텍처 설계 제안 | ✅ 완료 (2026-07-26) | in-process asio 다중 연결 시뮬레이터(별도 프로세스 N개 fork 대신), 기존 `ResilientSender`/`AesGcmPayload`/`apm::Metric` 재사용. 상세: `SESSION_LOG.md` 2026-07-26 항목 |
| 1-3 | 코드 스켈레톤(멤버 변수/함수 시그니처 전체) 제시 | ✅ 완료 (2026-07-26) | `LoadTester/` 신설안 + `ResilientSender` 콜백 추가안 제시. 상세: `SESSION_LOG.md` 2026-07-26 두 번째 항목. **사용자 확인 필요 4건 → 전부 추천안대로 확정 (2026-07-26)** |
| 1-4 | 구현 + 빌드 검증 | ✅ 완료 (2026-07-26) | `ResilientSender.h/.cpp` 수정 적용(선택적 `SendCallback`/`ConnectionStateCallback` 추가, `Agent/main.cpp`는 기본값 `nullptr`라 무변경). `APM_Agent/LoadTester/` 6개 파일 신규 작성 + `CMakeLists.txt`에 `LoadTester` 타겟 추가. **사용자가 WSL(Ubuntu, GNU 13.3.0)에서 `cmake --build build --target LoadTester` 빌드 성공 확인**(경고 없음 — `GW2_CrossPlatformCore`의 기존 `ASIO_STANDALONE` 재정의 경고만 있고 이번 변경과 무관). 코드 전문은 `SESSION_LOG.md` 2026-07-26 세 번째 항목 참고. |
| 1-5 | 실측 (N-agent 스케일 syscall/RSS/CPU/처리량/지연) | ⏸️ 보류 (2026-07-26) | 스크립트(`run_load_test.sh`) 완성 + 검증 완료(버그 2건 수정: `set -e`/`pipefail` 조기 종료, `strace` 미설치/키 미생성 진단 메시지 보강). 매트릭스 6단계 중 **1번(agents=1)만 실행 완료**(`sent=593/60s`, 드롭·재연결·실패 0 — 파이프라인 정상 확인). 나머지 5단계(10/50/100/100+ramp-up/300 에이전트)는 사용자 판단으로 보류, 재개 시 `APM_Agent/`에서 아래 명령만 다시 실행하면 됨(키/빌드/strace 세팅 이미 완료된 상태라 재개 비용 낮음): `bash run_load_test.sh 10 60 100 0`, `50 60 100 0`, `100 60 100 0`, `100 60 100 5000`, `300 60 100 5000`. 상세: `SESSION_LOG.md` 2026-07-26 네 번째 항목 |
| 1-6 | README/`Docs/PROJECT_TECHNICAL_REVIEW.md`에 결과 반영 | 대기 | |

### 2순위 — 알림(임계치 기반) ✅ 코드 적용 + 빌드/테스트 검증 완료

`APM_Console` 쪽에서만 닫히는 작업(Agent/Collector 변경 불필요). 확인 4건 확정: 대상 지표 CPU/메모리/디스크/TCP RTT, 임계치는 DB 저장+UI 편집, 상태 전이 시만 알림(OK→Alert→Resolved, Zabbix/Nagios/Alertmanager 방식), DB에 이력 영속화(`AlertRecord`). 설계 상세: `SESSION_LOG.md` 2026-07-26 다섯 번째 항목("2순위(알림) 설계 제안").

**2026-07-26 적용 완료** — 사용자가 "2순위 알림 기능 코드 적용"으로 명시 확인, 아래 12개 파일 실제 반영:
- 신규 7개: `Infrastructure/Persistence/AlertThreshold.cs`, `Infrastructure/Persistence/AlertRecord.cs`, `Infrastructure/AlertEvaluator.cs`, `tests/.../Infrastructure/AlertEvaluatorTests.cs`, `Models/AlertsViewModel.cs`, `Controllers/AlertsController.cs`, `Areas/Apm/Views/Alerts/Index.cshtml`
- 수정 5개: `Infrastructure/Persistence/ApmDbContext.cs`(DbSet 2개 + 시드 데이터), `Infrastructure/MetricsReceiverService.cs`(`EvaluateAlertsAsync` 추가), `ApmModule.cs`(`JsonStringEnumConverter` 추가), `Areas/Apm/Views/Dashboard/Index.cshtml`(알림 페이지 링크), `ApmConsole.Host/wwwroot/css/site.css`(`.alert-row-active` 스타일)

**검증 완료(사용자, WSL)**: `dotnet test` 결과 `Passed: 13, Failed: 0`(기존 8건 + 신규 `AlertEvaluatorTests` 5건) — 빌드/단위 테스트 통과 확인.

**다음 할 일**: `/apm/alerts` 페이지를 실제로 띄워 임계치 편집 저장(`POST /apm/alerts/thresholds`)과 SignalR 실시간 알림(`AlertOpened`/`AlertResolved`)이 브라우저에서 의도대로 동작하는지 수동 확인하면 2순위 완전 종료. **(2026-07-26 확인: 아직 미실행)** — 빌드/단위 테스트만 검증됐고 브라우저 시각 검증은 보류 중.

### 3순위 — 데이터 보존 정책(retention) ✅ 코드 적용 + 빌드 검증 완료

저장소가 두 군데(Collector 로컬 `IMetricStore`/Console `ApmDbContext`)라 양쪽 다 대상. 확인 4건 확정: 적용 범위 Console+Collector 둘 다, 시간 기준 정책, Metrics 기본 30일, `AlertRecord`는 Metrics보다 길게(180일). 백엔드별로 구현 방식이 다름 — TimescaleDB는 하이퍼테이블 네이티브 `add_retention_policy()`(청크째로 드롭), SQLite는 직접 `DELETE` + `PRAGMA incremental_vacuum`, Console(EF Core) 쪽은 백엔드 무관하게 `ExecuteDeleteAsync` 하나로 통일. 설계 상세: `SESSION_LOG.md` 2026-07-26 여섯 번째 항목("3순위(데이터 보존 정책) 설계 제안").

**2026-07-26 적용 완료** — 사용자가 "바로 적용해줘"로 명시 확인, 아래 14개 파일 실제 반영:
- Console 신규 1개: `Infrastructure/RetentionService.cs`(매시간 `Metrics`/`AlertRecords` 정리)
- Console 수정 3개: `Infrastructure/Persistence/ApmDbContext.cs`(`MetricRecord.Ts`/`AlertRecord.ClosedAt` 인덱스), `ApmModule.cs`(`RetentionService` 등록), `appsettings.json`(`MetricsRetentionDays`/`AlertRetentionDays`)
- Collector 수정 10개(파일 기준): `Storage/IMetricStore.h`(`Prune()` 추가), `Storage/SqliteMetricStore.{h,cpp}`(`Prune()` 구현 + `auto_vacuum=INCREMENTAL`), `Storage/TimescaleMetricStore.{h,cpp}`(생성자에서 `add_retention_policy()` 등록, `Prune()`은 no-op), `Storage/MetricStoreFactory.{h,cpp}`(`retentionDays` 인자 추가), `Collector/CollectorConfig.{h,cpp}`(`metricsRetentionDays` 필드, `metrics_retention_days` JSON 키), `Collector/main.cpp`(`pruneTimer` 24시간 간격 신설)

**검증 완료(사용자, WSL, 2026-07-26)**:
- Console: `dotnet build` → `Build succeeded, 0 Warning(s), 0 Error(s)`.
- Collector: `cmake --build build` → `APM_Storage`/`Collector`/`Agent`/`LoadTester`/`APM_Common_Tests` 전부 빌드 성공. 컴파일된 오브젝트가 `SqliteMetricStore.cpp.o`뿐인 것으로 보아 현재 `APM_STORAGE_BACKEND=SQLite`로 빌드됨 — `TimescaleMetricStore.cpp`(네이티브 `add_retention_policy()` 경로)는 이번엔 컴파일 대상에 포함 안 됨, TimescaleDB 백엔드 전환 시 별도 컴파일 확인 필요.
- (참고: `APM_Agent`에서 `dotnet build` 실행 시 `MSB1003` 에러가 났던 건 정상 — `APM_Agent`는 C++/CMake 프로젝트라 `.sln`/`.csproj`가 없음, `dotnet build`가 아니라 `cmake --build`가 맞는 명령.)

**남은 선택 사항(코드/빌드 관점에선 3순위 완료, 실행 관점 확인은 선택)**: Collector를 실제로 띄워 24시간 대기 없이 즉시 확인하려면 `pruneTimer` 간격을 임시로 줄여서 `[SqliteMetricStore] prune 완료` 로그가 찍히는지 보는 정도 — 필수는 아님(설계상 `DELETE ... WHERE ts < now - N일` 로직 자체는 단순해 런타임 리스크가 낮다고 판단).

**부수 발견(이번 작업 범위 밖)**: `APM_Console/src/ApmConsole.Host/appsettings.json`의 `ConnectionString`/키·인증서 경로가 옛 모노레포 경로(`/home/shkim/dev/gw2-cross/...`)로 남아있음 — 저장소 이관(2026-07-26) 이후 갱신 안 된 것으로 보임. 실행 시 문제되면 별도로 손봐야 함.

### 4순위 — 함수/트랜잭션 레벨 계측 ✅ 코드 적용 + protoc 재생성 + 빌드/테스트 검증 완료

"시스템 리소스 모니터링"과 "APM"의 정체성 갭을 메우는 항목. 확인 4건 확정: 계측 대상은 이 프로젝트 자체 코드(별도 데모 앱 없이 Collector/Console에 직접 삽입), 데이터 모델은 개별 span 원본 저장(5순위 백분위 정확도용), API 형태는 RAII 스코프 기반(C++ 소멸자/.NET `IAsyncDisposable`), 대상 언어는 C+++.NET 둘 다.

**설계 핵심**: `Metric.proto`에 `TransactionSpan` 메시지 추가(기존 메시지 뒤에 이어 붙임). Collector(C++) span은 기존 `Metric` 전송 파이프라인(`ResilientSender`/`ApmSession`)을 그대로 재사용해 네트워크로 전송(새 포트 불필요) — `ScopedSpan`(RAII) + `SpanRecorder`(전역 큐) 신설, `Collector.HandleMetricPacket`(메트릭 패킷 처리 핸들러)에 데모 계측. Console(.NET) span은 이미 자기 DB를 갖고 있어 네트워크 없이 `TraceScope`(`IAsyncDisposable`)가 직접 `ApmDbContext`에 저장 — `AlertsController.Index()`에 데모 계측. 패킷 ID 분기(`Metric.Descriptor.Index`/`TransactionSpan.Descriptor.Index`)가 새로 필요해져 `MetricsReceiverService`가 "id 무시하고 무조건 Metric으로 파싱"하던 걸 실제 분기하도록 바뀜. 새 테이블 `TransactionSpans`는 `RetentionService`가 `Metrics`와 같은 보존 기간으로 같이 정리하도록 확장.

상세 설계: `SESSION_LOG.md` 2026-07-26 일곱 번째 항목("4순위(함수/트랜잭션 레벨 계측) 설계 제안").

**2026-07-26 적용 완료** — 사용자가 "적용해줘"로 명시 확인, 아래 13개 파일 실제 반영:
- C++ 신규 4개: `Common/SpanRecorder.h`/`.cpp`(전역 span 큐), `Common/ScopedSpan.h`/`.cpp`(RAII 계측 + `APM_TRACE_SCOPE` 매크로)
- C++ 수정 3개: `Protocol/Metric.proto`(`TransactionSpan` 메시지 추가), `Common/CMakeLists.txt`(신규 소스 2개 추가), `Collector/main.cpp`(`Collector.HandleMetricPacket` 계측 + `flushToWebServer`가 span도 같이 전송)
- C# 신규 2개: `Infrastructure/Persistence/TransactionSpanRecord.cs`, `Infrastructure/TraceScope.cs`
- C# 수정 4개: `ApmDbContext.cs`(`TransactionSpans` DbSet + 복합 인덱스), `MetricsReceiverService.cs`(패킷 id로 `Metric`/`TransactionSpan` 분기, `DecryptAndParse`를 `Unseal()` 공유 방식으로 분리), `AlertsController.cs`(`Index()`에 데모 계측), `RetentionService.cs`(`TransactionSpans`도 같이 정리)

**적용 중 발견해 그 자리에서 고친 버그 1건**: `AlertsController.cs`에 `TraceScope`(네임스페이스 `ApmConsole.Domain.Apm.Infrastructure`) 참조를 추가했는데 `using` 선언이 설계안에서 빠져 있었음(컴파일 에러 유발했을 것) — 적용 직후 발견해 `using ApmConsole.Domain.Apm.Infrastructure;` 추가로 수정 완료.

**검증 완료(사용자, WSL, 2026-07-26)**:
- protoc 재생성 완료(`APM_Agent/Protocol`에서 `protoc --cpp_out=. Metric.proto`) 후 커밋 대상 확보.
- Console: `dotnet test` → `Passed: 13, Failed: 0`(4순위에서 새 테스트를 추가하지 않았으므로 기존 13건 그대로 회귀 없음 확인).
- Collector: `cmake --build build` → 로그에 `Common/CMakeFiles/APM_Common.dir/SpanRecorder.cpp.o`, `ScopedSpan.cpp.o`, 그리고 **재생성된 `__/Protocol/Metric.pb.cc.o`가 다시 컴파일**된 것으로 확인 — `apm::TransactionSpan` 참조가 정상적으로 해석됨. `Collector`/`Agent`/`LoadTester`/`APM_Common_Tests` 전부 빌드 성공.

**남은 선택 사항(코드/빌드 관점에선 4순위 완료, 실행 관점 확인은 선택)**: Collector+Console을 실제로 함께 띄워 `[MetricsReceiverService] span 저장: Collector.HandleMetricPacket ...` / `AlertsController.Index` 관련 span이 `TransactionSpans` 테이블에 실제로 쌓이는지 눈으로 확인하는 것 — 필수는 아님(패킷 id 분기 로직 자체는 단순하고, `Metric` 경로는 이미 동작 검증된 것과 동일한 프레이밍이라 리스크가 낮다고 판단).

**아직 안 한 것**: `git commit`(재생성된 `Metric.pb.h`/`.pb.cc` 포함) — 사용자가 직접 커밋 여부/시점 결정.

### 5순위 — 백분위/집계 통계 ⬜ 미착수

### 6순위 — OpenTelemetry ⬜ 미착수

**구현하지 않음** — "왜 자체 프로토콜을 만들었는가"에 대한 면접용 답변 포인트만 정리.

### 7순위 — 원격 명령 실행 ⏸️ 보류 (2026-07-26)

**보류 사유**: 원래 전 직장에서 이 기능을 접하고 APM에 필수 기능이라 생각해 로드맵에 넣었으나, 실제로는 업계 필수 기능이 아님을 확인함(Zabbix "Remote commands"/Nagios "Event Handler" 같은 인프라 모니터링 계열엔 있지만, Datadog/Dynatrace/New Relic 같은 상용 APM 계열은 관찰과 조치를 분리해 원격 명령을 에이전트에 직접 두지 않는 경향 — 공급망 공격 벡터 우려 때문). 전 직장 기능과 유사하게 구현할 경우의 잠재적 마찰 가능성을 고려해 **최후순위로 보류, 착수 여부 자체를 추후 재검토**하기로 함. 착수하게 되면 Agent 미사용 수신 경로/Collector 세션 레지스트리 부재부터 채워야 함(위 "아키텍처 제약" 참고).
