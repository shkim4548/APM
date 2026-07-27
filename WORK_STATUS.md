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
1-7(신규) : Collector Store() 블로킹 개선                🔴 커밋된 JobQueue 버전이 실사용 중 크래시 확인 — WorkerQueue로 재설계, 적용 대기 (세션 중단)
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

### 1-7-b — 재실측 중 크래시 발견 + `JobQueue` → 자체 `WorkerQueue` 전환 🔴 설계 완료, 적용 대기 — **세션 중단(2026-07-27)**

**무엇이 문제인가**: 위 1-7(JobQueue 버전, 이미 커밋됨 `39a3772`)을 실제로 검증하려고 1-5와 동일한 6단계 LoadTester 매트릭스를 재실행했더니, **6단계 전부 Collector가 시작 후 약 10초 만에 크래시**(`connect_fail`이 agents=10부터 이미 발생, agents=1도 뒤늦게 크래시 — `apm_metrics.db` 파일 하나 재사용하는 게 아니라 완전히 새로 뜬 프로세스가 매번 10초 만에 죽음). 즉 **재실측 6개 결과는 전부 무효** — 1-7의 실제 효과(72개 하드 리밋이 풀렸는지)는 아직 검증 안 된 상태.

**근본 원인**: `collector_stdout.log`에서 `[CRASH] cause=LOCK_TIMEOUT ... file=Lock.cpp line=52 func=WriteLock` 확인. `GW2_CrossPlatformCore/Thread/Lock.cpp`의 `Lock::WriteUnlock()`이 "소유 스레드가 언락할 때" 분기에서 `_writeCount`를 안 줄이고 `_lockFlag`의 무관한 하위 비트만 건드려, **락 소유자 ID 비트(`WRITE_THREAD_MASK`)가 영원히 안 지워짐**. 그 락을 처음 잡았다 놓은 스레드가 그 락을 영구 소유한 것처럼 남아, 다른 스레드가 나중에 같은 락을 잡으려 하면 10초 스핀 후 `CRASH("LOCK_TIMEOUT")`. `JobQueue`(`LockQueue` 내부에서 이 `Lock`을 씀)는 지금까지 `APM_Agent`에서 한 번도 크로스 스레드로 안 쓰였어서(1-7 이전엔 이 서브시스템 자체를 안 씀) 이 버그가 여태 안 드러났었고, 1-7에서 **네트워크 스레드가 `Push()`(락을 처음 잡음), 워커 스레드가 `Execute()`(같은 락을 다른 스레드에서 잡으려 시도)** 하면서 최초로 재현됨.

**사용자 결정(2026-07-27)**: `GW2_CrossPlatformCore/Thread/Lock.cpp`는 수정하지 않음 — "여러 프로젝트를 통해 이미 검증한 내용"이라는 판단. 대신 **`APM_Agent` 안에서 `JobQueue`를 대체할 자체 큐를 새로 만드는 방향**으로 확정.

**설계 완료, 코드 전문 작성 완료 — `SESSION_LOG.md` 2026-07-27 두 번째 항목("1-7 재실측 중 크래시 발견 + `JobQueue` → 자체 `WorkerQueue` 전환 설계") 참고, 아직 파일로는 미반영**:
- 신규 `Common/WorkerQueue.h`/`.cpp` — `std::mutex`/`std::condition_variable`/`std::queue<std::function<void()>>`만 쓰는 단일 워커 스레드 큐(`SpanRecorder`와 같은 패턴). `GW2_CrossPlatformCore/Thread/*` 의존 완전 제거.
- `Common/CMakeLists.txt`에 `WorkerQueue.cpp` 한 줄 추가.
- `Collector/main.cpp`: 1-7에서 추가했던 9개 헤더(`CoreMacro.h` 등) + `<fstream>`/`<execinfo.h>` 우회 코드를 전부 제거하고 `#include "WorkerQueue.h"` 한 줄로 교체. `JobQueueRef metricStoreQueue = MakeShared<JobQueue>(); GThreadManager->Launch(...)` 블록을 `WorkerQueue metricStoreQueue;`(로컬 객체, 생성자에서 워커 스레드 자동 기동)로 교체. `PacketHandler::Register` 람다의 캡처를 `metricStoreQueue`(값 복사) → `&metricStoreQueue`(참조)로, `metricStoreQueue->Push(MakeShared<Job>(...), true)` → `metricStoreQueue.Push([...]{ ... })`로 교체.
- 부수 효과: `WorkerQueue` 소멸자가 큐를 다 비운 뒤 `join()`하므로, 1-7에 남겨뒀던 "워커 스레드 정상 종료 경로 없음" 캐치사항도 해소됨.

**다음 세션에서 이어서 할 일(순서대로)**:
1. `SESSION_LOG.md` 2026-07-27 두 번째 항목의 코드 전문대로 4개 파일(신규 2 + 수정 2) 적용 — 사용자 확인("적용해줘") 필요, 아직 안 받음.
2. `cmake --build build` 전체 빌드 성공 확인.
3. `run_load_test.sh` 6단계 매트릭스(1/10/50/100/100+ramp5s/300) 재실행 — **이번엔 크래시 없이 끝까지 도는지가 1차 확인 사항**, 그다음 72개 하드 리밋이 실제로 풀렸는지/새로운 병목(메모리 백로그 등, 대화 중 논의함)이 보이는지 확인.
4. 결과를 1-5 베이스라인과 비교해 README/`PROJECT_TECHNICAL_REVIEW.md`에 반영할지 판단.
5. 그 이후에 WAL 재검토 이어가기.

**정리 필요한 산출물(다음 세션에서 처리)**:
- `APM_Agent/crash.log` — 이번 크래시가 만든 파일(untracked), 진단 끝났으니 삭제해도 됨(증거로 남기고 싶으면 유지).
- `APM_Agent/loadtest_results/agents_*_20260727_11*` (6개, 이번 세션의 무효 재실측 결과) — 크래시로 무효한 데이터라 실측 비교 시 참고하면 안 됨. 삭제하거나 "무효" 표시 후 보관.
- `APM_Agent/run_load_test.sh` — 실행 권한 비트만 변경됨(`chmod +x`, 내용 변경 없음), 커밋 대상.

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

이로써 로드맵 1~5순위 전부 코드/빌드 관점에서 완료. 남은 항목: 6순위(OpenTelemetry, 구현 안 함 - 면접 답변만 정리), 7순위(원격 명령 실행, 보류) — 그리고 미뤄둔 실행 관점 시각 검증들(2순위 알림, 5순위 트레이스, 1순위 부하 매트릭스 5단계).

### 6순위 — OpenTelemetry ⬜ 미착수

**구현하지 않음** — "왜 자체 프로토콜을 만들었는가"에 대한 면접용 답변 포인트만 정리.

### 7순위 — 원격 명령 실행 ⏸️ 보류 (2026-07-26)

**보류 사유**: 원래 전 직장에서 이 기능을 접하고 APM에 필수 기능이라 생각해 로드맵에 넣었으나, 실제로는 업계 필수 기능이 아님을 확인함(Zabbix "Remote commands"/Nagios "Event Handler" 같은 인프라 모니터링 계열엔 있지만, Datadog/Dynatrace/New Relic 같은 상용 APM 계열은 관찰과 조치를 분리해 원격 명령을 에이전트에 직접 두지 않는 경향 — 공급망 공격 벡터 우려 때문). 전 직장 기능과 유사하게 구현할 경우의 잠재적 마찰 가능성을 고려해 **최후순위로 보류, 착수 여부 자체를 추후 재검토**하기로 함. 착수하게 되면 Agent 미사용 수신 경로/Collector 세션 레지스트리 부재부터 채워야 함(위 "아키텍처 제약" 참고).
