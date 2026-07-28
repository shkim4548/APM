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
1-7(신규) : Collector Store() 블로킹 개선                ✅ WorkerQueue + WAL 적용 + 빌드/재실측 검증 완료(2026-07-28) — 100 이하 하드 리밋 해소, fdatasync -97%
1-7-e(신규) : 콘솔 로깅 병목 개선(워커 스레드 위임)      ✅ 코드 적용 + 빌드/재실측 검증 완료(2026-07-29) — 300-agent 전원(300/300) 접속 성공, 단 futex 경합 신규 발견(트레이드오프)
1-7-f(신규) : 1-7-e 검증 중 발견한 cout 동시쓰기 레이스 수정 ✅ 수정 + 빌드/재실측 검증 완료(2026-07-29) — 100-agent 기준 WAL+로깅 개선은 순효과 거의 없음/근소 손해로 결론
1순위 : 부하/스케일 테스트 툴                         ✅ 실측 + 문서화 완료
2순위 : 알림(alerting, 임계치 기반)                    ✅ 코드 적용 + 빌드/테스트 검증 완료
3순위 : 데이터 보존 정책(retention)                    ✅ 코드 적용 + 빌드 검증 완료
4순위 : 함수/트랜잭션 레벨 계측                         ✅ 코드 적용 + protoc 재생성 + 빌드/테스트 검증 완료
5순위 : 백분위/집계 통계                                ✅ 코드 적용 + 빌드/테스트 검증 완료
6순위 : OpenTelemetry — 구현 보류, 면접용 답변 정리만  ⬜ 미착수
7순위 : 원격 명령 실행 기능                            ⏸️ 보류(사유 아래 참고), 착수 여부 미정
```

---

## 작업 목록

### 1-7(신규) — Collector `SqliteMetricStore::Store()` 블로킹 개선 🔴 커밋된 버전이 크래시함 — WorkerQueue로 재설계 중, **세션 중단(2026-07-27)**

**⚠️ 다음 세션 시작 시 가장 먼저 확인할 것**: 현재 git에 커밋된 `Collector/main.cpp`(`39a3772` "Move Collector metric storage off the network thread via JobQueue")는 **실제 동시 접속 상황에서 시작 후 약 10초 만에 크래시하는 버그가 있는 버전**이다. 아래 "1-7-b" 절의 `WorkerQueue` 설계(이미 `SESSION_LOG.md`에 코드 전문 작성 완료, 아직 미적용)를 이어서 적용하는 게 최우선 작업.

**배경**: 1-5 실측에서 발견한 핵심 병목 — `Store()`가 SQLite 기본 롤백 저널 모드로 매 INSERT마다 동기 `fdatasync`를 호출하는데, 이게 네트워크 I/O와 **같은 단일 `io_context` 스레드**에서 블로킹으로 실행돼 동시 접속 ~72개에서 하드 리밋을 만듦(상세: `Docs/PROJECT_TECHNICAL_REVIEW.md` §7-4). 사용자가 "이건 관찰만 하고 넘길 문제가 아니라 서버 개발자로서 반드시 고쳐야 하는 문제"라고 판단, 실제 개선 착수 결정.

**사용자 결정 사항(2026-07-27 확정)**:
- 개선 방식: **`GW2_CrossPlatformCore/Thread/JobQueue` 재사용해서 저장 작업을 별도 워커 스레드로 넘김**(사용자가 직접 지목) — "시계열/append성 데이터라 원자성·경합 문제는 없다"는 판단.
- **WAL(`PRAGMA journal_mode=WAL` + `synchronous=NORMAL`) 병행 여부: 이번 라운드는 보류** — 정보 부족 사유(거부 아님, 추후 재검토 대상). WAL 동작 원리·JobQueue와의 관계(서로 다른 계층 — JobQueue는 "어느 스레드가 블로킹되는지"를 고치고, WAL은 "블로킹 비용 자체의 크기"를 줄임, WAL 단독으론 블로킹 문제 자체는 해결 안 됨) 논의 상세는 대화 기록 참고. **이번엔 JobQueue 비동기화만 진행.**

**조사 완료**:
- `JobQueue`(`enable_shared_from_this`, `Push`/`Execute`)는 한 인스턴스당 동시에 한 스레드만 실행 보장 — SQLite 단일 writer 제약과 자연스럽게 맞음. `GlobalQueue`(대기 큐) + `ThreadManager`(워커 스레드 관리)와 세트로 동작.
- **핵심 함정 1**: `JobQueue::Push(job, pushOnly=false)`(기본값)는 호출 스레드가 이미 다른 JobQueue의 `Execute()` 안이 아니면(`LCurrentJobQueue == nullptr`) **그 자리에서 동기 실행**해버림 — 워커 스레드로 반드시 넘기려면 `Push(job, /*pushOnly=*/true)`를 명시적으로 호출해야 함(편의 함수 `DoAsync()`는 내부적으로 `pushOnly=false`를 쓰므로 이 용도엔 못 씀, 저수준 `Push()`를 직접 호출해야 함).
- `ThreadManager::DoGlobalQueueWork()`는 스레드 로컬 `LEndTickCount`(이번 호출에서 큐를 비울 시간 예산)를 기준으로 동작 — 워커 루프에서 매 반복 `LEndTickCount = GetCurrentTick() + 예산ms`를 먼저 세팅해야 함(안 하면 즉시 break, 아무 일도 안 함). GW2 틱 서버 관례.
- **현재 `APM_Agent`(Agent/Collector/LoadTester)는 이 `Thread/` 서브시스템을 전혀 안 씀** — 워커 스레드가 하나도 안 떠 있는 상태. `GThreadManager->Launch(...)`로 새로 띄워야 함.
- CMake 변경 불필요 확인 — `GW2_CrossPlatformCore/CMakeLists.txt`가 이미 `Thread/`·`Main/`을 public include 경로로 노출 중(`target_include_directories`).
- **핵심 함정 2(2026-07-27 정정)**: 지난 세션에 "헤더 5개만 추가하면 된다"고 적어뒀던 게 부정확했음 — `GW2_CrossPlatformCore/Thread/*.h`는 자기완결적이지 않고 자신의 pch(`GW2_CrossPlatformCore/Main/CorePch.h`)가 특정 순서로 먼저 include해줬다는 걸 전제로 짜여 있음(예: `LockQueue.h`는 `USE_LOCK` 매크로를 쓰지만 그 매크로가 정의된 `CoreMacro.h`를 자기 스스로 include 안 함). `APM_Agent/pch.h`는 `CorePch.h`를 안 쓰므로(`Types.h`/`Container.h`만 가져옴), `Collector/main.cpp`가 `CorePch.h`와 동일한 순서로 직접 include해야 함: `CoreMacro.h` → `CoreGlobal.h` → `CoreTLS.h` → `Lock.h` → `ObjectPool.h` → `LockQueue.h` → `JobTimer.h` → `JobQueue.h` → `ThreadManager.h`(9개, `CorePch.h`에도 없어서 마찬가지로 별도 추가 필요 — 이 서브시스템을 실제로 기동하는 코드가 이 프레임워크 어디에도 없었다는 뜻).
- `JobQueueRef`(=`shared_ptr<JobQueue>`) 타입은 이미 `Main/Types.h`에 정의돼 있음(`USING_SHARED_PTR(JobQueue)`).

**설계 확정, 코드 전문 작성 완료 — `SESSION_LOG.md` 2026-07-27 항목 참고**:
- `main()`에서 `auto store = CreateMetricStore(...)` 직후: `IMetricStore* storePtr = store.get();` + `JobQueueRef metricStoreQueue = MakeShared<JobQueue>();` 선언 + `GThreadManager->Launch(...)`로 전용 워커 스레드 1개 기동.
- `PacketHandler::Register<apm::Metric>` 핸들러 안의 `store->Store(pkt)` 직접 호출을 `metricStoreQueue->Push(MakeShared<Job>([storePtr, pkt]{ storePtr->Store(pkt); }), /*pushOnly=*/true);`로 교체, 캡처 리스트도 `[&store, &pendingMetrics]` → `[storePtr, metricStoreQueue, &pendingMetrics]`로 변경.
- 콘솔 로그 문구 "metric stored" → "metric received"로 변경(저장이 이제 비동기라 로그 시점엔 아직 안 끝났을 수 있음) — **채택 확정**(SESSION_LOG 코드 전문에 반영됨, 지난 세션엔 "확정 아님"이었으나 이번에 그대로 채택).
- `APM_TRACE_SCOPE("Collector.StoreMetricAsync")` 추가(개선 전/후 지연시간 비교용) — **이번 코드 전문엔 미포함**, 필요시 후속으로 추가 가능(선택 사항으로 남겨둠).
- **짚어둘 캐치사항**: 워커 스레드가 `while(true)` 무한 루프라 정상 종료 경로가 없음 → 전역 정적 `GThreadManager` 소멸자의 `Join()`이 절대 안 끝남. 다만 `Collector`는 애초에 `ioContext.run()`도 무한 루프(시그널 핸들러 등 종료 처리 없음, kill로만 종료)라 **새로 생기는 문제는 아님** — 기존 관례를 그대로 따름.

**2026-07-27 적용 완료** — 사용자가 "바로 적용해줘"로 명시 확인, `Collector/main.cpp` 1개 파일(include 블록 + `main()` 함수 전문) 실제 반영.

**적용 중 발견해 그 자리에서 고친 문제 1건**: 빌드 시도 중 `CoreMacro.h`(`PrintStackTrace()`/`CrashLog()`)가 `<fstream>`/`<execinfo.h>`(Windows는 `<dbghelp.h>`)를 자기 스스로 include하지 않고 자신의 pch(`CorePch.h`)가 미리 include해줬다는 전제로 짜여 있던 것을 추가로 발견 — 애초 설계 조사 때(9개 헤더 정정) 못 잡았던 부분. `Collector/main.cpp`의 include 블록에 `<fstream>` + `#ifdef _WIN32 <dbghelp.h> #else <execinfo.h> #endif`를 `CoreMacro.h` 앞에 추가해 해결.

**검증 완료(WSL, 2026-07-27)**:
- `cmake --build build --target Collector` → 최초 시도는 위 헤더 누락으로 컴파일 에러, 수정 후 재시도 성공.
- `cmake --build build`(전체) → `GW2_CrossPlatformCore`/`APM_Storage`/`APM_Common`/`Collector`/`APM_Common_Tests`/`Agent`/`LoadTester` 전부 빌드 성공(회귀 없음).
- `ctest --test-dir build` → `9/9 tests passed`(기존 `AesGcmCipher`/`AesGcmPayload` 테스트, 이번 변경과 직접 관련된 테스트는 없음 — Collector `main()`은 애초에 유닛테스트 대상이 아님).

**"남은 선택 사항(실행 관점 확인은 선택)"이라고 적어뒀던 게 틀렸음** — 아래 1-7-b에서 그 "선택 사항"(LoadTester 재실측)을 실제로 해보니 필수였던 심각한 버그가 발견됨. 코드/빌드 검증만으론 못 잡는 문제였음(교훈: 스레드 분리처럼 동시성이 실제로 개입하는 변경은 빌드 성공+단위테스트 통과만으로 "완료"라 부르면 안 됨).

**보류 항목(여전히 유효)**: WAL(`journal_mode=WAL` + `synchronous=NORMAL`) — 정보 부족으로 미적용, 추후 재검토 대상. 아래 1-7-b가 먼저 해결돼야 그다음에 다시 다룰 수 있음.

---

### 1-7-b — 재실측 중 크래시 발견 + `JobQueue` → 자체 `WorkerQueue` 전환 ✅ 코드 적용 + 빌드 + 재실측 검증 완료(2026-07-28)

**무엇이 문제인가**: 위 1-7(JobQueue 버전, 이미 커밋됨 `39a3772`)을 실제로 검증하려고 1-5와 동일한 6단계 LoadTester 매트릭스를 재실행했더니, **6단계 전부 Collector가 시작 후 약 10초 만에 크래시**(`connect_fail`이 agents=10부터 이미 발생, agents=1도 뒤늦게 크래시 — `apm_metrics.db` 파일 하나 재사용하는 게 아니라 완전히 새로 뜬 프로세스가 매번 10초 만에 죽음). 즉 **재실측 6개 결과는 전부 무효** — 1-7의 실제 효과(72개 하드 리밋이 풀렸는지)는 아직 검증 안 된 상태.

**근본 원인**: `collector_stdout.log`에서 `[CRASH] cause=LOCK_TIMEOUT ... file=Lock.cpp line=52 func=WriteLock` 확인. `GW2_CrossPlatformCore/Thread/Lock.cpp`의 `Lock::WriteUnlock()`이 "소유 스레드가 언락할 때" 분기에서 `_writeCount`를 안 줄이고 `_lockFlag`의 무관한 하위 비트만 건드려, **락 소유자 ID 비트(`WRITE_THREAD_MASK`)가 영원히 안 지워짐**. 그 락을 처음 잡았다 놓은 스레드가 그 락을 영구 소유한 것처럼 남아, 다른 스레드가 나중에 같은 락을 잡으려 하면 10초 스핀 후 `CRASH("LOCK_TIMEOUT")`. `JobQueue`(`LockQueue` 내부에서 이 `Lock`을 씀)는 지금까지 `APM_Agent`에서 한 번도 크로스 스레드로 안 쓰였어서(1-7 이전엔 이 서브시스템 자체를 안 씀) 이 버그가 여태 안 드러났었고, 1-7에서 **네트워크 스레드가 `Push()`(락을 처음 잡음), 워커 스레드가 `Execute()`(같은 락을 다른 스레드에서 잡으려 시도)** 하면서 최초로 재현됨.

**사용자 결정(2026-07-27)**: `GW2_CrossPlatformCore/Thread/Lock.cpp`는 수정하지 않음 — "여러 프로젝트를 통해 이미 검증한 내용"이라는 판단. 대신 **`APM_Agent` 안에서 `JobQueue`를 대체할 자체 큐를 새로 만드는 방향**으로 확정.

**설계 완료, 코드 전문 작성 완료 — `SESSION_LOG.md` 2026-07-27 두 번째 항목("1-7 재실측 중 크래시 발견 + `JobQueue` → 자체 `WorkerQueue` 전환 설계") 참고, 아직 파일로는 미반영**:
- 신규 `Common/WorkerQueue.h`/`.cpp` — `std::mutex`/`std::condition_variable`/`std::queue<std::function<void()>>`만 쓰는 단일 워커 스레드 큐(`SpanRecorder`와 같은 패턴). `GW2_CrossPlatformCore/Thread/*` 의존 완전 제거.
- `Common/CMakeLists.txt`에 `WorkerQueue.cpp` 한 줄 추가.
- `Collector/main.cpp`: 1-7에서 추가했던 9개 헤더(`CoreMacro.h` 등) + `<fstream>`/`<execinfo.h>` 우회 코드를 전부 제거하고 `#include "WorkerQueue.h"` 한 줄로 교체. `JobQueueRef metricStoreQueue = MakeShared<JobQueue>(); GThreadManager->Launch(...)` 블록을 `WorkerQueue metricStoreQueue;`(로컬 객체, 생성자에서 워커 스레드 자동 기동)로 교체. `PacketHandler::Register` 람다의 캡처를 `metricStoreQueue`(값 복사) → `&metricStoreQueue`(참조)로, `metricStoreQueue->Push(MakeShared<Job>(...), true)` → `metricStoreQueue.Push([...]{ ... })`로 교체.
- 부수 효과: `WorkerQueue` 소멸자가 큐를 다 비운 뒤 `join()`하므로, 1-7에 남겨뒀던 "워커 스레드 정상 종료 경로 없음" 캐치사항도 해소됨.

**2026-07-28 적용 완료** — 사용자가 "적용해줘"로 명시 확인, 아래 4개 파일 실제 반영:
- 신규 2개: `Common/WorkerQueue.h`/`.cpp`(단일 워커 스레드, `std::mutex`/`condition_variable`/`std::queue`만 사용, `GW2_CrossPlatformCore/Thread/*` 의존 제거)
- 수정 2개: `Common/CMakeLists.txt`(`WorkerQueue.cpp` 한 줄 추가), `Collector/main.cpp`(include 블록에서 9개 헤더 + `<fstream>`/`<execinfo.h>`/`<dbghelp.h>` 우회 코드 제거하고 `#include "WorkerQueue.h"`로 교체, `JobQueueRef metricStoreQueue = MakeShared<JobQueue>()` + `GThreadManager->Launch(...)` 블록을 `WorkerQueue metricStoreQueue;` 로컬 객체로 교체, `PacketHandler::Register` 람다 캡처 `metricStoreQueue`(값) → `&metricStoreQueue`(참조), `Push(MakeShared<Job>(...), true)` → `Push([...]{...})`로 교체)

적용 후 disk 상태가 `SESSION_LOG.md` 설계안과 일치함을 재확인 완료.

**검증 완료(사용자, WSL, 2026-07-28)**: `cmake --build build` → `GW2_CrossPlatformCore`/`APM_Storage`/`APM_Common`(신규 `WorkerQueue.cpp.o` 포함)/`Collector`/`Agent`/`LoadTester`/`APM_Common_Tests` 전부 빌드 성공.

**재실측 완료(사용자, WSL, 2026-07-28)** — `run_load_test.sh` 6단계 매트릭스(1/10/50/100/100+ramp5s/300) 재실행, **6단계 전부 크래시 없이 60초 끝까지 정상 종료**(`loadtest_results/agents_*_20260728_*` 6개). 1-5 베이스라인과 비교:

| agents | 개선 전 접속 성공 | 개선 전 p95/p99 | 개선 후 접속 성공 | 개선 후 p95/p99 |
|---|---|---|---|---|
| 1 | 1 | 0/1ms | 1 | 0/1ms |
| 10 | 10 | 0/1ms | 10 | 0/1ms |
| 50 | 50 | 12,051/22,177ms | 50 | **0/1ms** |
| 100 | 72(하드 리밋) | 34,212/48,412ms | **100(전원)** | 0/1,010ms |
| 100(ramp5s) | 72 | 31,572/45,213ms | **100(전원)** | 0/1ms |
| 300(ramp5s) | 71 | 33,467/47,732ms | **229**(3배↑) | 20,512/26,994ms |

`strace` 요약으로 300-agent 기준 네트워크 스레드의 `fdatasync`(9,270→8회)/`pwrite64`(23,340→11회)가 급감한 것도 확인 — SQLite 저장이 워커 스레드로 실제로 이동했음을 syscall 레벨로 검증. **100개 이하 구간의 하드 리밋은 완전히 해소**(전원 접속 성공, 지연시간 사실상 0ms).

**새로 발견한 병목(300-agent 한정, 이번 라운드 범위 밖으로 기록만)**: 처리량이 늘자(같은 60초간 처리 로그가 9,625줄→90,276줄로 9.4배 증가) `PacketHandler::Register` 핸들러의 `std::cout << ... << std::endl`(메트릭 1건마다 동기 flush)이 새 병목으로 드러남 — 300-agent 재실측에서 `write` syscall이 전체 시간의 92% 차지. `queue_drop=0`(유실 없음)이라 심각도 낮음. 다음에 다룰 경우 후보: `std::endl` → `'\n'` 교체 또는 로깅 빈도/버퍼링 조정.

**README.md**/`Docs/PROJECT_TECHNICAL_REVIEW.md`(신규 §7-5) 반영 완료(2026-07-28) — 사용자가 "문서화만 하고 로깅 병목은 넘어가기"로 확정, 로깅 병목은 후속 과제로만 기록.

**git 커밋 완료(2026-07-28, `2050b498` "APM 2차 마무리")** — 위 변경분 전부 반영됨. 참고: `git add -A` 방식으로 커밋된 것으로 보여 `crash.log`/2026-07-27 무효 재실측 결과 6개도 같이 딸려 들어감(아래 "정리 필요한 산출물" 참고, 급한 문제는 아님).

**정리 필요한 산출물(git에 이미 커밋됨, 원하면 별도 정리 커밋으로 제거 가능)**:
- `APM_Agent/crash.log` — 진단 끝난 파일.
- `APM_Agent/loadtest_results/agents_*_20260727_11*` (6개) — 크래시로 무효한 데이터, 실측 비교 시 참고하면 안 됨.

---

### 1-7-d — WAL(`journal_mode=WAL`+`synchronous=NORMAL`) 도입 ✅ 완료(2026-07-28)

**배경**: 1-7-b(`strace -c`, 네트워크 스레드만 추적) 요약에서 `fdatasync`가 8회로 급감한 걸 근거로 "SQLite 저장 비용이 사라졌다"고 정리했으나, `strace`가 기본적으로 새 스레드를 안 따라간다는 걸 감안하면 이건 착시일 수 있다는 논의가 나옴. WAL 도입 여부를 결정하기 전에 **워커 스레드 자체까지 실측**하기로 함(사용자 요청: "이 과정 대신 시행해줄 수는 없는건가" — `wsl.exe`로 직접 WSL 안에 들어가 빌드/테스트/부하테스트를 대행할 수 있음을 이번에 확인, 이후 직접 실행).

**측정(`strace -f -c`, 300-agent, WAL 적용 전)**: `fdatasync`가 다시 나타남 — 9,008회, 10.8초(17.1%). `fdatasync`+`pwrite64`+`fcntl` 합산 전체 syscall 시간의 약 25%. 착시였음을 확인, WAL 도입 근거 확보.

**적용 완료** — 사용자가 "문서화하고 WAL 적용하자"로 명시 확인, `APM_Agent/Storage/SqliteMetricStore.cpp` 생성자에 `PRAGMA journal_mode = WAL;`(결과를 `CapturePragmaResult` 콜백으로 확인해 실패 시 로그) + `PRAGMA synchronous = NORMAL;` 추가. `.gitignore`에 `*.db-wal`/`*.db-shm`/`*.db-journal` 추가(WAL 사이드카 파일이 `*.db` 패턴에 안 걸려 실수로 커밋될 뻔한 걸 미리 방지).

**검증 완료(WSL, `wsl.exe`로 직접 실행, 2026-07-28)**:
- `cmake --build build` 성공, `ctest` 9/9 통과.
- 스모크 테스트: Collector 짧게 실행 후 `apm_metrics.db-wal`/`-shm` 파일 생성 확인 + "WAL 모드 전환 실패" 로그 없음 → WAL 정상 활성화 확인.
- 재실측(`strace -f -c`, 300-agent, WAL 적용 후) vs 적용 전 비교:

| syscall | 적용 전 | 적용 후 | 변화 |
|---|---|---|---|
| `fdatasync` | 10.81s(17.1%), 9,008회 | 0.34s(0.7%), 54회 | -97% 시간 |
| `pwrite64` | 3.05s(4.8%), 22,677회 | 4.92s(9.9%), 34,212회 | +61% |
| `fcntl` | 2.31s(3.7%), 20,280회 | 12.41s(24.9%), 98,972회 | +437% |
| `futex` | 30.41s(48.0%) | 18.19s(36.5%) | -40% |
| `write`(로깅) | 11.34s(17.9%) | 12.75s(25.6%) | 거의 동일 |
| 전체 syscall 시간 | 63.33s | 49.81s | **-21%** |
| connect_success | 246/300 | 250/300 | 유의미한 차이 없음 |

**결론**: `fdatasync`는 거의 제거됐지만(-97%), WAL의 공유메모리 인덱스 락(`fcntl`)이 5배 넘게 늘어 fsync 비용의 상당 부분이 락 비용으로 형태만 바뀜(전체 -21%는 실제 순이득). 300-agent 극단값에서 `connect_success`/지연시간은 유의미하게 안 바뀜 — 이 규모에선 이미 `write`(콘솔 로깅)/`futex`(락 대기)가 더 크게 지배. 100개 이하 구간(1-7-b에서 이미 0~1ms로 해소)엔 체감 효과 작음, 장기 운영 시 디스크 I/O 총량 감소 의미.

**문서 반영 완료**: `Docs/PROJECT_TECHNICAL_REVIEW.md`(신규 §7-6, §7-4/§7-5 관련 서술 갱신), `README.md`.

**git 커밋 완료(2026-07-28, `2d44c72` "Enable SQLite WAL mode for the metric storage worker thread")** — 위 변경분(`SqliteMetricStore.cpp`, `.gitignore`, 문서 3종, `loadtest_results/straceF_300_*` 2개, `apm_metrics.db-journal` 삭제) 전부 반영됨. 참고: 같은 커밋에서 `run_load_test.sh`의 실행 권한 비트가 `755→644`로 같이 바뀜(내용 변경 없음, 원인 미확인 — WSL/Windows 파일시스템 간 상호작용으로 추정, 실행엔 `bash run_load_test.sh`로 문제없음).

**남은 것**: 콘솔 로깅 병목(1-7-b에서 발견, 300-agent 한정)은 여전히 후속 과제로만 남아있음. WORK_STATUS.md 로드맵 표는 이번 갱신에서 최신 상태로 반영 완료.

---

### 1-7-e — 콘솔 로깅 병목(1-7-b에서 발견) 개선 — 워커 스레드 위임 + `'\n'` 결합 ✅ 코드 적용 + 빌드 + 재실측 검증 완료(2026-07-29)

**배경**: 1-7-b 300-agent 재실측에서 새로 발견해 후속 과제로 남겨뒀던 항목(`PacketHandler::Register` 핸들러의 `std::cout << ... << std::endl`이 메트릭마다 강제 flush를 부르며 네트워크 스레드를 블로킹). 사용자가 이번 세션에 개선 착수를 요청.

**사용자 결정**: 애초 제안한 "1순위(`std::endl`→`'\n'` + `sync_with_stdio(false)`만)" 대신, 사용자가 "2순위(로깅을 워커 스레드로 위임)를 먼저 적용하는 게 맞아 보인다"고 판단(사유: flush를 생략하면 메모리 버퍼에 문제가 생길 것 같다는 우려) → 이 우려는 정정(`std::cout` 버퍼는 고정 크기라 문제 없음)했으나, 정정 과정에서 "2순위를 `std::endl` 유지한 채 그대로 적용하면 `WorkerQueue` 내부 무제한 큐 적체로 실제 메모리 증가 리스크가 있다"는 진짜 리스크를 발견 → 2순위(워커 스레드 위임) + 1순위 일부(`'\n'`, `sync_with_stdio(false)`)를 결합하는 방향으로 확정. 상세: `SESSION_LOG.md` 2026-07-29 항목.

**적용 완료(2026-07-29)** — `Collector/main.cpp` 1개 파일, 4곳 수정:
1. `main()` 맨 앞에 `std::ios::sync_with_stdio(false);` 추가.
2. `WorkerQueue metricStoreQueue;` 옆에 `WorkerQueue consoleLogQueue;` 신설(책임 분리 — 로그 flush 지연이 메트릭 저장을 밀리게 하지 않도록).
3. `PacketHandler::Register<apm::Metric>` 람다 캡처에 `&consoleLogQueue` 추가.
4. `std::cout << ... << std::endl;` 블록을 `consoleLogQueue.Push([pkt]() { ... << '\n'; });`로 이동(네트워크 스레드에서 로그 조립/출력 자체를 떼어냄).

**검증 완료(WSL, 2026-07-29)**:
- `cmake --build build` 성공(회귀 없음), `ctest` 9/9 통과.
- `strace -f -c`로 300-agent(ramp-up 5s, 60초) 재실측, 직전 WAL 적용 후 실측(`straceF_300_afterWAL_20260728_122148`)과 비교:

| 지표 | WAL 적용 후(로깅 워커 분리 전) | 이번 실측(로깅 워커 분리 후) | 비고 |
|---|---|---|---|
| connect_success | 250/300 | **300/300(전원)** | 목표 달성 — 로깅에 의한 네트워크 스레드 블로킹 해소 |
| sent_total | 144,277 | 171,874 | 연결 성공 수가 늘어 처리량 자체가 증가(비교 시 감안 필요) |
| latency p95/p99 | 19,890ms/26,044ms | 19,322ms/24,970ms | 더 많은 연결(250→300)을 처리하면서도 지연시간은 악화되지 않음 |
| write | 12.75s(25.6%) | 11.44s(13.29%), 136,748회 | 절대 시간은 비슷, 전체 시간 대비 비중은 감소 |
| fcntl | 12.41s(24.9%), 98,972회 | 15.44s(17.94%), 122,682회 | 연결 성공 수 증가로 SQLite WAL I/O 총량 자체가 늘어난 영향 |
| fdatasync | 0.34s(0.7%), 54회 | 0.20s(0.23%), 69회 | 변화 없음(WAL 유지) |
| **futex** | 18.19s(36.5%) | **49.26s(57.22%), 58,738회, 에러 8,567건** | **새로 드러난 병목** — 워커 스레드가 1개(`metricStoreQueue`)에서 2개(+`consoleLogQueue`)로 늘며 스레드 간 락 경합 증가 |
| 전체 syscall 시간 | 49.81s | **86.09s** | 처리량 증가 + futex 경합 증가가 겹친 결과 |
| syscall 시간/메트릭(정규화) | 49.81s÷144,277건 ≈ 0.345ms | 86.09s÷171,874건 ≈ 0.501ms | 메트릭 1건당 총 syscall 비용은 오히려 증가 |
| queue_drop | 0 | 0 | 유실 없음 유지 |

- `collector_stdout.log`(187,242줄, `metric received` 95,948건)에 크래시/WAL 실패 로그 없음 — 정상 동작 확인.

**결론 — 목표는 달성했으나 새 트레이드오프 발견**: 애초 목표(콘솔 로깅이 네트워크 스레드를 블로킹해 접속 성공률을 깎아먹는 문제)는 해소됨 — 300-agent 시나리오에서 처음으로 전원(300/300) 접속 성공, 더 많은 트래픽을 처리하면서도 지연시간은 악화되지 않음. 다만 워커 스레드가 1개→2개로 늘면서 스레드 간 `Push`/`notify` 락 경합(`futex`)이 새로운 지배적 비용(57%)으로 떠올라, 메트릭 1건당 총 syscall 비용(정규화 기준)은 오히려 늘어남 — 1-7-d WAL 트레이드오프(`fdatasync` 감소 vs `fcntl` 증가)와 같은 패턴("한 비용을 줄이면 다른 형태의 비용이 늘 수 있다")이 이번에도 재현됨.

**남은 것**: futex 경합(스레드 간 락 비용)은 이번 라운드 범위 밖 — 완화하려면 두 큐를 다시 하나로 합치거나(단, 로그/저장 책임 분리 이점 상실), 로그를 배치로 묶어 `Push` 빈도 자체를 줄이는 방향 등을 고려할 수 있음. `queue_drop=0`(유실 없음)이라 심각도는 낮아 후속 과제로 기록만.

**커밋 완료(2026-07-29, `83f3761` "Move Collector console logging off the network thread")**.

---

### 1-7-f — 1-7-e 검증 중 발견한 버그: `sync_with_stdio(false)` + 다중 스레드 `cout` 동시 쓰기 레이스 ✅ 수정 + 빌드/재실측 검증 완료(2026-07-29)

**배경**: 사용자가 "Collector 1대 : Agent 100개"를 성능 기준으로 삼고 싶다며 WAL+로깅 개선(1-7-d, 1-7-e) 적용 후 100-agent 규모에서 실제로 개선됐는지 확인 요청. 100-agent 재실측 결과를 스레드별로 직접 계측(임시 진단 코드)하다가 `std::cout`/`std::cerr`에 여러 스레드가 동시에 쓰면서 출력이 문자 단위로 깨지는 걸 발견(`consoleLogQueue`는 실제로 별도 스레드였지만, 네트워크 스레드가 `connection accepted`/`accept error`/`WebServer로 N건 전송 시도`×2를 여전히 직접 `cout`/`cerr`로 찍고 있었음 — 1-7-e의 `sync_with_stdio(false)`가 C stdio 내부 락을 없애 이 동시 접근이 실제 레이스로 이어짐).

**영향 범위**: 콘솔 로그 텍스트 가독성 문제일 뿐 — 저장된 메트릭 데이터, `queue_drop`, 이미 측정한 syscall 레벨 지표(1-7-e 표)에는 영향 없음. 상세 원인 분석·수정 전/후 코드 전문(`Collector/main.cpp` 2곳)은 `SESSION_LOG.md` 2026-07-29 두 번째 항목 참고.

**수정 완료(2026-07-29)** — 네트워크 스레드에 남아있던 나머지 cout/cerr 호출 4곳(연결 수립, accept 에러, WebServer 전송 시도 메트릭/span)을 전부 `consoleLogQueue.Push(...)`로 위임 — 런타임 중엔 오직 `consoleLogQueue` 워커 스레드 하나만 스트림을 건드리도록 통일해 레이스 원천 차단(`sync_with_stdio(false)`는 유지, 단일 쓰기 스레드 하에선 안전).

**검증 완료(WSL, 2026-07-29)**:
- `cmake --build build` 성공, `ctest` 9/9 통과.
- 100-agent 스트레스(interval 50ms, ramp 2s, 10초)로 레이스 재현 확인 — 수정 후 81,381줄 전부 정상 접두어로 시작(깨진 줄 0건, 수정 전엔 진단 로그가 실제로 뒤섞이는 걸 확인했었음).
- 100-agent 표준 재실측: connect_success 100/100, p95/p99 0/2,053ms — 레이스 수정 전(2,049ms)과 사실상 동일, **레이스 수정이 성능 지표 자체는 안 바꿈**을 확인(스레드 배치만 정리, 총 작업량 불변).
- 300-agent `strace -f -c` 재검증: futex 57.78%(51.10s)/write 13.15%(11.63s)/전체 88.45s/connect_success 300/300, 로그 98,600줄 전부 정상 — 직전 1-7-e 보고값(futex 57.22%/49.26s, write 13.29%/11.44s, 전체 86.09s)과 오차범위 내 일치. **1-7-e 비교 표 수치는 그대로 유효, 갱신 불필요**.

**100-agent 기준 질문에 대한 결론**: WAL+로깅 개선은 100-agent 규모에선 1-7-b(WorkerQueue)에서 이미 해소된 하드 리밋(72→100)에 추가 이득을 주지 않음 — 오히려 p99가 소폭 늘어남(1,010ms→2,053ms), 워커 스레드가 1개→3개로 늘며 생기는 동기화 오버헤드로 보임(300-agent futex 경합 증가와 같은 패턴, 규모만 작을 뿐). 이 개선의 실질 효과는 300-agent 같은 고부하 구간(접속 성공 250→300)에 있음 — **100-agent를 기준으로 삼는다면 "이번 라운드 개선은 이 규모에선 순효과가 거의 없거나 근소하게 손해"가 정확한 결론**.

**커밋 완료(2026-07-29, `d664153` "Fix a console-log data race introduced by the previous logging fix")**.

**최종 결정(2026-07-29, 사용자 확정) — 이 스레드 종료**: 300-agent 시나리오에 남아있는 futex 경합(스레드 간 락 대기)은 **Collector 단일 프로세스의 처리 능력 한계 또는 테스트에 쓰는 서버 머신 자체의 스펙 미달**로 판단하고 더 파고들지 않기로 함. 실제 운영이라면 이 지점부터는 코드를 더 최적화하기보다 Collector를 여러 대로 수평 확장하는 게 정공법이라는 결론. `README.md`/`Docs/PROJECT_TECHNICAL_REVIEW.md`(신규 §7-7, 버그 8) 문서 반영 완료 — 이로써 **로드맵 1~7-f 전부 완료 처리, 프로젝트를 완료로 평가**(사용자 확정). 남은 건 미뤄뒀던 실행 관점 시각 검증(`/apm/alerts`, `/apm/traces`)뿐이며 이번 세션에서 문서화와 동시 진행.

**포트폴리오 문서(`Docs/portfolio_apm.html`) 갱신 완료(2026-07-29)** — `SESSION_LOG.md` 전체(2026-07-26~29, 1순위~1-7-f)를 다시 읽어 이관 이후 추가된 기능/서사가 전혀 반영 안 돼 있던 걸 확인하고 보강:
- 신규 섹션 `#features`("메트릭 수집기"에서 "APM"으로) — 2~5순위(알림/보존정책/트랜잭션 계측/백분위 통계) 설계 판단 카드 4개.
- 신규 섹션 `#scale`(300 동시 접속까지 — 병목을 찾고 고치는 5라운드) — 1-5 베이스라인부터 1-7-f까지 "측정→수정→재측정" 서사를 라운드별 카드 + 요약 표 + "여기서 멈추기로 함" 콜아웃으로 정리.
- `#spotlight`에 Bug #3(EF Core+SQLite `DateTimeOffset` 크래시) 신규 스포트라이트 추가, 요약 표에 콘솔 로그 동시쓰기 레이스 행 추가.
- hero 통계(테스트 수 17→27, 300-agent 부하 테스트·버그 10건 항목 추가), 기술 스택에 "성능 프로파일링/동시성" 그룹 신설.
- Claude 아티팩트로 게시(비공개) — `https://claude.ai/code/artifact/60fa80c5-aa9a-4bcd-bcea-0962ce3a5435`, 파일 경로 재게시로 갱신 가능.
- 아키텍처 섹션 바로 뒤에 `#screenshots`("실행 화면") 신규 섹션 추가 — `/apm/dashboard`/`/apm/alerts`/`/apm/traces` 3장 스크린샷 자리를 `aspect-ratio` 고정 placeholder(`.screenshot-frame`)로 미리 확보. 실제 스크린샷 촬영 후엔 그 div를 `<img>`로만 교체하면 레이아웃이 안 흔들리도록 설계, 교체 방법은 섹션 바로 위 HTML 주석에 명시.
- **2차 갱신(같은 날)**: `Docs/PROJECT_TECHNICAL_REVIEW.md`를 참고해 추가 반영 —
  - 신규 섹션 `#metric-impl`(지표 수집 — OS 커널에서 직접) — §6(수집 지표 구현 상세)의 jiffies CPU 델타/`MemAvailable`/TCP_INFO 커널 조회/`statvfs` 내용을 표+카드로 반영, 이전엔 포트폴리오에 전혀 없던 내용.
  - 설계 결정 카드 `// 10`(테스트 전략) 추가 — §10 내용(전체 커버리지 대신 암호화+프레이밍 우선, 테스트 가능하게 만든 리팩터링) 반영.
  - 스크린샷 자리 3장→4장으로 확장(터미널/부하테스트 실행 로그 자리 추가) — 신규 섹션(`#metric-impl`)에 대응하는 시각 자료 자리 확보.
  - 아티팩트 같은 링크로 재게시 완료.

**스크린샷 4장 실제 촬영 완료(2026-07-29)** — 사용자가 `sudo apt-get install libnss3 libnspr4 libasound2t64`(headless Chromium 구동용) + `sudo apt-get install fonts-noto-cjk`(한글 렌더링용, 1차 촬영에서 한글이 전부 □로 깨져 나와 추가 요청) 실행. Playwright(Node 18 호환을 위해 1.47.0 핀 고정, 스크래치패드에 로컬 설치)로 실제 캡처:
- Collector+Console을 클린 상태로 다시 띄우고 `LoadTester`로 실 트래픽 생성, CPU 임계치를 일부러 낮춰 활성 알림까지 렌더링되게 만든 뒤 `/apm/dashboard`/`/apm/alerts`/`/apm/traces` 3장 캡처.
- 4번째("터미널")는 raw 터미널 캡처 대신, 실제 로그 내용(이번 세션 Collector `metric received` 라인 + 기존 커밋된 300-agent `loadtester_result.csv`의 `connected=300` 요약)을 페이지 다크 테마와 맞춘 HTML로 재현해 스크린샷 — 내용 자체는 전부 실측/실 로그 원문.
- `Docs/screenshots/{dashboard,alerts,traces,terminal}.png` 4개 파일로 저장, `#screenshots` 섹션의 placeholder `<div>` 4개를 실제 `<img src="screenshots/...">`로 교체(상대 경로, `aspect-ratio` 유지).
- 아티팩트 재게시 완료. **참고**: Claude 아티팩트는 단일 파일만 렌더링하는 self-contained 제약이 있어, 상대 경로 이미지가 아티팩트 미리보기에서는 안 보일 수 있음(레포 파일 자체를 열면 정상 렌더링) — 채팅에서 4장 다 직접 확인시켜드림. — 문서 전체(1259줄)를 다시 읽고 `PROJECT_TECHNICAL_REVIEW.md` §3~5(보안/프로토콜/저장) 나머지 부분까지 재확인, 구조 이상 없음(태그 균형/교차참조 방향/"게임" 네이밍 원칙 위반 없음) 확인 후 실제 갭 2건 반영:
- hero 태그에 알림/보존/트랜잭션 추적/백분위 통계 + 부하 테스트/strace 프로파일링 태그 추가 — 기존 태그가 전부 초기 파이프라인(암호화/저장/닷넷)만 나열해 스크롤 전 첫인상에서 새 기능/스케일 테스트가 안 보였음.
- 설계 결정 카드 `// 11`(Collector↔Console 관계 — JSON 설정 파일을 쓴 이유 + stdin 별도 스레드/asio::post로 락 불필요하게 만든 동시성 설계) 신규 추가 — §3-7 내용 중 유일하게 반영 안 돼 있던 부분.
- 아티팩트 같은 링크로 재게시 완료.

---

### 1순위 — 부하/스케일 테스트 툴 ✅ 실측 + 문서화 완료

| # | 작업 | 상태 | 메모 |
|---|---|---|---|
| 1-1 | 기존 코드 구조 파악 | ✅ 완료 (2026-07-26) | `Agent`/`Collector` `main.cpp`, `ApmSession`, `ResilientSender`, `PacketHandler` 확인. **발견**: `Collector`가 `ioContext.run()`을 메인 스레드에서 단일 호출(단일 스레드 io_context) — 접속 수가 늘어도 복호화/파싱/SQLite 저장은 한 스레드에서 순차 처리. 스케일 병목의 1차 가설. |
| 1-2 | LoadTester 아키텍처 설계 제안 | ✅ 완료 (2026-07-26) | in-process asio 다중 연결 시뮬레이터(별도 프로세스 N개 fork 대신), 기존 `ResilientSender`/`AesGcmPayload`/`apm::Metric` 재사용. 상세: `SESSION_LOG.md` 2026-07-26 항목 |
| 1-3 | 코드 스켈레톤(멤버 변수/함수 시그니처 전체) 제시 | ✅ 완료 (2026-07-26) | `LoadTester/` 신설안 + `ResilientSender` 콜백 추가안 제시. 상세: `SESSION_LOG.md` 2026-07-26 두 번째 항목. **사용자 확인 필요 4건 → 전부 추천안대로 확정 (2026-07-26)** |
| 1-4 | 구현 + 빌드 검증 | ✅ 완료 (2026-07-26) | `ResilientSender.h/.cpp` 수정 적용(선택적 `SendCallback`/`ConnectionStateCallback` 추가, `Agent/main.cpp`는 기본값 `nullptr`라 무변경). `APM_Agent/LoadTester/` 6개 파일 신규 작성 + `CMakeLists.txt`에 `LoadTester` 타겟 추가. **사용자가 WSL(Ubuntu, GNU 13.3.0)에서 `cmake --build build --target LoadTester` 빌드 성공 확인**(경고 없음 — `GW2_CrossPlatformCore`의 기존 `ASIO_STANDALONE` 재정의 경고만 있고 이번 변경과 무관). 코드 전문은 `SESSION_LOG.md` 2026-07-26 세 번째 항목 참고. |
| 1-5 | 실측 (N-agent 스케일 syscall/RSS/CPU/처리량/지연) | ✅ 완료 (2026-07-26) | 매트릭스 6단계(1/10/50/100/100+ramp-up/300 에이전트) 전부 실행 완료. **핵심 발견**: 동시 접속 성공 수가 71~72개에서 하드 리밋(100/300 요청 모두 동일) — ramp-up으로도 안 바뀜. 지연시간은 50 에이전트부터 절벽(p95 0ms→12초→34초). `queue_drop=0`(유실 없음, 그냥 밀림). CPU/메모리는 병목 아님(RSS 13~19MB 안정, CPU 평균 ~28%로 요청 규모 무관). `strace` 분석 결과 `pwrite64`/`fcntl`/**`fdatasync`**/`write`/저널 파일 관리(`openat`/`unlink`)가 시간의 70%+ 차지 — SQLite 기본 롤백 저널의 매 INSERT마다 동기 `fdatasync`가 네트워크 I/O와 같은 단일 `io_context` 스레드를 블로킹하는 게 근본 원인으로 확인됨(§1-1 가설을 구체적으로 검증). 상세: `SESSION_LOG.md` 2026-07-26 네 번째 항목 |
| 1-6 | README/`Docs/PROJECT_TECHNICAL_REVIEW.md`에 결과 반영 | ✅ 완료 (2026-07-26) | `README.md`에 스케일 테스트 요약 bullet 추가(+ `.NET xUnit 8개→18개`로 테스트 카운트 오탈자 수정, 5순위까지 반영 안 돼 있던 것 발견해 같이 고침). `Docs/PROJECT_TECHNICAL_REVIEW.md`에 신규 `7-4. Collector 스케일 테스트 — LoadTester로 병목 찾기` 섹션(실측 표 + 발견 4건 + 결론 + 알려진 개선 방향) + 예상 질문 2건 추가 — 기존 문서 스타일(배경/실측/발견/예상 질문) 그대로 따름. |

### 2순위 — 알림(임계치 기반) ✅ 코드 적용 + 빌드/테스트 검증 완료

`APM_Console` 쪽에서만 닫히는 작업(Agent/Collector 변경 불필요). 확인 4건 확정: 대상 지표 CPU/메모리/디스크/TCP RTT, 임계치는 DB 저장+UI 편집, 상태 전이 시만 알림(OK→Alert→Resolved, Zabbix/Nagios/Alertmanager 방식), DB에 이력 영속화(`AlertRecord`). 설계 상세: `SESSION_LOG.md` 2026-07-26 다섯 번째 항목("2순위(알림) 설계 제안").

**2026-07-26 적용 완료** — 사용자가 "2순위 알림 기능 코드 적용"으로 명시 확인, 아래 12개 파일 실제 반영:
- 신규 7개: `Infrastructure/Persistence/AlertThreshold.cs`, `Infrastructure/Persistence/AlertRecord.cs`, `Infrastructure/AlertEvaluator.cs`, `tests/.../Infrastructure/AlertEvaluatorTests.cs`, `Models/AlertsViewModel.cs`, `Controllers/AlertsController.cs`, `Areas/Apm/Views/Alerts/Index.cshtml`
- 수정 5개: `Infrastructure/Persistence/ApmDbContext.cs`(DbSet 2개 + 시드 데이터), `Infrastructure/MetricsReceiverService.cs`(`EvaluateAlertsAsync` 추가), `ApmModule.cs`(`JsonStringEnumConverter` 추가), `Areas/Apm/Views/Dashboard/Index.cshtml`(알림 페이지 링크), `ApmConsole.Host/wwwroot/css/site.css`(`.alert-row-active` 스타일)

**검증 완료(사용자, WSL)**: `dotnet test` 결과 `Passed: 13, Failed: 0`(기존 8건 + 신규 `AlertEvaluatorTests` 5건) — 빌드/단위 테스트 통과 확인.

**다음 할 일**: `/apm/alerts` 페이지를 실제로 띄워 임계치 편집 저장(`POST /apm/alerts/thresholds`)과 SignalR 실시간 알림(`AlertOpened`/`AlertResolved`)이 브라우저에서 의도대로 동작하는지 수동 확인하면 2순위 완전 종료. **(2026-07-26 확인: 아직 미실행)** — 빌드/단위 테스트만 검증됐고 브라우저 시각 검증은 보류 중.

**실행 검증 완료(2026-07-29)** — Collector+APM_Console을 실제로 함께 띄워 검증(과정에서 `RetentionService` 크래시 버그 발견/수정, 아래 참고). GUI 브라우저 스크린샷은 샌드박스에 Chromium 구동용 시스템 라이브러리(`libnss3` 등)가 없어 `sudo` 설치가 필요해 보류했으나, `curl`로 실제 HTTP 상호작용까지 확인:
- `GET /apm/alerts` → 200, 임계치 4개(CPU/메모리/디스크/TCP RTT) 정상 렌더링.
- `POST /apm/alerts/thresholds`(`value[1]=77` 등)로 임계치 편집 → 302 리다이렉트 → 재조회 시 실제로 77로 반영된 것 확인(폼 저장 경로 실동작 확인).
- CPU 임계치를 1%로 낮추고 `LoadTester`로 실 트래픽 발생 → `AlertEvaluator`가 실제로 알림을 열고, `/apm/alerts` 재조회 시 "CPU 사용률, 발생 07/29 01:18:39, 측정값 40.7, 임계치 1.0"이 `alert-row-active` 스타일로 뜨는 것 확인 — 임계치 초과 감지→저장→렌더링 전체 파이프라인이 실제로 동작함을 확인. 검증 후 임계치는 90으로 원복.
- SignalR(`AlertOpened`/`AlertResolved`)이 **살아있는 브라우저 클라이언트**로 실시간 push되는지는 미검증(웹소켓 클라이언트 없이는 확인 불가) — 위 알림 상태 전이/저장/렌더링 자체는 검증됐으므로 리스크는 낮다고 판단.

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

**남은 선택 사항(코드/빌드 관점에선 3순위 완료, 실행 관점 확인은 선택)**: Collector 쪽(C++, `SqliteMetricStore::Prune()`)을 실제로 띄워 24시간 대기 없이 즉시 확인하려면 `pruneTimer` 간격을 임시로 줄여서 `[SqliteMetricStore] prune 완료` 로그가 찍히는지 보는 정도 — 아직 미실행. Console 쪽(.NET, `RetentionService`)은 2026-07-29 실행 검증 중 **시작 즉시 전체 호스트를 크래시시키는 버그**(`DateTimeOffset` 비교가 EF Core+SQLite 조합에서 SQL 번역 안 됨)를 발견해 raw SQL로 수정 완료 — 상세: `SESSION_LOG.md` 2026-07-29 항목, `Docs/PROJECT_TECHNICAL_REVIEW.md` 버그 9.

**부수 발견(2026-07-26 기록, 2026-07-29 수정 완료)**: `APM_Console/src/ApmConsole.Host/appsettings.json`의 `ConnectionString`/키·인증서 경로가 옛 모노레포 경로(`/home/shkim/dev/gw2-cross/...`)로 남아있던 것 — 저장소 이관(2026-07-26) 이후 갱신 안 된 채 방치돼 있었음. `/home/shkim/dev/APM/...`로 수정하고 `APM_Console/certs/webserver.crt`/`.key`(이 저장소엔 없었음)를 `generate_webserver_cert.sh`로 새로 생성해 실제로 Collector+Console 연동까지 확인 완료.

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

**커밋 완료 (2026-07-26, `33ec155`)**: "Add load testing, alerting, data retention, and transaction tracing" — 1순위(LoadTester, 그동안 스테이징만 되고 커밋 안 된 상태였음)까지 함께 한 커밋으로 반영(사용자 결정: main.cpp 등 여러 순위가 같은 파일에 얽혀 있어 비대화형 세션에서 patch 단위 분리가 불가능해 단일 커밋으로 진행). 커밋 메시지는 AI 도구 언급/서명 없이 작성. `apm_metrics.db-journal`/`loadtest_results/`(런타임 산출물)와 `.vscode/`는 이번 커밋에서 의도적으로 제외 — 여전히 untracked 상태로 디스크에 남아있음(필요시 `.gitignore`에 `*.db-journal`/`loadtest_results/` 추가 고려). `CLAUDE.md`도 이번 작업과 무관해 손대지 않았으나 여전히 untracked 상태(원래 "커밋되어 있어야 할 프로젝트 지침 파일"인데 실제로는 추적 안 되고 있음 — 별도 확인 필요할 수 있음).

### 5순위 — 백분위/집계 통계 ✅ 코드 적용 + 빌드/테스트 검증 완료

확인 4건 확정: 대상 데이터 `TransactionSpans`만(Metrics는 이미 시계열 그래프 있어 제외), 계산 시점은 조회 시점(사전 집계 테이블 없음), 집계 시간 창은 사용자 선택(1시간/24시간/7일), 노출은 새 페이지 `/apm/traces`.

**설계 핵심**: SQLite에 `PERCENTILE_CONT` 같은 SQL 백분위 함수가 없어(PostgreSQL/TimescaleDB엔 있음) 시간 창으로 거른 span을 메모리로 가져와 C#에서 `GroupBy(OperationName)` + 정렬 + `PercentileCalculator`(선형 보간, `AlertEvaluator`와 같은 순수 로직 패턴)로 계산 — 백엔드 무관 통일. 4순위에서 이미 만든 `TransactionSpanRecord`의 `(OperationName, Ts)` 복합 인덱스가 이 쿼리에 그대로 맞아 **스키마 변경 없음**. 저장은 마이크로초(`DurationUs`)지만 화면엔 밀리초로 환산해서 표시. 설계 상세: `SESSION_LOG.md` 2026-07-26 여덟 번째 항목("5순위(백분위/집계 통계) 설계 제안").

**2026-07-26 적용 완료** — 사용자가 "바로 적용하자"로 명시 확인, 아래 6개 파일 실제 반영:
- 신규 5개: `Infrastructure/PercentileCalculator.cs`, `tests/.../Infrastructure/PercentileCalculatorTests.cs`, `Models/TracesViewModel.cs`, `Controllers/TracesController.cs`, `Areas/Apm/Views/Traces/Index.cshtml`
- 수정 1개: `Areas/Apm/Views/Dashboard/Index.cshtml`(트랜잭션 통계 페이지 링크 추가)

Agent/Collector 변경 없음(Console 쪽만 닫히는 작업 — C++ 재빌드 불필요).

**검증 완료(사용자, WSL, 2026-07-26)**: `dotnet build` → `Build succeeded, 0 Warning(s), 0 Error(s)`. `dotnet test` → `Passed: 18, Failed: 0`(기존 13건 + 신규 `PercentileCalculatorTests` 5건) — 예상대로 회귀 없이 통과.

**남은 선택 사항(코드/빌드 관점에선 5순위 완료, 실행 관점 확인은 선택)**: `/apm/traces` 페이지를 브라우저에서 직접 띄워 실제 트랜잭션 span 데이터(4순위 `Collector.HandleMetricPacket`/`AlertsController.Index` 계측분)로 시간 창 전환(1시간/24시간/7일)과 p50/p95/p99 표시가 의도대로 나오는지 확인 — 필수는 아님(2순위 `/apm/alerts` 시각 검증과 마찬가지로 아직 미실행 상태, 사용자 판단으로 뒤로 미뤄둔 항목들과 함께 나중에 일괄 확인 가능).

**실행 검증 완료(2026-07-29)** — 실제로 띄워보니 `/apm/traces`가 **항상 HTTP 500**이었음(2순위 `RetentionService`와 같은 근본 원인 — `DateTimeOffset` 비교가 EF Core+SQLite에서 SQL 번역 안 됨, `TracesController.Index()`의 `Where(s => s.Ts >= cutoff)`). `Ts` 비교를 SQL로 안 보내고 메모리에서 필터링하도록 수정 후 재검증: `window=1h/24h/7d` 전부 200, 1h 창에서 `LoadTester`가 만든 실제 `Collector.HandleMetricPacket` span 데이터가 표에 나타나는 것 확인. 상세: `SESSION_LOG.md` 2026-07-29 항목, `Docs/PROJECT_TECHNICAL_REVIEW.md` 버그 10.

이로써 로드맵 1~5순위 전부 코드/빌드 관점에서 완료. 남은 항목: 6순위(OpenTelemetry, 구현 안 함 - 면접 답변만 정리), 7순위(원격 명령 실행, 보류) — 그리고 미뤄둔 실행 관점 시각 검증들(2순위 알림, 5순위 트레이스, 1순위 부하 매트릭스 5단계).

### 6순위 — OpenTelemetry ⬜ 미착수

**구현하지 않음** — "왜 자체 프로토콜을 만들었는가"에 대한 면접용 답변 포인트만 정리.

### 7순위 — 원격 명령 실행 ⏸️ 보류 (2026-07-26)

**보류 사유**: 원래 전 직장에서 이 기능을 접하고 APM에 필수 기능이라 생각해 로드맵에 넣었으나, 실제로는 업계 필수 기능이 아님을 확인함(Zabbix "Remote commands"/Nagios "Event Handler" 같은 인프라 모니터링 계열엔 있지만, Datadog/Dynatrace/New Relic 같은 상용 APM 계열은 관찰과 조치를 분리해 원격 명령을 에이전트에 직접 두지 않는 경향 — 공급망 공격 벡터 우려 때문). 전 직장 기능과 유사하게 구현할 경우의 잠재적 마찰 가능성을 고려해 **최후순위로 보류, 착수 여부 자체를 추후 재검토**하기로 함. 착수하게 되면 Agent 미사용 수신 경로/Collector 세션 레지스트리 부재부터 채워야 함(위 "아키텍처 제약" 참고).
