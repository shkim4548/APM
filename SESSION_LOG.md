# SESSION_LOG.md

> 설계 제안·코드 전문을 시간순으로 기록하는 로그.
> VSCode 확장 채팅 패널의 코드 블록 렌더링 품질 문제를 우회하기 위해 이 파일을 별도 탭으로 열어 참고하는 용도
> (원 모노레포 `gw2-cross/SESSION_LOG.md`와 동일한 목적으로 이 저장소에도 재도입, 2026-07-26).
> 최신 항목이 아래에 계속 추가(append)됨 — 위에서 아래로 시간순.

---

## 2026-07-26 — 저장소 이관 + 1순위(부하/스케일 테스트 툴) 설계 제안

### 배경

`gw2-cross` 모노레포에서 포트폴리오 공개용으로 분리한 새 저장소(`APM`, private)에서 후속 기능 개발을 시작. 클론/SSH 인증 확인(이상 없음), README 3종 + `PROJECT_TECHNICAL_REVIEW.md` 전반부를 읽고 현재 구현 상태 파악 완료. 로드맵 우선순위 확정(`WORK_STATUS.md` 참고), 첫 착수 항목은 1순위(부하/스케일 테스트 툴).

### 기존 코드 구조 파악

`APM_Agent/Agent/main.cpp`: `ResilientSender` + `MetricScheduler`로 5초 주기 `apm::Metric` 큐잉 전송.

`APM_Agent/Collector/main.cpp`: `PacketHandler::Register<apm::Metric>`로 저장 콜백 등록, `acceptor.async_accept`로 연결 수락, `ioContext.run()`을 **메인 스레드에서 단 한 번만 호출**(149번째 줄) — 별도 스레드 풀 없음. `GW2_CrossPlatformCore/Network/IoCore.cpp`도 동일 패턴(`_ioContext.run()` 단일 호출).

**핵심 발견**: Collector는 처음부터 "epoll 이벤트 루프 하나가 전부 처리"하는 구조다. 접속 수가 늘어도 소켓 자체의 다중화는 epoll이 효율적으로 처리하지만, 패킷 파싱·복호화(`AesGcmPayload::Open`)·SQLite 저장(`IMetricStore::Store`) 같은 CPU 바운드 작업은 전부 그 한 스레드에서 순차 처리된다. 이게 스케일의 실제 병목이 될 가능성이 높다 — 부하 테스트 결과가 이 가설을 검증해줄 1차 대상이다.

`ApmSession::Send()`의 `SendCallback(bool success)`(`ApmSession.h:55`)는 TCP 쓰기 완료 시점에 호출된다 — "enqueue → 전송 완료" 지연은 잴 수 있지만, "Collector가 실제로 처리를 끝냈는지"는 이 콜백만으로 알 수 없다(프로토콜에 애플리케이션 레벨 ACK가 없음 — 2순위 원격 명령 기능에서 양방향 통신을 만들 때 자연스럽게 열릴 여지).

`ResilientSender`(`ResilientSender.h:29`)의 `maxQueueSize` 기본값 100은 단일 Agent 기준 설계값(`FlushNext()`가 큐 맨 앞부터 순서대로 전송, 가득 차면 `pop_front()`로 오래된 것부터 버림 — `EnqueueRaw()`, `ResilientSender.cpp:77`). 부하 테스트에서 이 기본값을 그대로 쓰면 "Collector의 처리 한계"가 아니라 "Agent 큐 설계의 한계"를 측정하는 착시가 생길 수 있다 — LoadTester는 이 값을 크게 오버라이드해야 한다.

`Metric.proto`(`apm::Metric`): `cpu_usage_percent`(double), `mem_used_bytes`/`mem_total_bytes`/`disk_used_bytes`/`disk_total_bytes`/`net_rx_bytes_per_sec`/`net_tx_bytes_per_sec`(uint64), `tcp_rtt_us`/`tcp_rtt_var_us`/`tcp_retransmits`/`tcp_total_retrans`/`tcp_snd_cwnd`(uint32) 12필드. 부하 테스트에서 이 값들은 고정값/랜덤값으로 채워도 충분 — 측정 대상은 파이프라인 처리 능력이지 지표의 사실성이 아니다.

### LoadTester 설계 제안

**목적**: 동시 Agent 수를 늘려가며 Collector가 몇 개까지 버티는지, 처리량(msg/sec)·지연·에러율(큐 드롭, 재연결)이 어떻게 변하는지 측정 → README의 단일 에이전트 실측치(syscall 1,921회/19.4ms, RSS 13.8MB, CPU 0.017% — `APM_Agent/README.md` "성능/권한 설계" 절)를 N-agent 스케일 기준으로 확장.

**아키텍처(권장)**: `APM_Agent/LoadTester/`에 별도 실행파일을 신설하고, 실제 Agent가 쓰는 `ResilientSender`+`AesGcmPayload`+`apm::Metric`을 그대로 재사용해서 **하나의 io_context 안에 N개의 시뮬레이션 연결**을 생성한다.

- **왜 프로세스 N개 fork 대신 이 방식인가**: asio는 io_context 하나로 스레드 하나에서도 수천 개의 비동기 소켓을 다중화할 수 있다. 실제 Agent 바이너리를 N개 띄우면 OS 프로세스 오버헤드(컨텍스트 스위칭, 각자의 TLS/OpenSSL 초기화 비용)가 측정하고 싶은 "Collector의 처리 능력"과 무관한 잡음으로 섞인다. 기존 프로토콜 코드(`ResilientSender`/`ApmSession`)를 그대로 재사용하므로 Collector 입장에서는 진짜 Agent와 구분 불가능한 트래픽이다.

**파라미터(커맨드라인 인자)**:
- `--agents N`: 동시 시뮬레이션 Agent 수
- `--interval-ms`: 각 시뮬레이션 Agent의 전송 주기(실제 Agent 기본 5000ms보다 훨씬 짧게, 예: 100ms — 스트레스를 걸려면)
- `--duration-sec`: 테스트 지속 시간
- `--ramp-up-ms`: 0이면 전체 동시 connect(thundering herd 테스트), >0이면 이 시간 동안 스태거링 connect
- `--queue-size`: `ResilientSender`의 `maxQueueSize`를 크게 잡아 "Agent 큐 한계"가 "Collector 한계"로 오인되지 않게 함
- `--collector-host`/`--collector-port`: 기본 127.0.0.1:9000

**측정 지표(두 관점)**:
1. **LoadTester 자체 기록**: 연결 성공/실패/재연결 횟수, 초당 전송 성공 건수, enqueue→`Send` 완료 콜백까지의 지연시간 분포(p50/p95/p99), 큐 드롭 횟수
2. **Collector 프로세스 외부 관측**(기존 README 방법론과 동일): `strace -c`로 syscall 프로파일, `/proc/[pid]/status`·`/proc/[pid]/stat`으로 RSS/CPU — 별도 셸 스크립트로 Collector를 감싸서 LoadTester 실행 전/중/후 샘플링

**출력**: 1초마다 진행 상황(연결 수/누적 전송/최근 처리량) 콘솔 출력, 종료 시 요약 통계를 콘솔 표 + CSV 파일로 저장 — 이 수치를 이후 README/`PROJECT_TECHNICAL_REVIEW.md`에 "N-agent 스케일 실측"으로 반영하는 게 최종 목표.

### 결정 사항

- 상태 추적 파일 형식: 원 모노레포 관행(`WORK_STATUS.md` 요약 + `SESSION_LOG.md` 코드/설계 전문 로그) 그대로 재사용하기로 확정(2026-07-26). 파일명도 원본과 동일하게 유지.
- 이 로그 항목까지는 **설계 제안 단계** — 실제 코드 스켈레톤/파일 작성은 다음 단계에서 사용자 확인 후 진행.
