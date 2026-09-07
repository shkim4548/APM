# APM — Work Status

> 여러 환경(Windows PC / WSL)에서 작업 이어받기용 인수인계 문서.
> 작업 시작 전 반드시 이 파일을 확인하고, 완료/중단 시 업데이트할 것.
> 상세 설계안·코드 전문은 `Docs/SESSION_LOG.md` 참고 — 이 파일은 요약만 남긴다.
> **포트폴리오 PPT/문서화 작업을 다시 시작할 경우**: `DOCUMENTATION_PLAYBOOK.md`(재사용 가능한 헬퍼 카탈로그, 반복 검증 루프, 자주 겪은 함정과 해법 정리)를 먼저 읽을 것 — 아래 "부가 산출물" 절의 chronological 로그보다 실전에 바로 쓰기 좋다.

---

## 저장소 성격 (2026-07-26)

이 저장소(`https://github.com/shkim4548/APM`, private)는 원래 모노레포 `/home/shkim/dev/gw2-cross`에서 포트폴리오 공개 목적으로 `GW2_CrossPlatformCore`/`APM_Agent`/`APM_Console`/`Docs`만 추출해 새로 만든 것이다. 완성 전까지 private 유지, 이후 기능 추가는 이 저장소를 기준으로 진행한다. 원 모노레포의 `WORK_STATUS.md`/`Docs/SESSION_LOG.md` 관행(상태 요약 + 코드 전문 로그 분리)을 그대로 이어받는다.

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
8순위(신규 트랙) : Qt/MFC 포트폴리오 확장               🟡 진행 중(2026-09-07) — 0~1단계(빈 창+Signal/Slot) 빌드/실행 검증 완료, 2단계(SQLite) 착수 전
```

**8순위는 위 1~7과 독립된 별개 트랙이다** — APM 백엔드(1~7)는 완료 상태로 더 손댈 것이 없고, 여기에 Qt(신규)·MFC(기존 Viewer 보강)를 얹는 프론트엔드 작업을 새로 시작하는 것. 계획 전문은 `Docs/QT_MFC_PORTFOLIO_PLAN.md`.

---

## 작업 목록

### 1-7(신규) — Collector `SqliteMetricStore::Store()` 블로킹 개선 🔴 커밋된 버전이 크래시함 — WorkerQueue로 재설계 중, **세션 중단(2026-07-27)**

**⚠️ 다음 세션 시작 시 가장 먼저 확인할 것**: 현재 git에 커밋된 `Collector/main.cpp`(`39a3772` "Move Collector metric storage off the network thread via JobQueue")는 **실제 동시 접속 상황에서 시작 후 약 10초 만에 크래시하는 버그가 있는 버전**이다. 아래 "1-7-b" 절의 `WorkerQueue` 설계(이미 `Docs/SESSION_LOG.md`에 코드 전문 작성 완료, 아직 미적용)를 이어서 적용하는 게 최우선 작업.

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

**설계 확정, 코드 전문 작성 완료 — `Docs/SESSION_LOG.md` 2026-07-27 항목 참고**:
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

**설계 완료, 코드 전문 작성 완료 — `Docs/SESSION_LOG.md` 2026-07-27 두 번째 항목("1-7 재실측 중 크래시 발견 + `JobQueue` → 자체 `WorkerQueue` 전환 설계") 참고, 아직 파일로는 미반영**:
- 신규 `Common/WorkerQueue.h`/`.cpp` — `std::mutex`/`std::condition_variable`/`std::queue<std::function<void()>>`만 쓰는 단일 워커 스레드 큐(`SpanRecorder`와 같은 패턴). `GW2_CrossPlatformCore/Thread/*` 의존 완전 제거.
- `Common/CMakeLists.txt`에 `WorkerQueue.cpp` 한 줄 추가.
- `Collector/main.cpp`: 1-7에서 추가했던 9개 헤더(`CoreMacro.h` 등) + `<fstream>`/`<execinfo.h>` 우회 코드를 전부 제거하고 `#include "WorkerQueue.h"` 한 줄로 교체. `JobQueueRef metricStoreQueue = MakeShared<JobQueue>(); GThreadManager->Launch(...)` 블록을 `WorkerQueue metricStoreQueue;`(로컬 객체, 생성자에서 워커 스레드 자동 기동)로 교체. `PacketHandler::Register` 람다의 캡처를 `metricStoreQueue`(값 복사) → `&metricStoreQueue`(참조)로, `metricStoreQueue->Push(MakeShared<Job>(...), true)` → `metricStoreQueue.Push([...]{ ... })`로 교체.
- 부수 효과: `WorkerQueue` 소멸자가 큐를 다 비운 뒤 `join()`하므로, 1-7에 남겨뒀던 "워커 스레드 정상 종료 경로 없음" 캐치사항도 해소됨.

**2026-07-28 적용 완료** — 사용자가 "적용해줘"로 명시 확인, 아래 4개 파일 실제 반영:
- 신규 2개: `Common/WorkerQueue.h`/`.cpp`(단일 워커 스레드, `std::mutex`/`condition_variable`/`std::queue`만 사용, `GW2_CrossPlatformCore/Thread/*` 의존 제거)
- 수정 2개: `Common/CMakeLists.txt`(`WorkerQueue.cpp` 한 줄 추가), `Collector/main.cpp`(include 블록에서 9개 헤더 + `<fstream>`/`<execinfo.h>`/`<dbghelp.h>` 우회 코드 제거하고 `#include "WorkerQueue.h"`로 교체, `JobQueueRef metricStoreQueue = MakeShared<JobQueue>()` + `GThreadManager->Launch(...)` 블록을 `WorkerQueue metricStoreQueue;` 로컬 객체로 교체, `PacketHandler::Register` 람다 캡처 `metricStoreQueue`(값) → `&metricStoreQueue`(참조), `Push(MakeShared<Job>(...), true)` → `Push([...]{...})`로 교체)

적용 후 disk 상태가 `Docs/SESSION_LOG.md` 설계안과 일치함을 재확인 완료.

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

**사용자 결정**: 애초 제안한 "1순위(`std::endl`→`'\n'` + `sync_with_stdio(false)`만)" 대신, 사용자가 "2순위(로깅을 워커 스레드로 위임)를 먼저 적용하는 게 맞아 보인다"고 판단(사유: flush를 생략하면 메모리 버퍼에 문제가 생길 것 같다는 우려) → 이 우려는 정정(`std::cout` 버퍼는 고정 크기라 문제 없음)했으나, 정정 과정에서 "2순위를 `std::endl` 유지한 채 그대로 적용하면 `WorkerQueue` 내부 무제한 큐 적체로 실제 메모리 증가 리스크가 있다"는 진짜 리스크를 발견 → 2순위(워커 스레드 위임) + 1순위 일부(`'\n'`, `sync_with_stdio(false)`)를 결합하는 방향으로 확정. 상세: `Docs/SESSION_LOG.md` 2026-07-29 항목.

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

**영향 범위**: 콘솔 로그 텍스트 가독성 문제일 뿐 — 저장된 메트릭 데이터, `queue_drop`, 이미 측정한 syscall 레벨 지표(1-7-e 표)에는 영향 없음. 상세 원인 분석·수정 전/후 코드 전문(`Collector/main.cpp` 2곳)은 `Docs/SESSION_LOG.md` 2026-07-29 두 번째 항목 참고.

**수정 완료(2026-07-29)** — 네트워크 스레드에 남아있던 나머지 cout/cerr 호출 4곳(연결 수립, accept 에러, WebServer 전송 시도 메트릭/span)을 전부 `consoleLogQueue.Push(...)`로 위임 — 런타임 중엔 오직 `consoleLogQueue` 워커 스레드 하나만 스트림을 건드리도록 통일해 레이스 원천 차단(`sync_with_stdio(false)`는 유지, 단일 쓰기 스레드 하에선 안전).

**검증 완료(WSL, 2026-07-29)**:
- `cmake --build build` 성공, `ctest` 9/9 통과.
- 100-agent 스트레스(interval 50ms, ramp 2s, 10초)로 레이스 재현 확인 — 수정 후 81,381줄 전부 정상 접두어로 시작(깨진 줄 0건, 수정 전엔 진단 로그가 실제로 뒤섞이는 걸 확인했었음).
- 100-agent 표준 재실측: connect_success 100/100, p95/p99 0/2,053ms — 레이스 수정 전(2,049ms)과 사실상 동일, **레이스 수정이 성능 지표 자체는 안 바꿈**을 확인(스레드 배치만 정리, 총 작업량 불변).
- 300-agent `strace -f -c` 재검증: futex 57.78%(51.10s)/write 13.15%(11.63s)/전체 88.45s/connect_success 300/300, 로그 98,600줄 전부 정상 — 직전 1-7-e 보고값(futex 57.22%/49.26s, write 13.29%/11.44s, 전체 86.09s)과 오차범위 내 일치. **1-7-e 비교 표 수치는 그대로 유효, 갱신 불필요**.

**100-agent 기준 질문에 대한 결론**: WAL+로깅 개선은 100-agent 규모에선 1-7-b(WorkerQueue)에서 이미 해소된 하드 리밋(72→100)에 추가 이득을 주지 않음 — 오히려 p99가 소폭 늘어남(1,010ms→2,053ms), 워커 스레드가 1개→3개로 늘며 생기는 동기화 오버헤드로 보임(300-agent futex 경합 증가와 같은 패턴, 규모만 작을 뿐). 이 개선의 실질 효과는 300-agent 같은 고부하 구간(접속 성공 250→300)에 있음 — **100-agent를 기준으로 삼는다면 "이번 라운드 개선은 이 규모에선 순효과가 거의 없거나 근소하게 손해"가 정확한 결론**.

**커밋 완료(2026-07-29, `d664153` "Fix a console-log data race introduced by the previous logging fix")**.

**최종 결정(2026-07-29, 사용자 확정) — 이 스레드 종료**: 300-agent 시나리오에 남아있는 futex 경합(스레드 간 락 대기)은 **Collector 단일 프로세스의 처리 능력 한계 또는 테스트에 쓰는 서버 머신 자체의 스펙 미달**로 판단하고 더 파고들지 않기로 함. 실제 운영이라면 이 지점부터는 코드를 더 최적화하기보다 Collector를 여러 대로 수평 확장하는 게 정공법이라는 결론. `README.md`/`Docs/PROJECT_TECHNICAL_REVIEW.md`(신규 §7-7, 버그 8) 문서 반영 완료 — 이로써 **로드맵 1~7-f 전부 완료 처리, 프로젝트를 완료로 평가**(사용자 확정). 남은 건 미뤄뒀던 실행 관점 시각 검증(`/apm/alerts`, `/apm/traces`)뿐이며 이번 세션에서 문서화와 동시 진행.

**포트폴리오 문서(`Docs/portfolio_apm.html`) 갱신 완료(2026-07-29)** — `Docs/SESSION_LOG.md` 전체(2026-07-26~29, 1순위~1-7-f)를 다시 읽어 이관 이후 추가된 기능/서사가 전혀 반영 안 돼 있던 걸 확인하고 보강:
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
- 아티팩트 재게시 완료. **참고**: Claude 아티팩트는 단일 파일만 렌더링하는 self-contained 제약이 있어, 상대 경로 이미지가 아티팩트 미리보기에서는 안 보일 수 있음(레포 파일 자체를 열면 정상 렌더링) — 채팅에서 4장 다 직접 확인시켜드림.

**라이트 테마 버전 신규 추가(2026-07-29)** — 사용자 요청("현 파일은 그대로 두고 밝은 테마로도 받아보고 싶다")으로 `Docs/portfolio_apm_light.html` 신규 생성(원본 `portfolio_apm.html`은 무수정). `:root` 색상 토큰을 GitHub Light 계열 팔레트로 교체 + `var()`를 안 거치는 하드코딩 색상(코드 문법 강조 6곳, `rgba()` 틴트 8곳, 스크린샷 프레임 텍스처 1곳)까지 전부 수동으로 라이트 대응값으로 맞춤(다크 전용으로 튜닝돼 있어 그대로 두면 가독성이 깨졌을 부분들). Playwright로 hero/코드 블록/스케일 카드/스크린샷 섹션 전부 실제 렌더링 확인(문법 강조 대비, 배지 틴트, 버그·픽스 박스 전부 정상). 아티팩트 게시 완료(`https://claude.ai/code/artifact/55463050-53a4-45f5-bd0a-eb17030673e0`, 원본과 별도 URL). — 문서 전체(1259줄)를 다시 읽고 `PROJECT_TECHNICAL_REVIEW.md` §3~5(보안/프로토콜/저장) 나머지 부분까지 재확인, 구조 이상 없음(태그 균형/교차참조 방향/"게임" 네이밍 원칙 위반 없음) 확인 후 실제 갭 2건 반영:
- hero 태그에 알림/보존/트랜잭션 추적/백분위 통계 + 부하 테스트/strace 프로파일링 태그 추가 — 기존 태그가 전부 초기 파이프라인(암호화/저장/닷넷)만 나열해 스크롤 전 첫인상에서 새 기능/스케일 테스트가 안 보였음.
- 설계 결정 카드 `// 11`(Collector↔Console 관계 — JSON 설정 파일을 쓴 이유 + stdin 별도 스레드/asio::post로 락 불필요하게 만든 동시성 설계) 신규 추가 — §3-7 내용 중 유일하게 반영 안 돼 있던 부분.
- 아티팩트 같은 링크로 재게시 완료.

---

### 1순위 — 부하/스케일 테스트 툴 ✅ 실측 + 문서화 완료

| # | 작업 | 상태 | 메모 |
|---|---|---|---|
| 1-1 | 기존 코드 구조 파악 | ✅ 완료 (2026-07-26) | `Agent`/`Collector` `main.cpp`, `ApmSession`, `ResilientSender`, `PacketHandler` 확인. **발견**: `Collector`가 `ioContext.run()`을 메인 스레드에서 단일 호출(단일 스레드 io_context) — 접속 수가 늘어도 복호화/파싱/SQLite 저장은 한 스레드에서 순차 처리. 스케일 병목의 1차 가설. |
| 1-2 | LoadTester 아키텍처 설계 제안 | ✅ 완료 (2026-07-26) | in-process asio 다중 연결 시뮬레이터(별도 프로세스 N개 fork 대신), 기존 `ResilientSender`/`AesGcmPayload`/`apm::Metric` 재사용. 상세: `Docs/SESSION_LOG.md` 2026-07-26 항목 |
| 1-3 | 코드 스켈레톤(멤버 변수/함수 시그니처 전체) 제시 | ✅ 완료 (2026-07-26) | `LoadTester/` 신설안 + `ResilientSender` 콜백 추가안 제시. 상세: `Docs/SESSION_LOG.md` 2026-07-26 두 번째 항목. **사용자 확인 필요 4건 → 전부 추천안대로 확정 (2026-07-26)** |
| 1-4 | 구현 + 빌드 검증 | ✅ 완료 (2026-07-26) | `ResilientSender.h/.cpp` 수정 적용(선택적 `SendCallback`/`ConnectionStateCallback` 추가, `Agent/main.cpp`는 기본값 `nullptr`라 무변경). `APM_Agent/LoadTester/` 6개 파일 신규 작성 + `CMakeLists.txt`에 `LoadTester` 타겟 추가. **사용자가 WSL(Ubuntu, GNU 13.3.0)에서 `cmake --build build --target LoadTester` 빌드 성공 확인**(경고 없음 — `GW2_CrossPlatformCore`의 기존 `ASIO_STANDALONE` 재정의 경고만 있고 이번 변경과 무관). 코드 전문은 `Docs/SESSION_LOG.md` 2026-07-26 세 번째 항목 참고. |
| 1-5 | 실측 (N-agent 스케일 syscall/RSS/CPU/처리량/지연) | ✅ 완료 (2026-07-26) | 매트릭스 6단계(1/10/50/100/100+ramp-up/300 에이전트) 전부 실행 완료. **핵심 발견**: 동시 접속 성공 수가 71~72개에서 하드 리밋(100/300 요청 모두 동일) — ramp-up으로도 안 바뀜. 지연시간은 50 에이전트부터 절벽(p95 0ms→12초→34초). `queue_drop=0`(유실 없음, 그냥 밀림). CPU/메모리는 병목 아님(RSS 13~19MB 안정, CPU 평균 ~28%로 요청 규모 무관). `strace` 분석 결과 `pwrite64`/`fcntl`/**`fdatasync`**/`write`/저널 파일 관리(`openat`/`unlink`)가 시간의 70%+ 차지 — SQLite 기본 롤백 저널의 매 INSERT마다 동기 `fdatasync`가 네트워크 I/O와 같은 단일 `io_context` 스레드를 블로킹하는 게 근본 원인으로 확인됨(§1-1 가설을 구체적으로 검증). 상세: `Docs/SESSION_LOG.md` 2026-07-26 네 번째 항목 |
| 1-6 | README/`Docs/PROJECT_TECHNICAL_REVIEW.md`에 결과 반영 | ✅ 완료 (2026-07-26) | `README.md`에 스케일 테스트 요약 bullet 추가(+ `.NET xUnit 8개→18개`로 테스트 카운트 오탈자 수정, 5순위까지 반영 안 돼 있던 것 발견해 같이 고침). `Docs/PROJECT_TECHNICAL_REVIEW.md`에 신규 `7-4. Collector 스케일 테스트 — LoadTester로 병목 찾기` 섹션(실측 표 + 발견 4건 + 결론 + 알려진 개선 방향) + 예상 질문 2건 추가 — 기존 문서 스타일(배경/실측/발견/예상 질문) 그대로 따름. |

### 2순위 — 알림(임계치 기반) ✅ 코드 적용 + 빌드/테스트 검증 완료

`APM_Console` 쪽에서만 닫히는 작업(Agent/Collector 변경 불필요). 확인 4건 확정: 대상 지표 CPU/메모리/디스크/TCP RTT, 임계치는 DB 저장+UI 편집, 상태 전이 시만 알림(OK→Alert→Resolved, Zabbix/Nagios/Alertmanager 방식), DB에 이력 영속화(`AlertRecord`). 설계 상세: `Docs/SESSION_LOG.md` 2026-07-26 다섯 번째 항목("2순위(알림) 설계 제안").

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

저장소가 두 군데(Collector 로컬 `IMetricStore`/Console `ApmDbContext`)라 양쪽 다 대상. 확인 4건 확정: 적용 범위 Console+Collector 둘 다, 시간 기준 정책, Metrics 기본 30일, `AlertRecord`는 Metrics보다 길게(180일). 백엔드별로 구현 방식이 다름 — TimescaleDB는 하이퍼테이블 네이티브 `add_retention_policy()`(청크째로 드롭), SQLite는 직접 `DELETE` + `PRAGMA incremental_vacuum`, Console(EF Core) 쪽은 백엔드 무관하게 `ExecuteDeleteAsync` 하나로 통일. 설계 상세: `Docs/SESSION_LOG.md` 2026-07-26 여섯 번째 항목("3순위(데이터 보존 정책) 설계 제안").

**2026-07-26 적용 완료** — 사용자가 "바로 적용해줘"로 명시 확인, 아래 14개 파일 실제 반영:
- Console 신규 1개: `Infrastructure/RetentionService.cs`(매시간 `Metrics`/`AlertRecords` 정리)
- Console 수정 3개: `Infrastructure/Persistence/ApmDbContext.cs`(`MetricRecord.Ts`/`AlertRecord.ClosedAt` 인덱스), `ApmModule.cs`(`RetentionService` 등록), `appsettings.json`(`MetricsRetentionDays`/`AlertRetentionDays`)
- Collector 수정 10개(파일 기준): `Storage/IMetricStore.h`(`Prune()` 추가), `Storage/SqliteMetricStore.{h,cpp}`(`Prune()` 구현 + `auto_vacuum=INCREMENTAL`), `Storage/TimescaleMetricStore.{h,cpp}`(생성자에서 `add_retention_policy()` 등록, `Prune()`은 no-op), `Storage/MetricStoreFactory.{h,cpp}`(`retentionDays` 인자 추가), `Collector/CollectorConfig.{h,cpp}`(`metricsRetentionDays` 필드, `metrics_retention_days` JSON 키), `Collector/main.cpp`(`pruneTimer` 24시간 간격 신설)

**검증 완료(사용자, WSL, 2026-07-26)**:
- Console: `dotnet build` → `Build succeeded, 0 Warning(s), 0 Error(s)`.
- Collector: `cmake --build build` → `APM_Storage`/`Collector`/`Agent`/`LoadTester`/`APM_Common_Tests` 전부 빌드 성공. 컴파일된 오브젝트가 `SqliteMetricStore.cpp.o`뿐인 것으로 보아 현재 `APM_STORAGE_BACKEND=SQLite`로 빌드됨 — `TimescaleMetricStore.cpp`(네이티브 `add_retention_policy()` 경로)는 이번엔 컴파일 대상에 포함 안 됨, TimescaleDB 백엔드 전환 시 별도 컴파일 확인 필요.
- (참고: `APM_Agent`에서 `dotnet build` 실행 시 `MSB1003` 에러가 났던 건 정상 — `APM_Agent`는 C++/CMake 프로젝트라 `.sln`/`.csproj`가 없음, `dotnet build`가 아니라 `cmake --build`가 맞는 명령.)

**남은 선택 사항(코드/빌드 관점에선 3순위 완료, 실행 관점 확인은 선택)**: Collector 쪽(C++, `SqliteMetricStore::Prune()`)을 실제로 띄워 24시간 대기 없이 즉시 확인하려면 `pruneTimer` 간격을 임시로 줄여서 `[SqliteMetricStore] prune 완료` 로그가 찍히는지 보는 정도 — 아직 미실행. Console 쪽(.NET, `RetentionService`)은 2026-07-29 실행 검증 중 **시작 즉시 전체 호스트를 크래시시키는 버그**(`DateTimeOffset` 비교가 EF Core+SQLite 조합에서 SQL 번역 안 됨)를 발견해 raw SQL로 수정 완료 — 상세: `Docs/SESSION_LOG.md` 2026-07-29 항목, `Docs/PROJECT_TECHNICAL_REVIEW.md` 버그 9.

**부수 발견(2026-07-26 기록, 2026-07-29 수정 완료)**: `APM_Console/src/ApmConsole.Host/appsettings.json`의 `ConnectionString`/키·인증서 경로가 옛 모노레포 경로(`/home/shkim/dev/gw2-cross/...`)로 남아있던 것 — 저장소 이관(2026-07-26) 이후 갱신 안 된 채 방치돼 있었음. `/home/shkim/dev/APM/...`로 수정하고 `APM_Console/certs/webserver.crt`/`.key`(이 저장소엔 없었음)를 `generate_webserver_cert.sh`로 새로 생성해 실제로 Collector+Console 연동까지 확인 완료.

### 4순위 — 함수/트랜잭션 레벨 계측 ✅ 코드 적용 + protoc 재생성 + 빌드/테스트 검증 완료

"시스템 리소스 모니터링"과 "APM"의 정체성 갭을 메우는 항목. 확인 4건 확정: 계측 대상은 이 프로젝트 자체 코드(별도 데모 앱 없이 Collector/Console에 직접 삽입), 데이터 모델은 개별 span 원본 저장(5순위 백분위 정확도용), API 형태는 RAII 스코프 기반(C++ 소멸자/.NET `IAsyncDisposable`), 대상 언어는 C+++.NET 둘 다.

**설계 핵심**: `Metric.proto`에 `TransactionSpan` 메시지 추가(기존 메시지 뒤에 이어 붙임). Collector(C++) span은 기존 `Metric` 전송 파이프라인(`ResilientSender`/`ApmSession`)을 그대로 재사용해 네트워크로 전송(새 포트 불필요) — `ScopedSpan`(RAII) + `SpanRecorder`(전역 큐) 신설, `Collector.HandleMetricPacket`(메트릭 패킷 처리 핸들러)에 데모 계측. Console(.NET) span은 이미 자기 DB를 갖고 있어 네트워크 없이 `TraceScope`(`IAsyncDisposable`)가 직접 `ApmDbContext`에 저장 — `AlertsController.Index()`에 데모 계측. 패킷 ID 분기(`Metric.Descriptor.Index`/`TransactionSpan.Descriptor.Index`)가 새로 필요해져 `MetricsReceiverService`가 "id 무시하고 무조건 Metric으로 파싱"하던 걸 실제 분기하도록 바뀜. 새 테이블 `TransactionSpans`는 `RetentionService`가 `Metrics`와 같은 보존 기간으로 같이 정리하도록 확장.

상세 설계: `Docs/SESSION_LOG.md` 2026-07-26 일곱 번째 항목("4순위(함수/트랜잭션 레벨 계측) 설계 제안").

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

**설계 핵심**: SQLite에 `PERCENTILE_CONT` 같은 SQL 백분위 함수가 없어(PostgreSQL/TimescaleDB엔 있음) 시간 창으로 거른 span을 메모리로 가져와 C#에서 `GroupBy(OperationName)` + 정렬 + `PercentileCalculator`(선형 보간, `AlertEvaluator`와 같은 순수 로직 패턴)로 계산 — 백엔드 무관 통일. 4순위에서 이미 만든 `TransactionSpanRecord`의 `(OperationName, Ts)` 복합 인덱스가 이 쿼리에 그대로 맞아 **스키마 변경 없음**. 저장은 마이크로초(`DurationUs`)지만 화면엔 밀리초로 환산해서 표시. 설계 상세: `Docs/SESSION_LOG.md` 2026-07-26 여덟 번째 항목("5순위(백분위/집계 통계) 설계 제안").

**2026-07-26 적용 완료** — 사용자가 "바로 적용하자"로 명시 확인, 아래 6개 파일 실제 반영:
- 신규 5개: `Infrastructure/PercentileCalculator.cs`, `tests/.../Infrastructure/PercentileCalculatorTests.cs`, `Models/TracesViewModel.cs`, `Controllers/TracesController.cs`, `Areas/Apm/Views/Traces/Index.cshtml`
- 수정 1개: `Areas/Apm/Views/Dashboard/Index.cshtml`(트랜잭션 통계 페이지 링크 추가)

Agent/Collector 변경 없음(Console 쪽만 닫히는 작업 — C++ 재빌드 불필요).

**검증 완료(사용자, WSL, 2026-07-26)**: `dotnet build` → `Build succeeded, 0 Warning(s), 0 Error(s)`. `dotnet test` → `Passed: 18, Failed: 0`(기존 13건 + 신규 `PercentileCalculatorTests` 5건) — 예상대로 회귀 없이 통과.

**남은 선택 사항(코드/빌드 관점에선 5순위 완료, 실행 관점 확인은 선택)**: `/apm/traces` 페이지를 브라우저에서 직접 띄워 실제 트랜잭션 span 데이터(4순위 `Collector.HandleMetricPacket`/`AlertsController.Index` 계측분)로 시간 창 전환(1시간/24시간/7일)과 p50/p95/p99 표시가 의도대로 나오는지 확인 — 필수는 아님(2순위 `/apm/alerts` 시각 검증과 마찬가지로 아직 미실행 상태, 사용자 판단으로 뒤로 미뤄둔 항목들과 함께 나중에 일괄 확인 가능).

**실행 검증 완료(2026-07-29)** — 실제로 띄워보니 `/apm/traces`가 **항상 HTTP 500**이었음(2순위 `RetentionService`와 같은 근본 원인 — `DateTimeOffset` 비교가 EF Core+SQLite에서 SQL 번역 안 됨, `TracesController.Index()`의 `Where(s => s.Ts >= cutoff)`). `Ts` 비교를 SQL로 안 보내고 메모리에서 필터링하도록 수정 후 재검증: `window=1h/24h/7d` 전부 200, 1h 창에서 `LoadTester`가 만든 실제 `Collector.HandleMetricPacket` span 데이터가 표에 나타나는 것 확인. 상세: `Docs/SESSION_LOG.md` 2026-07-29 항목, `Docs/PROJECT_TECHNICAL_REVIEW.md` 버그 10.

이로써 로드맵 1~5순위 전부 코드/빌드 관점에서 완료. 남은 항목: 6순위(OpenTelemetry, 구현 안 함 - 면접 답변만 정리), 7순위(원격 명령 실행, 보류) — 그리고 미뤄둔 실행 관점 시각 검증들(2순위 알림, 5순위 트레이스, 1순위 부하 매트릭스 5단계).

### 6순위 — OpenTelemetry ⬜ 미착수

**구현하지 않음** — "왜 자체 프로토콜을 만들었는가"에 대한 면접용 답변 포인트만 정리.

### 7순위 — 원격 명령 실행 ⏸️ 보류 (2026-07-26)

**보류 사유**: 원래 전 직장에서 이 기능을 접하고 APM에 필수 기능이라 생각해 로드맵에 넣었으나, 실제로는 업계 필수 기능이 아님을 확인함(Zabbix "Remote commands"/Nagios "Event Handler" 같은 인프라 모니터링 계열엔 있지만, Datadog/Dynatrace/New Relic 같은 상용 APM 계열은 관찰과 조치를 분리해 원격 명령을 에이전트에 직접 두지 않는 경향 — 공급망 공격 벡터 우려 때문). 전 직장 기능과 유사하게 구현할 경우의 잠재적 마찰 가능성을 고려해 **최후순위로 보류, 착수 여부 자체를 추후 재검토**하기로 함. 착수하게 되면 Agent 미사용 수신 경로/Collector 세션 레지스트리 부재부터 채워야 함(위 "아키텍처 제약" 참고).

---

## 부가 산출물 — 라이트 테마 포트폴리오 기반 발표용 PPT (2026-07-29) ✅ 문서화 작업 우선 완료 처리(사용자 확정)

> 이 절은 무엇을 언제 했는지의 chronological 로그다. **재사용 가능한 노하우(헬퍼 함수, 검증 루프, 자주 겪은 함정)는 `DOCUMENTATION_PLAYBOOK.md`에 따로 정리해뒀으니 그쪽을 먼저 참고할 것.**

**배경**: `Docs/portfolio_apm_light.html`(라이트 테마 웹 포트폴리오)을 기반으로 발표용 PPT가 필요하다는 요청. 기존에 `Docs/build_portfolio_pptx.py`(2026-07-25, python-pptx, 13슬라이드)가 있었으나 그 이후(07-26~29) 작업 전부(부하/스케일 테스트 5라운드, 알림/보존정책/트랜잭션추적/백분위통계, 버그 스포트라이트 2건, 스크린샷 등)가 반영 안 된 구버전이라 이번 건에서는 참고하지 않고 **새로 작성**(사용자 명시 지시) — 단 라이트 테마 HTML 문서는 내용 근거로 계속 참조.

**1차 시도(Marp+LibreOffice) — 폐기**: `npx @marp-team/marp-cli`로 라이트 테마 팔레트를 그대로 재현한 Markdown 덱을 작성해 `--pptx --pptx-editable`(LibreOffice 경유)로 변환하는 경로를 먼저 시도. Chrome 렌더링(PNG 프리뷰)까지는 디자인이 정확히 재현됐으나, **LibreOffice의 HTML→PPTX 변환 단계에서 텍스트 런 경계의 숫자가 무작위로 소실되는 버그 발견**(예: "AES-256-GCM"→"AES- -GCM", "p95 34초"→"p 초", "Round 1/2/3/4" 제목 숫자 전부 소실, "229/300"→"/"). marp-cli 공식 문서가 이미 "EXPERIMENTAL, 재현성 보장 안 됨"이라 경고한 지점이 실제로 재현된 것 — 포트폴리오용으로 숫자가 틀린 채 나가는 건 용납 불가로 판단해 이 경로 폐기(`Docs/portfolio_apm_light_slides.md` 삭제 완료).

**환경 준비(사용자가 `!`로 직접 실행)**: `sudo apt-get install libreoffice`(1차 시도용, 결과적으로 폐기했지만 확인 과정에 필요), `sudo apt-get install poppler-utils`(pdftoppm, pptx→PDF 시각 검증용), `sudo apt-get install python3-pip` + `python3.12-venv`(python-pptx 설치용, Debian "externally-managed-environment" 제약으로 venv 필수).

**2차 시도(python-pptx 네이티브 생성) — 채택, 완료**: `Docs/build_portfolio_apm_light_pptx.py` 신규 작성(기존 `build_portfolio_pptx.py`는 구조/헬퍼 모두 미참고, 처음부터 새로 작성) — HTML→LibreOffice 변환 단계 자체가 없어 텍스트 손상 위험이 구조적으로 없음. 라이트 테마 색상 팔레트(`--accent:#0969DA` 등 GitHub Light 계열)를 그대로 이식, 폰트는 이 환경에 설치된 `Noto Sans CJK KR`/`Noto Sans Mono CJK KR`로 대체(원본 웹폰트 JetBrains Mono/Inter/Fira Code는 미설치). 16슬라이드 구성(타이틀 → 프로젝트 스탯 → 아키텍처 → 지표 수집 구현 → 설계 결정 하이라이트 2장 → 운영 기능 → 스케일 테스트 5라운드 개요+상세 2장 → 버그 스포트라이트 2건 → 실측 지표 → 실행 화면 → 기술 스택 → contact).

**검증**: `soffice --headless --convert-to pdf`로 변환 후 `pdftoppm`+Read 도구로 16장 전부 시각 확인. 1차 렌더에서 발견한 레이아웃 이슈 3건(제목 슬라이드 태그 2줄 배치 시 겹침, 통계 카드 값/라벨 간격 부족, 아키텍처 박스 텍스트 단어 중간 줄바꿈) 전부 수정 후 재검증 — 숫자/텍스트 손상 없음, 레이아웃 정상.

**산출물**: `Docs/build_portfolio_apm_light_pptx.py`(생성 스크립트), `Docs/portfolio_apm_light.pptx`(16슬라이드, 텍스트 편집 가능한 네이티브 PPTX). 둘 다 아직 git 미커밋 상태(커밋은 사용자 요청 시 진행).

**후속 수정 완료(같은 날, 2026-07-29)**:
- **최소 폰트 크기 10.5pt로 상향**: 사용자 요청으로 그동안 9~10pt로 쓰던 부분(태그 pill 텍스트, 페이지 번호, 카드 번호 라벨 `// 01` 등) 전부 10.5pt로 상향, 재검증 완료(겹침/넘침 없음).
- **아키텍처(3번) 슬라이드 재작성**: 사용자 지시("시스템 구조표는 technical_review에 있는 내용을 참조, 색을 굳이 맞출 필요는 없다")로 `Docs/portfolio_apm_light.html` 기반의 3색 박스(Agent=파랑/Collector=초록/Console=주황) 레이아웃을 폐기하고, `Docs/PROJECT_TECHNICAL_REVIEW.md` §1 "전체 파이프라인" 원문 다이어그램을 그대로 따르는 단일 무채색 블록(`lines_block()` 신규 헬퍼, 문단 단위로 줄바꿈 제어)으로 교체 + 기술리뷰 §1에만 있던 "Collector는 두 역할을 동시에 한다(수집 서버/중계자, 서로 독립적)" 인사이트를 콜아웃으로 추가. 박스 높이 부족으로 마지막 줄이 잘리는 문제 발견 후 `diagram_h`를 3.15in→4.3in로 조정해 재검증 완료(16컷 중 3번 슬라이드만 재확인, 나머지는 이번 변경과 무관해 미재검증).

**세션 중단 사유**: 사용자 사정으로 일시 중단. 코드/재생성/검증 자체는 위까지 문제 없이 완료된 상태.

**시각 완성도 개선(2026-07-29, 이번 세션)** — 사용자 피드백("PPT가 `Docs/PROJECT_TECHNICAL_REVIEW.html`에 비해 시각적으로 너무 떨어진다")에 따라, 두 문서를 실제로 렌더링해(Chromium headless 스크린샷 + soffice→pdftoppm) 나란히 비교 분석 후 구체적 격차 3건을 찾아 전부 수정:

1. **아키텍처 슬라이드(3번)가 텍스트 블록이었던 문제** — HTML은 Agent/Collector/Console 3개 색상 박스+화살표 다이어그램인데 PPT는 회색 카드 안 모노스페이스 텍스트 나열이었음. `python-pptx` 네이티브 도형(둥근 사각형 3개 + `RIGHT_ARROW` 커넥터 2개, 색상 코딩: Agent=파랑/Collector=빨강/Console=주황)으로 재작성(`arch_box()`/`arch_arrow()` 신규 헬퍼). 화살표 라벨("TLS(...)")이 박스 제목과 겹치는 1차 시도 버그를 발견해 라벨 전용 상단 밴드로 분리해 해결.
2. **버그 카드가 경고창처럼 보이는 문제** — HTML `bug-card`는 카드 배경은 무채색이고 왼쪽 3px 테두리만 빨강/초록인데, PPT `bug_fix()`는 카드 전체를 연분홍/연초록으로 채워 절제되지 않은 인상. 카드 배경을 `CARD`(연회색) 고정 + 왼쪽 6pt 컬러 바 방식으로 교체.
3. **모노스페이스 폰트가 헐렁해 보이는 문제** — `Noto Sans Mono CJK KR`은 라틴 문자도 한글 전각 폭으로 그려 영문 제목("Standalone Asio" 등)의 자간이 떠 보였음. `_script_segments()`/`_add_text_runs()` 신규 헬퍼로 문자열을 스크립트(한글/비한글) 단위로 쪼개 비한글 구간엔 `Noto Sans Mono`(비CJK, 타이트한 라틴 전용 폭)를, 한글 구간엔 기존 CJK 폰트를 쓰도록 `text()`/`multi_run()`/`lines_block()` 공통 경로에 적용 — 전체 슬라이드에 일괄 반영.

**검증**: `soffice --headless --convert-to pdf` + `pdftoppm`으로 16슬라이드 전부 재렌더링, Read 도구로 16장 전부 시각 확인 — 겹침/잘림/폰트 깨짐 없음. python-pptx는 스크래치패드에 새 venv(`/tmp/.../scratchpad/pptxvenv`)로 재설치해 사용(이전 세션 venv는 세션별 스크래치패드라 이미 사라짐 — 다음 세션에서도 다시 설치 필요할 수 있음, 재설치 명령: `python3 -m venv <path> && <path>/bin/pip install python-pptx`).

**실행 화면 스크린샷 삽입 + 터미널 이미지 대비 수정(2026-07-29, 같은 세션 이어서)** — 사용자가 지적한 2건:
1. **슬라이드 14("실제 실행 화면")에 사진이 하나도 없었음** — 텍스트 카드만 있고 실제 이미지가 삽입 안 돼 있던 걸 발견. `screenshot_card()` 신규 헬퍼(Pillow로 원본 비율 계산해 letterbox 방식으로 잘리지 않게 중앙 배치, `add_picture` crop 대신 contain 방식 선택 — 대시보드 차트가 잘리면 안 되므로) 추가, `Docs/screenshots/{dashboard,traces,alerts,terminal}.png` 4장 전부 실제 삽입.
2. **`Docs/screenshots/terminal.png` 가독성 문제** — 픽셀 실측(Pillow) 결과 로그 본문 텍스트 색(#30363D대)이 카드 배경(#1C2230)과 거의 같아 대비가 사실상 없었음(테마 자체는 유지, 대비만 문제). Chromium headless로 같은 다크 테마·같은 로그 내용을 텍스트 색만 고대비(#C9D1D9)로 바꿔 재렌더링해 파일 교체 — `Docs/portfolio_apm.html`/`portfolio_apm_light.html` 두 곳에서도 같은 파일을 참조하므로 함께 개선됨. 원본 대비 레이아웃/내용 변경 없음.

재검증: 16슬라이드 재렌더링, 슬라이드 14 확인 — 4장 전부 잘림/겹침 없이 표시, 터미널 이미지 텍스트 판독 가능.

**"설계 결정 하이라이트" 슬라이드(5·6번) 개선(2026-07-29, 같은 세션 이어서)** — 사용자 요청 2건:
1. 제목 "설계 결정 하이라이트" → "설계 결정"으로 변경.
2. 문제/판단/해결이 카드 하나에 라벨+텍스트로 계속 이어 붙던 걸, 각각 별도 테두리 박스(mini-card)로 분리 — 신규 `decision_card()` 헬퍼 추가(라벨 색상: 문제=회색/판단=파랑/해결=초록). 기존 `card()` 함수는 Round 1~4 슬라이드(9, 11)에서도 재사용 중이라 그대로 두고, 설계 결정 슬라이드 전용 함수를 새로 분리해 다른 슬라이드에 영향 없음.

재검증: 슬라이드 5/6 재렌더링(3단 분리 카드 정상), 슬라이드 9/11(기존 `card()` 재사용처) 회귀 없음 확인.

**"기술 스택" 슬라이드(15번) 개선(2026-07-29, 같은 세션 이어서)** — 사용자 요청 2건: (1) 각 기술 스택 그룹을 카드로 감싸고, (2) 하위 항목("·"로 구분돼 있던 한 줄 텍스트)을 각 카드 안에 미니카드로 배치. 신규 `tech_card()` 헬퍼 추가 — 항목 텍스트 폭을 추정해 카드 폭에 맞춰 자동 줄바꿈(flow-wrap)하는 미니카드 그리드로 렌더링, 카드 높이는 항목 줄 수에 따라 자동 계산. 5개 그룹을 2단 masonry(좌/우 컬럼 각자 누적 높이 추적)로 배치 — 그룹마다 항목 개수가 달라 카드 높이가 제각각이라 균일한 그리드 대신 컬럼별 독립 누적 방식 채택.

재검증: 슬라이드 15 재렌더링 — 5개 카드 전부 겹침/잘림 없이 표시, 미니카드 줄바꿈 정상.

**"실제 실행 화면" 슬라이드 2분할(2026-07-29, 같은 세션 이어서)** — 사용자 요청: 한 슬라이드에 4장 몰아넣지 말고, 슬라이드당 2장만 크게. 기존 슬라이드 14(2x2 그리드, 각 2.4in 높이)를 슬라이드 14~15 두 장(슬라이드당 2장, 각 5.0in 높이)으로 분할 — 1페이지는 dashboard+traces, 2페이지는 alerts+terminal. 제목에 "(1/2)"/"(2/2)" 추가(설계 결정 슬라이드와 같은 패턴). 이후 전체 슬라이드가 16→17장으로 늘어나 `page_num()` 기본 total을 16→17로 변경하고 기술 스택(16번)/Contact(17번) 페이지 번호도 갱신.

재검증: 17슬라이드 전체 재렌더링 — 실행 화면 2장 다 큼직하게(글자까지 판독 가능한 크기) 표시, 페이지 번호(14/17~17/17) 정상.

**8~12페이지("300 동시 접속" 라운드/버그 스포트라이트) 가독성 개선(2026-07-29, 같은 세션 이어서)** — 사용자가 "이 구간 전체적으로 마음에 안 드는데 방향성을 모르겠다"고 해서, 렌더링 재확인 후 근본 원인 진단(모든 슬라이드가 상단 55~60%에만 내용이 있고 하단이 텅 빔 + 5개 슬라이드가 전부 같은 레이아웃 반복 + 핵심 숫자가 문단 속에 묻힘) → 2안(여백 채우기 vs 라운드+버그 통합) 제시 후 사용자가 "여백 채우기(5페이지 유지)" 선택.

`dataviz` 스킬 로드 후 진행 — 차트에 쓸 2계열 카테고리 컬러(파랑/주황)를 `scripts/validate_palette.js`로 실제 검증(CVD ΔE 24.7, normal-vision ΔE 33.6, 전부 PASS)한 뒤 채택, 팔레트 감으로 고르지 않음.

- **슬라이드 8**: 표+콜아웃 아래 빈 공간에 라운드별 접속 성공률(%) 그룹 막대 차트 신규 추가(`bar_chart_two_series()`) — 100-agent/300-agent 두 계열, 알약형 막대+직접 라벨+범례.
- **슬라이드 9, 11**: Round 카드 2개 아래 빈 공간에 "이전→이후" 핵심 숫자를 큰 스탯으로 뽑아낸 카드 추가(기존 `stat_block()` 재사용 — 슬라이드 2/17과 같은 시각 언어라 일관성 유지).
- **슬라이드 10, 12**: 버그 카드 2개 아래 빈 공간에 실제 코드 스니펫 추가(`code_block()` 신규, 문법 하이라이트 — `Docs/portfolio_apm_light.html`에 이미 있던 이 버그의 실제 코드 발췌를 그대로 재사용, 새로 지어내지 않음). 1차 렌더에서 텍스트가 좌측 정렬 안 되고 오른쪽으로 밀리는 버그 발견 → `word_wrap=False`가 원인으로 판단해 `True`로 수정 후 재검증, 정상화.

재검증: 8~12번 슬라이드 재렌더링 — 5개 전부 슬라이드 하단까지 실제 콘텐츠로 채워짐, 슬라이드마다 시각적 형식이 달라짐(표+차트 / 카드+스탯 / 버그+코드)을 확인.

**4, 13페이지 표 폰트 크기 확대(2026-07-29, 같은 세션 이어서)** — 사용자 요청: 표 가독성 확보를 위해 폰트 크게. `table()` 함수에 `header_size`/`cell_size` 파라미터 추가(기본값은 기존과 동일 — 슬라이드 8의 Round 비교표는 그대로 유지, 요청 범위 아님). 슬라이드 4/13만 헤더 10.5→13pt, 셀 11.5→14.5pt로 키우고 `row_h`/`header_h`도 비례해서 늘림. 재검증: 두 슬라이드 다 겹침/잘림 없이 여유 있게 들어감(아래 빈 공간 여유 충분), 슬라이드 8은 영향 없음 확인.

**2, 7페이지 개선(2026-07-29, 같은 세션 이어서)** — 사용자 요청 2건:
1. 슬라이드 2("프로젝트 한눈에 보기") — 스탯 6개를 각각 카드(`CARD` 배경+`BORDER` 테두리)로 감쌈, `stat_block()`은 그대로 재사용.
2. 슬라이드 7("Operational Features") — 8~12페이지와 같은 "카드가 상단에만 있고 하단이 비는" 문제가 동일하게 있어 사용자에게 제안 요청받음: 카드마다 번호 킥커(`// 01`~`// 04`, 설계 결정 카드와 같은 패턴) + 얇은 구분선 + 실제 구현 세부사항 한 줄(`Docs/portfolio_apm_light.html`에 이미 있던 내용 재사용 — 알림 상태는 DB 재조회로 판단/`AlertEvaluator` 순수 함수 분리, `APM_TRACE_SCOPE` 매크로, 보존 기간 30일/180일, `PercentileCalculator` 선형 보간 등) 추가. 카드 높이 1.5in→2.15in로 확대.

재검증: 두 슬라이드 재렌더링 — 겹침 없음, 슬라이드 7은 여백이 상당히 줄고 카드마다 내용이 실제로 늘어남.

**9~12페이지 카드 내부 문장 분리(2026-07-29, 같은 세션 이어서)** — 사용자 요청: 카드 내부 긴 문장을 잘라 미니카드로. `decision_card()`(9,11번, Round 카드)와 `bug_fix()`(10,12번, 버그 스포트라이트)를 각각 수정 — 기존엔 한 카드 안에 긴 문단이 통으로 들어있었는데, 문장을 의미 단위(결과/원인, 문제/해결, 증상/원인, 판단/검증, 원인/발견 계기, 원칙/검증 등)로 쪼개 각각 라벨 달린 미니카드로 렌더링(`decision_card`와 같은 시각 언어).

**작업 중 발견해 같이 고친 버그 2건**:
1. 줄바꿈 추정이 실제로 넘침 — 미니카드 높이를 문자 수 기반(`len(text)/chars_per_line`)으로 추정하던 기존 방식이 한글 비중이 높은 문장에서 실제 렌더 폭을 과소평가해 텍스트가 카드 밖으로 잘림(슬라이드 10에서 최초 발견). 이미 파일에 있던 CJK/라틴 폭 구분 함수 `_text_width_in()`을 재사용하는 `_est_lines()`로 교체해 `decision_card`/`bug_fix` 둘 다 수정.
2. `decision_card`/`bug_fix`의 "해결" 계열 텍스트에 지정한 `SUCCESS`(초록) 색이 실제로는 적용 안 되고 있었음(`sub_line()`이 색을 무시하고 항상 회색 고정) — 직접 run을 만들도록 고쳐 초록 강조가 실제로 보이게 수정.

문장을 압축해 미니카드 4~6개 * 2카드 + 코드블록/스탯카드가 한 슬라이드(7.5in)를 넘지 않도록 줄 수를 계산기로 미리 검증한 뒤(각 문장 3~4회 반복 수정) 반영 — 슬라이드 10/12는 코드블록도 6→6줄, 4줄로 살짝 축약, 카드 간 여백도 0.2→0.12~0.15in으로 축소.

재검증: 9~12번 슬라이드 재렌더링 — 4개 전부 겹침/잘림 없음, 페이지 번호와도 안 겹침, "해결"/"검증" 텍스트 초록색 정상 표시.

**Collector 내부 구조 딥다이브 슬라이드 신규 추가(2026-07-29, 같은 세션 이어서)** — 사용자가 Agent/Collector/Console 3개 컴포넌트 구조도를 각각 넣고 싶다고 해서, 슬라이드 3(전체 구조)과 내용이 겹치고(특히 Collector "두 역할" 콜아웃과 중복) 17→20장으로 늘어나는 부담을 짚어 대안 3개(개요만 보강/Collector만 1장/3개 다) 제시 → 사용자가 **"Collector만 딥다이브 1장"** 선택.

신규 슬라이드 4(전체 17→18장, 이후 모든 페이지 번호 +1 재조정)로 "Collector 내부 구조 — 스레드 배치" 추가 — 8~12페이지(5라운드 스케일 개선) 서사가 실제로 어떤 스레드 배치로 귀결됐는지 시각화:
- 네트워크 스레드(메인, `asio::io_context`) — accept/TLS/디스패치, 저장·로깅은 `Push()`만 하고 즉시 리턴
- `metricStoreQueue` 워커(Round 2 신설) / `consoleLogQueue` 워커(Round 4 신설) — 아래로 향하는 화살표 2개로 연결
- 하단 콜아웃에 "공유 상태는 정확히 한 스레드만" 설계 원칙 + stdin 스레드(`asio::post()`로 네트워크 스레드에 위임) + 300-agent futex 트레이드오프(8~12p 연결)까지 요약

내용은 전부 `WORK_STATUS.md`/`PROJECT_TECHNICAL_REVIEW.md`에 이미 있던 실제 사실 재사용(지어낸 내용 없음). 슬라이드 3의 `arch_box()`/`RIGHT_ARROW` 대신 세로 배치라 `DOWN_ARROW` 신규 사용, 나머지는 기존 헬퍼(`arch_box`, `callout`) 재사용.

**전체 페이지 번호 재조정**: `page_num()` 기본 total 17→18, 신규 슬라이드 이후 모든 `# Slide N` 주석과 `page_num(s, N)` 호출을 +1씩 순차 조정(슬라이드 5~18).

재검증: 18슬라이드 전체 재렌더링 — 신규 슬라이드 4 겹침/잘림 없음, 앞뒤 슬라이드(3, 5, 9, 18) 페이지 번호 정확히 매칭 확인.

**남은 것**: `Docs/build_portfolio_apm_light_pptx.py`/`Docs/portfolio_apm_light.pptx`/`Docs/screenshots/terminal.png`(수정분) 전부 여전히 git 미커밋 상태 — 커밋은 사용자 요청 시 진행.

**다음 세션 할 일 메모 3건 — 착수 완료(2026-07-29, 이어지는 세션)**: 착수 전 애매했던 부분(번호 표기 대상, 8번 슬라이드 콜아웃 여부)을 `AskUserQuestion`으로 먼저 확인 후 반영.

**확인 결과**:
1. 킥커 스타일 → "//" 제거, 텍스트만 남기기로 확정.
2. "// 01" 번호 표기 → 실제로는 6,7,8번 슬라이드(설계 결정 카드 + Operational Features 카드) 내부의 "//n"을 "01. Standalone Asio 채택" 식으로 번호+제목 병합 형태로 바꾸라는 의미였음(애초 "기술스택 페이지"라 언급했던 건 이 카드들을 잘못 지칭한 것으로 확인, 기술 스택 페이지(17번) 카드 그룹 제목의 "// 네트워크 코어" 등은 이번 범위 아님 — 손대지 않음).
3. 8번 슬라이드 콜아웃 → 위 2번과 같은 요청이었음(별도 하이라이트 박스 신설 아님). 2,3,4,5번 슬라이드의 기존 초록 콜아웃 박스 푸터 이동은 별도로 "함께 진행" 확정.

**적용 완료** — `Docs/build_portfolio_apm_light_pptx.py` 수정:
- `title_block()` 호출 8곳(슬라이드 3,4,5,8,9,14,15/16,17)의 kicker 문자열에서 `"// "` 프리픽스 제거.
- 슬라이드 6,7 `decisions_p1`/`decisions_p2`의 `decision_card()` 호출: `num`(`"// 01"` 등)을 별도 렌더 대신 `f"{num}. {title_s}"`로 제목에 병합, `decision_card()`엔 `num=None` 전달(함수 자체는 무수정 — Round 카드(10,12번, `"// Round 1 — ..."` 형식)는 이번 범위 밖이라 그대로 유지).
- 슬라이드 8 `features` 루프: 마찬가지로 번호를 제목에 병합(`f"{num}. {t}"`), 번호 전용 텍스트 라인 제거하고 위로 당겨진 공간만큼 본문(`body`) 영역을 넓힘(0.7in→0.9in) — 여백 확보 부수 효과.
- 슬라이드 2,3,4,5의 `callout()` 4곳: 콘텐츠 바로 아래 가변 위치 대신 `Inches(6.95) - h`(슬라이드 하단 고정 앵커, 콜아웃 높이만큼 위로) 위치로 통일 — 페이지 번호(y=7.12)와 겹치지 않는 하단 여백에 자리.

**검증 완료**: 스크래치패드에 python-pptx venv 재설치(이전 세션 것은 세션별 스크래치패드라 소실, 재설치 명령은 위 참고) 후 재생성 → `soffice --headless --convert-to pdf` + `pdftoppm`으로 슬라이드 2~9, 14, 15, 17 전부 렌더링해 Read 도구로 직접 확인. 킥커 "//" 제거, "01. 제목" 병합 형식, 콜아웃 푸터 이동 전부 의도대로 반영됐고 겹침/잘림 없음(슬라이드 6,7은 번호 줄이 사라진 만큼 카드 높이가 줄어 하단 여백이 더 생김, 슬라이드 8은 본문 텍스트 공간이 넓어짐 — 둘 다 부수적 개선, 회귀 아님).

**남은 것**: `Docs/portfolio_apm_light.pptx` 재생성됨, `Docs/build_portfolio_apm_light_pptx.py` 변경분과 함께 여전히 git 미커밋 상태 — 커밋은 사용자 요청 시 진행.

**첫/마지막 페이지 퀄리티 개선 + 레포지토리 주소 추가(2026-07-29, 이어지는 세션)** — 사용자 지시: 지원 시작 시점에 레포지토리(`github.com/shkim4548/APM`)를 공개로 전환할 예정이라 마지막 페이지에 주소 추가.

- **슬라이드 1(타이틀)**: 기존엔 콘텐츠가 상단 1.7in 여백 아래 좁게 몰려 있고 하단 ~2.3in이 통째로 비어있었음. 콘텐츠 시작 위치를 0.85in으로 올리고, 태그를 고정 3줄(4+3+1개, 마지막 줄에 태그 1개만 외로이 남는 문제)이 아니라 폭 기준 auto-flow(다음 태그가 우측 끝을 넘으면 줄바꿈)로 재배치해 2줄로 자연스럽게 정리, 하단에 구분선 + `Docs/portfolio_apm_light.html` hero 섹션에 이미 있던(PPT엔 그동안 빠져 있던) `hero-stats` 4개(5/300/27/10)를 스탯 스트립으로 신규 추가해 여백을 실제 콘텐츠로 채움.
- **슬라이드 18(Contact)**: 기존엔 "이메일로 요청하면 공유" 문구였는데, 레포지토리가 공개로 전환되면 그 문구가 더 이상 맞지 않아 "GitHub에서 소스 코드 전체 확인 가능 + 이메일은 별도 문의용"으로 수정. 이메일 한 줄만 있던 것을 EMAIL/GITHUB 두 개의 카드(라벨+값)로 재구성, 하단 통계도 슬라이드 1과 같은 스탯 스트립 스타일로 통일(3개→4개, 300 추가)해 여백 개선 + 처음/마지막 페이지 시각적 수미상관.

재검증: `soffice --headless --convert-to pdf` + `pdftoppm`으로 두 슬라이드 재렌더링, Read로 확인 — 겹침/잘림 없음, 상하 여백 균형 개선 확인.

**남은 것**: 이번 변경분도 git 미커밋 상태로 누적 중 — 커밋은 사용자 요청 시 진행.

**PPT 전체 내용 검증 + 정정, README.md 최신화(2026-07-29, 이어지는 세션)** — 사용자 요청으로 18슬라이드 전체를 실제 소스 코드(`Lock.cpp`, `Collector/main.cpp`, 테스트 파일)와 `PROJECT_TECHNICAL_REVIEW.md`/`WORK_STATUS.md`/`portfolio_apm_light.html`/`git remote`까지 대조 검증.

**확정 오류 2건, 발견 즉시 수정**:
1. 슬라이드 8(Operational Features) 카드 순서(01~04)가 부제("순서대로 추가")와 달리 실제 추가 순서(WORK_STATUS 로드맵 2→3→4→5순위: 알림→보존정책→계측→백분위)와 어긋나 있었음 — 02(계측)/03(보존정책) 순서를 실제 순서에 맞게 교체.
2. 슬라이드 4·9가 "5라운드"라고 명시하는데 실제 라벨링된 "Round N"은 4개(슬라이드 9 표, 10, 12)뿐이라 처음엔 "4라운드"로 잘못 고쳤다가, `portfolio_apm_light.html`을 대조해 5번째 라운드("Round 5 — 검증하다 새 버그" = `sync_with_stdio` 데이터 레이스, PPT의 버그 스포트라이트 2와 동일 내용)가 원본엔 있고 PPT에만 라벨이 안 붙어있던 것임을 확인 — "5라운드" 표기를 되돌리고, 대신 슬라이드 13 제목에 "(Round 5)"를 붙여 라벨 누락을 해소.

**minor 수정 2건**: 슬라이드 4 콜아웃/부제의 페이지 참조(futex 논의는 실제로 9번 슬라이드에만 있는데 "8~12p"로 뭉뚱그려져 있던 것 → "9~12p"/"9~13p"로 좁힘), 슬라이드 9 차트 캡션·17번 기술스택 카드 제목에 남아있던 "// " 프리픽스 제거(지난 라운드의 킥커 정리가 이 두 곳엔 안 미쳤던 것).

**검증 완료**: `soffice`+`pdftoppm`으로 수정된 슬라이드(4,8,9,13,17) 전부 재렌더링해 겹침/잘림 없음 확인.

**그 외 27개 항목(모든 실측 수치, 코드 스니펫 2건, 설계 결정 카드 4개, 기술스택, GitHub 주소 등)은 전부 정확함을 확인** — 특히 버그 스포트라이트 1·2의 코드 스니펫은 실제 파일과 바이트 단위로 대조해 완전 일치 확인. 테스트 카운트 "27(C++9+.NET18)"은 실제 소스에서 `TEST(`/`[Fact]`/`[Theory]` 개수를 직접 세어 재확인(참고: `PROJECT_TECHNICAL_REVIEW.md` 자체의 §0/§10 요약 표는 여전히 예전 값 ".NET 8개"로 낡아있음 — PPT가 아니라 그 문서 쪽이 오차, 이번 세션에선 미수정).

**README.md 최신화**: `Docs/` 행에 라이트 테마 포트폴리오(`portfolio_apm_light.html`)와 발표용 PPT(`portfolio_apm_light.pptx`), 스크린샷 디렉터리가 빠져있던 것을 추가. 그 외 본문(검증된 것 섹션 수치·버그 참조)은 이미 최신 상태였음을 확인(추가 수정 불필요).

**남은 것**: 이번 수정분(`build_portfolio_apm_light_pptx.py`, `portfolio_apm_light.pptx`, `README.md`)도 git 미커밋 상태. `PROJECT_TECHNICAL_REVIEW.md`의 §0/§10 테스트 카운트 표(".NET 8개"로 낡음)는 이번엔 범위 밖으로 남겨둠 — 원하면 후속으로 정정 가능.

**`PROJECT_TECHNICAL_REVIEW.md` 테스트 카운트 정정 + 전체 커밋(2026-07-29, 이어지는 세션)** — 사용자가 "이어서 정정 후 커밋하자"로 확정, 위에서 범위 밖으로 남겨뒀던 §0/§10 표를 마저 정정:
- §0 "기술 스택 요약" 표: "xUnit(.NET) 8개" → "18개".
- §10 "테스트 전략": `.NET(APM_Console/tests/, xUnit) 8개` → `18개`, 실제 테스트 파일(`AlertEvaluatorTests.cs` 5개, `PercentileCalculatorTests.cs` 5개)에서 빠져있던 항목을 실제 테스트 메서드명 기준으로 추가.
- 같은 절의 "왜 이 두 개만 골랐나" 제목/본문 — 이제 테스트 영역이 2개(암호화, TCP 프레이밍)가 아니라 4개(+알림 상태전이, +백분위 계산)가 됐으므로 "왜 이 영역들만 골랐나"로 제목 변경, 본문에 새 두 영역(순수 함수·상태 전이 로직) 추가해 설명 일관성 유지.

**커밋 완료(2026-07-29, `eb03286` "Add a native-PPTX presentation deck and fix stale test counts")** — 이번 대화에서 다룬 전체 변경분 한 번에 커밋: `Docs/build_portfolio_apm_light_pptx.py`(신규), `Docs/portfolio_apm_light.pptx`(신규), `Docs/PROJECT_TECHNICAL_REVIEW.md`(테스트 카운트 정정), `Docs/screenshots/terminal.png`(이전 세션에 이미 대비 수정된 버전, 이번에 같이 커밋), `README.md`, `WORK_STATUS.md`. 커밋 메시지는 이 저장소 관례대로 AI 도구 언급/서명 없이 작성. 커밋 후 `git status` 클린 확인.

**README.md 재검증(2026-07-29, 이어지는 세션)** — 사용자가 "혹시 모르니 한번더" 요청, PPT 검증과 같은 강도로 재검증:
- 모든 파일/디렉터리 링크(`GW2_CrossPlatformCore/`, `APM_Agent/README.md`, `APM_Console/README.md`, `Docs/` 하위 문서 6종, `HOW_TO_RUN.md` 2개)와 `PROJECT_TECHNICAL_REVIEW.md` 섹션 참조(§7-4~7-7, 버그 8·9·10)가 실제로 존재하는지 파일시스템에서 직접 확인 — 전부 유효.
- 파이프라인 순서 서술("암호화 → 프레이밍 → TLS")이 `PROJECT_TECHNICAL_REVIEW.md` §1 다이어그램의 시각적 배치(프레이밍이 위에 먼저 그려짐)와 달라 보여 오류로 의심했으나, 실제 `ApmSession::Send()` 코드(`_payloadSealer->Seal()`을 먼저 호출해 크기를 얻은 뒤 `PacketHeader`를 구성)를 직접 확인해 README 쪽이 맞고 기술 리뷰 문서의 다이어그램 레이아웃이 오해 소지가 있을 뿐임을 확인(문서 수정은 하지 않음, 범위 밖).
- C++20/.NET 8 표기도 각각 `CMakeLists.txt`(`CMAKE_CXX_STANDARD 20`)/`.csproj`(`net8.0`)와 직접 대조해 확인.
- **수정 사항 없음** — 재검증 결과 README는 그대로 정확함.

**최종 결정(2026-07-29, 사용자 확정) — 문서화 작업 우선순위로 완료 처리**: 포트폴리오 PPT(라이트 테마 기반, 18슬라이드) + README/기술 리뷰 문서 정합성 검증까지 마무리되어, 사용자가 이 "부가 산출물" 트랙을 우선 완료로 판단. 로드맵 본편(1~7-f)은 이미 이전 세션에 완료 처리됐고, 이번 트랙(문서화·포트폴리오)까지 닫히면서 프로젝트의 코드/문서 양쪽 모두 마무리 상태. 남은 미착수 항목은 로드맵의 6순위(OpenTelemetry, 구현 안 함 확정)·7순위(원격 명령 실행, 보류 확정)뿐이며 둘 다 착수 여부 자체가 보류 상태로 다음 세션 확인 불필요.

---

### 1-7-b 크래시 근본 원인 재진단 + `Lock.cpp` 수정 (2026-08-05)

사용자가 직접 코드 분석 세션 중 1-7-b `LOCK_TIMEOUT` 크래시의 원인(`Lock::WriteUnlock()`)을 재검토 — "이미 여러 Windows 실시간 게임 서버에서 검증된 `JobQueue`+`Lock`이 처음부터 이 버그를 갖고 있었을 리 없다"는 사용자 가설을 원 모노레포(`../gw2/GW2_Server/GW2_ServerCore/Lock.cpp`)와 `diff`로 직접 검증.

**확정 원인**: OS API 차이가 아니라 **이관(포팅) 중 `WriteUnlock()`과 `ReadLock()`의 함수 본문이 뒤바뀐 복사·붙여넣기 실수** — 원래 `WriteUnlock()`의 해제 로직(`--_writeCount` + 0이면 `_lockFlag.store(EMPTY_FLAG)`)이 유실되고 그 자리에 `ReadLock()`의 본문이 들어갔으며, `ReadLock()` 자체는 통째로 사라짐(헤더 선언만 남아 지금까지 아무도 안 불러서 링크 에러 없이 숨어있었음). 상세 diff/코드 전문: `Docs/SESSION_LOG.md` 2026-08-05 항목.

**수정 완료** — `GW2_CrossPlatformCore/Thread/Lock.cpp`(APM 저장소 사본만, 사용자가 "우선 APM 아래에 있는 내용만 수정" 확정 — `../gw2` 원본 미수정): `WriteUnlock()`을 원본 로직으로 복원, `ReadLock()` 신규 복원.

**검증 완료**: `cmake --build build --target GW2_CrossPlatformCore Collector Agent` 빌드 성공, `ctest` 9/9 통과. 단, 현재 `Collector/main.cpp`는 `JobQueue`/`Lock`을 안 쓰고 `WorkerQueue`로 이미 대체된 상태라 **이 수정은 현재 런타임 동작엔 영향 없음** — 향후 `Thread/JobQueue`를 다시 쓸 경우를 위한 정합성 수정.

**남은 것**: `CODE_ARCHITECTURE.md` 반영 여부, 커밋 여부 사용자 확인 대기.

---

### 현재 작업 현황 (2026-08-26, 디스크 상태 직접 재확인)

**로드맵(위 표) 상태 변화 없음** — 1~7-f 전부 완료, 6/7순위는 여전히 미착수/보류 확정 상태 그대로. 2026-08-05 이후 이어지는 세션들도 로드맵 항목이 아니라 **완료 처리된 코드에 대한 사용자 직접 코드 분석 세션**(Console 수신 파이프라인, Console↔브라우저 통신까지 확장).

**`git status` 기준 미커밋 변경분 전체(2026-08-26 재확인, 2026-08-05 대비 변화 없음)**:
- `M SESSION_LOG.md` — Lock.cpp 항목(코드 전문 포함).
- `M WORK_STATUS.md` — 이 문서 자체.
- `M GW2_CrossPlatformCore/Thread/Lock.cpp` — `WriteUnlock()`/`ReadLock()` 수정(§1-7-b 재진단 항목 참고).
- `M APM_Agent/Agent/main.cpp` — 람다 서식 변경(한 줄 → 중괄호 개행, 로직 동일). **사용자 확인 완료(2026-08-26): 의도한 변경, 유지.**
- `?? CODE_ARCHITECTURE.md` — 사용자 직접 분석용 Q&A 기록 문서. 2026-08-05 이후 크게 늘어남 — 현재 §1~12까지 구성:
  §1 추천 순서, §2 Collector 생명주기, §3/§4 Agent↔Collector/Collector↔Console 패킷 구성, §5 프로세스 유지 방식 비교, §6 `APM_Common` 정적 라이브러리, §7 sender/scheduler 람다 이유, §8 WorkerQueue vs JobQueue, §9 Lock.cpp 이관 버그, §10 순서도(10-1~10-3), §11 Console 패킷 수신 상세(TcpListener/TLS, ReadExactAsync vs C++ 누적 버퍼 재조립 및 그 이유, id분기→복호화→저장→SignalR, 알림 엣지 트리거), §12 Console↔브라우저 통신(서버 렌더링 초기 로드 vs SignalR/WebSocket 실시간 갱신, WebSocket 탄생 배경).
- `?? CODE_ARCHITECTURE_flowchart.html` — 순서도 HTML. §10-1~10-4(구 §6-1~6-4에서 번호만 이동), §11(Console 패킷 수신 순서도 신규) 구성.
- `?? DOCUMENTATION_PLAYBOOK.md` — 이전 세션부터 미커밋 상태로 남아있던 것(변경 없음).

**커밋 여부**: 사용자 확인 완료(2026-08-26) — **아직 커밋하지 않음, 분석 세션이 더 이어질 수 있으니 나중에 한 번에 커밋**. 다음에 이 판단이 바뀌지 않는 한 매 세션 커밋 여부를 다시 묻지 않아도 됨.

**다음 세션 시작 시 확인할 것**: 없음 — 위 3건 전부 이번에 확정됨. 사용자의 원래 분석 목표(§10-3/10-4 메모): (1) 수집 항목별(CPU/메모리/디스크/네트워크/TCP) 개별 분석, (2) `APM_Agent/LoadTester/` 분석으로 이동은 여전히 유효.

---

### GitHub 원격 브랜치 불일치 발견 (2026-08-26, WSL 환경) — 원인 조사는 Windows PC 쪽에서 이어갈 것

사용자가 "커밋 하나가 push되지 못하고 LFS에 물려 있을 것"이라고 언급해 이 WSL 저장소에서 확인 — **이 환경에는 LFS 문제 자체가 없었음**:
- `.gitattributes`가 히스토리 전체에 한 번도 존재한 적 없음(LFS 필터 설정 없음), local/global/system git config 어디에도 `lfs.*` 없음, 이 머신엔 `git-lfs` 바이너리 자체가 미설치.
- 히스토리 전체에서 가장 큰 blob도 21.8MB(`APM_Agent/loadtest_results/.../collector_stdout.log`)로 GitHub 100MB 하드 리밋/50MB 경고 기준에도 안 걸림. 저장소 전체 크기도 17MB 수준.

**대신 발견한 실제 문제 — 로컬 `master`와 GitHub 원격이 완전히 갈라져 있음**:
- `git ls-remote origin` 기준 GitHub엔 `master` 브랜치가 아예 없고 **`main`만 존재**(HEAD도 `main`).
- `origin/main` 최신 커밋(`20e1822` "APM 2차 마무리")은 로컬에 캐시된 옛 `origin/master`(`2050b498`, 같은 커밋 메시지)와 **메시지는 같지만 해시가 다름** — 그 지점까지의 히스토리가 다시 쓰여(rewrite) `main`이라는 새 브랜치명으로 force-push된 것으로 보임(아마 다른 PC에서 큰 파일을 빼내려고 `git filter-repo`/`bfg`/LFS 마이그레이션 등을 수행한 흔적으로 추정).
- 그 결과 **로컬 `master`에만 있고 GitHub 어디에도 없는 커밋이 5개**: `43becab`(크래시/500 버그 수정) → `04971d2` → `f08cb64` → `81d0f97` → `eb03286`(전부 2026-07-29 문서화/포트폴리오 마무리 세션 내용, 위 로드맵상 완료 처리된 작업).
- `git merge-base eb03286 origin/main`이 공통 조상을 못 찾음(exit 1) — 두 히스토리가 동일 프로젝트의 연속임에도 blob/커밋 레벨에서 완전히 별개 계보로 갈라진 상태.

**사용자 판단(2026-08-26)**: "아무래도 다른 PC(Windows)에서 처리된 듯하다" — LFS 마이그레이션/히스토리 재작성 작업 자체는 Windows PC 쪽에서 이미 진행된 것으로 추정, 이 WSL 환경에선 재현/확인 불가.

**다음에 어느 환경에서든 이 문제를 다룰 때 확인할 것**:
1. Windows PC에서 `git status`/`git log --oneline -10`/`git remote -v`로 실제 어느 브랜치에 있고 원격과 관계가 어떤지 확인.
2. 로컬 `master`의 5개 커밋(`43becab`~`eb03286`)이 Windows PC나 GitHub `main` 어디에도 없다면, 그 5개 커밋의 변경사항을 유실 없이 `main` 위에 재적용(cherry-pick 등)하는 방법을 검토해야 함 — 히스토리가 이미 재작성된 상태라 단순 `git push origin master:main`은 안 될 가능성 높음(비-fast-forward).
3. GitHub 웹(`https://github.com/shkim4548/APM`)에서 브랜치 목록/커밋 이력을 직접 봐도 빠르게 확인 가능.

---

### 문서 재정리 + Qt·MFC 포트폴리오 계획 문서화 (2026-09-05)

**배경**: 트랙 1(네트워크+데스크톱 UI) 대응용 "Qt·MFC 포트폴리오 계획"을 채팅에서 초안으로 검토하던 중, 계획서의 "Collector에 직접 TCP로 붙을지" 결정 항목을 실제 코드(`Collector/main.cpp`, `APM_Console` 컨트롤러/SignalR 허브)로 재확인해보니 원래 초안의 A안/B안 구도가 부정확했음을 발견(Collector는 Agent 전용 암호화 리스너뿐이고 조회 포트가 없음, 반면 Console의 `/apm/hub/metrics` SignalR 허브는 표준 프로토콜이라 Qt가 코어 무수정으로 구독 가능). 이 발견을 계기로 채팅에만 있던 계획을 파일로 정리하기로 함.

**사용자 요청**: "session log docs 아래로 옮기고 포트폴리오 계획 검토부터 새로 작성하자" — 저장소 문서 재배치 + 계획 문서 신규 작성.

**적용 완료**:
- `SESSION_LOG.md`(루트) → `Docs/SESSION_LOG.md`로 이동(`git mv`, 히스토리 보존). CLAUDE.md(rule 6)/`WORK_STATUS.md`/`CODE_ARCHITECTURE.md`/`APM_Agent/HOW_TO_RUN.md`/`APM_Console/HOW_TO_RUN.md`의 상호 참조를 전부 `Docs/SESSION_LOG.md`로 갱신. `Docs/` 안쪽 문서(`PROJECT_TECHNICAL_REVIEW.md`, `ARIA_TO_AES_MIGRATION.md`, `TLS_SSL_FUNDAMENTALS.md`, `WEBSERVER_TEST_SCENARIO.md`)의 참조는 같은 디렉토리라 경로 접두어 불필요 — 미변경.
  - 이동 근거: `SESSION_LOG.md`는 CLAUDE.md rule 6 정의상 "채팅 코드 블록 렌더링 우회용 코드 전문 아카이브"라 `Docs/`의 다른 참고자료(`PROJECT_TECHNICAL_REVIEW.md` 등)와 성격이 같음. 반면 `CLAUDE.md`/`WORK_STATUS.md`/`DOCUMENTATION_PLAYBOOK.md`/`CODE_ARCHITECTURE.md`는 "새 세션이 가장 먼저 읽어야 할 진입점" 성격이라 루트에 그대로 둠.
- `Docs/QT_MFC_PORTFOLIO_PLAN.md` 신규 작성 — 채팅 초안 전체를 재구성해 파일로 옮김. §3-2("데이터 연결 방식")를 A안/B안 대신 3가지 경로 비교표로 전면 재작성(Collector 직접 TCP / SQLite 폴링 / Console SignalR 구독), 권장안을 "SQLite(초기 적재)+SignalR 구독(실시간 갱신)" 하이브리드로 확정. §6 공수 견적에 SignalR 구현 단계(4~6시간)와 배포판 구성 단계(2시간)를 추가해 총 견적을 3~4일→4~5일로 조정. §8에 "확인 필요" 항목 중 Collector 포트 구조 질문을 해결됨으로 표시.
- 코드 근거는 `Docs/QT_MFC_PORTFOLIO_PLAN.md`가 아니라 `CODE_ARCHITECTURE.md` §13(신규)에 기록 — 기존 관례(코드 분석 결과는 `CODE_ARCHITECTURE.md`, 계획/설계 판단은 별도 문서)를 그대로 따름.

**남은 것**: `Docs/QT_MFC_PORTFOLIO_PLAN.md` §8의 미확정 2건(Qt 코드 저장소 내 위치, 테스트 대상 최소 범위) — 실제 착수 전에 결정 필요. 커밋 여부 미확인(위 GitHub 브랜치 불일치 이슈가 해소 안 된 상태라 커밋 타이밍 사용자 판단 대기).

**같은 날 2차 수정 — 원칙 변경**: 사용자가 "원칙 하나는 제외해라, 커도 된다. 합격할만큼의 내용과 이론적 지식을 쌓는 것이 중요하다"고 지시 — `Docs/QT_MFC_PORTFOLIO_PLAN.md` §2의 "원칙 1(크게 만들지 않는다)"을 제거(구 원칙 2/3 → 원칙 1/2로 재번호). "면접에서 말할 판단 3~4개만 만들고 멈춘다"는 스코프 제한을 걷어내고, 대신 구현 범위를 넓히되 각 단계의 밑바탕 이론(Qt 이벤트 루프/메타오브젝트, signal/slot 내부 동작, MVC, WebSocket/SignalR 프로토콜, MFC 메시지 펌프 등)까지 설명 가능한 수준으로 쌓는 쪽으로 방향 전환. §6 공수 견적의 "2단계에서 끊어도 된다"는 문구와 §9 "시간 되는 만큼"이라는 표현도 같이 제거 — 4~5일 견적은 이제 "최소 바닥선"이라고 명시.

---

### 새 트랙 착수 — Qt/MFC 포트폴리오 확장 시작 방법 확정 (2026-09-06)

**사용자 확인**: "APM은 사실상 더 변경할 거리가 없고, 여기에 Qt, MFC를 붙이니까 사실상 새로운 개발계획이다" — 위 로드맵(1~7)과는 독립된 신규 트랙으로 다루기로 확정. 이전 세션들의 세부 Q&A 내용을 매번 다시 참조할 필요 없이, 이 문서(로드맵 표 + 이 절)와 `Docs/QT_MFC_PORTFOLIO_PLAN.md`만 보고 시작할 수 있도록 정리.

**시작 방법(확정)**:
1. 위 로드맵 표에 **8순위(신규 트랙)**로 등록 — 1~7 로드맵과 섞지 않음.
2. `Docs/QT_MFC_PORTFOLIO_PLAN.md` §8("남은 실행 준비")의 착수 차단 항목 2건을 이 자리에서 결정해 코딩 시작을 막지 않도록 함:
   - **저장소 내 위치**: `APM_QtDashboard/`(신규 최상위 디렉토리, `APM_Agent`/`APM_Console`과 나란히) — 기존 저장소 관례 그대로 따름.
   - **Qt 버전**: Qt 6.
   - (테스트 범위·공고 재확인 2건은 비차단 항목으로 남겨둠 — 진행하면서 결정)
3. 실제 코딩 착수는 `Docs/QT_MFC_PORTFOLIO_PLAN.md` §9 진행 순서의 1번부터: Qt 설치 확인 → Qt Creator로 `APM_QtDashboard/` 아래 빈 프로젝트 생성 → 빌드 확인(§6 0단계).

**다음 세션 시작 시 확인할 것**: `APM_QtDashboard/` 디렉토리가 아직 생성되지 않았다면 위 3번부터 이어서 진행. 생성돼 있다면 `Docs/QT_MFC_PORTFOLIO_PLAN.md` §6 표에서 어느 단계까지 완료됐는지 먼저 확인(코드 파일이므로 Claude가 직접 만들지 않음 — CLAUDE.md rule 2, 사용자가 직접 진행).

---

### 0~1단계(빈 창 + Signal/Slot) 빌드/실행 검증 완료 (2026-09-07)

**진행 내용**: 사용자가 `APM_QtDashboard/`에 0~1단계 코드(`main.cpp`, `MainWindow.h/.cpp`, `CMakeLists.txt`)를 직접 작성. 오타로 컴파일 실패 → 사용자의 명시적 요청("오타만 잡고")에 따라 Claude가 예외적으로 오타 4곳 직접 수정(`QMainWinodw`→`QMainWindow` ×2, 클래스 종료 세미콜론 누락, `clickeds`→`clicked`). `cmake --build .` 빌드 성공 확인, 이후 사용자가 직접 실행해 "Click me" 클릭 시 라벨 카운트 증가까지 검증 완료. 수정 전/후 코드 전문은 `Docs/SESSION_LOG.md` 2026-09-07 항목 참고.

**다음 세션 시작 시 확인할 것**: `Docs/SESSION_LOG.md`에 이미 준비된 2단계(`webserver_apm.db` 초기 데이터 표시 — `MetricsRepository`, `QTableWidget` 2개, CMakeLists Sql 컴포넌트 추가) 제안을 따라 사용자가 직접 코드 작성 → 빌드/실행 검증. 검증 항목 3가지(§SESSION_LOG "검증" 절 참고): Sql 드라이버 `find_package` 성공 여부, DB에 실제 행이 있을 때 테이블 표시 여부, DB 파일 없을 때 `Open()` 실패 처리 여부.

**커밋 완료(2026-09-07, `2a85702` "Add Qt/MFC portfolio track and code-analysis docs, fix a lock bug")** — 그동안 누적돼 있던 미커밋 변경분 전체를 한 번에 커밋: `Docs/SESSION_LOG.md` 이동(구 `SESSION_LOG.md`) 및 참조 갱신(`CLAUDE.md`/`HOW_TO_RUN.md` 2개), `GW2_CrossPlatformCore/Thread/Lock.cpp`(2026-08-05 `WriteUnlock`/`ReadLock` 버그 수정), `CODE_ARCHITECTURE.md`/`CODE_ARCHITECTURE_flowchart.html`/`DOCUMENTATION_PLAYBOOK.md`(신규), `Docs/QT_MFC_PORTFOLIO_PLAN.md`(신규), `APM_QtDashboard/`(0~1단계 스캐폴드), `APM_Agent/Agent/main.cpp`(람다 서식, 로직 무변경). `git add -A`로 스테이징 — `.gitignore`가 `build/`를 이미 걸러줘서 `APM_QtDashboard/build/`는 제외됨. **로컬 커밋만 완료, push는 하지 않음** — 위 "GitHub 브랜치 불일치" 절(로컬 `master` vs `origin/main` 히스토리 재작성/공통 조상 없음 이슈)이 아직 미해결이라 push 방법/타이밍은 별도 결정 필요.
