# APM_Agent

경량 모니터링(APM/옵저버빌리티) 에이전트 + 수집 서버. 비동기 I/O 기반 다중 클라이언트 TCP 서버 코어(`GW2_CrossPlatformCore`, Standalone Asio 기반) 위에 TLS·페이로드 레벨 암호화·메시지 프레이밍·타입 안전 직렬화를 갖춘 통신 계층, 그리고 선택형 시계열 저장소를 갖추고 있다.

> 현재 상태: 수집(CPU/메모리/디스크/네트워크 대역폭/TCP 연결 품질) → 직렬화(Protobuf) → 암호화(AES-256-GCM) → 프레이밍 → TLS 전송 → 복호화·검증 → 저장(SQLite/TimescaleDB 선택형)까지 전체 파이프라인이 end-to-end로 동작·검증 완료. 여기에 더해 `Collector`가 저장과 별개로 **`APM_Console`(웹 대시보드)로 네트워크 전송까지 수행**한다(아래 "Collector → WebServer 전송" 참고) — 대시보드는 [`APM_Console/README.md`](../APM_Console/README.md) 참고.
>
> **2026-07-20**: Agent↔Collector 구간도 원래 쓰던 ARIA-256-CBC+HMAC-SHA256에서 AES-256-GCM으로 통일했다(Collector↔WebServer는 원래도 AES-GCM). ARIA-CBC+HMAC 구현·검증 경험 자체는 코드에서 빠졌지만 사라지지 않고 [`Docs/ARIA_TO_AES_MIGRATION.md`](../Docs/ARIA_TO_AES_MIGRATION.md)에 남겼다 — 마이그레이션 배경과 트레이드오프는 그 문서 참고.

---

## 아키텍처

```
수집 대상 시스템 (Agent)                             수집 서버 (Collector)
┌──────────────────────┐                           ┌──────────────────────┐
│  리소스 수집기          │                           │  타입 안전 디스패치     │
│  (CPU/메모리/디스크/     │                           │  (PacketHandler)      │
│   네트워크/TCP 품질)    │                           │        │             │
└──────────┬───────────┘                           └────────┼─────────────┘
           │  Protobuf 직렬화                                  ▼
           │  (apm::Metric)                          ┌──────────────────────┐
           ▼                                          │  선택형 저장소          │
     [PacketHeader{size,id}] ← 프레이밍                │ (SQLite/TimescaleDB) │
           │                                          └──────────────────────┘
           │  AesGcmPayload::Seal()                     ▲  AesGcmPayload::Open()
           │  (AES-256-GCM, AEAD)                        │  (복호화+태그 검증 동시)
           ▼                                             │
     ┌────────────────────── TLS (asio::ssl) ──────────────────────┐
     │              비동기 I/O 코어 (asio::io_context)               │
     └───────────────────────────────────────────────────────────┘
```

두 통신 주체 모두 `GW2_CrossPlatformCore`(OS별 비동기 I/O 메커니즘을 추상화한 크로스플랫폼 네트워크 계층)를 기반으로 하며, 그 위에 `APM_Agent` 전용 TLS 세션(`ApmSession`)이 메시지 프레이밍·페이로드 암호화·재연결/로컬 버퍼링(`ResilientSender`)까지 전부 처리한다. `Collector`는 수신한 메트릭을 `PacketHandler`를 통해 타입 안전하게 역직렬화한 뒤 선택된 저장소 백엔드에 저장한다.

---

## 보안 설계

### 1. 전송 계층 — TLS
`asio::ssl`(OpenSSL 백엔드)로 소켓을 감싸 도청·중간자 공격을 방지한다. 핸드셰이크 시 인증서 기반으로 대칭 세션 키를 안전하게 합의하고, 이후 실제 트래픽은 그 세션 키로 암호화된다(공개키 암호는 키 교환에만 쓰이고, 대량 데이터는 대칭키로 처리 — 상세는 `TLS_MECHANISM.md` 참고).

### 2. 페이로드 계층 — AES-256-GCM (AEAD)
TLS는 네트워크 구간만 보호한다. 민감 페이로드는 애플리케이션 레벨에서 한 번 더 암호화해 TLS 종단 이후에도 보호되도록 한다.

- **암복호화+인증**: `AesGcmCipher` — OpenSSL EVP API(`EVP_aes_256_gcm`)로 AES-256-GCM 처리. GCM은 AEAD(Authenticated Encryption with Associated Data)라 암호화와 동시에 인증 태그가 계산되고, 복호화 시 태그 검증에 실패하면 평문이 아예 나오지 않는다 — `SecurePayload`(아래 4번)처럼 "MAC을 손으로 먼저 검증"하는 코드 자체가 필요 없다.
- **결합**: `AesGcmPayload` — `[Nonce(12B)][ciphertext][Tag(16B)]` 와이어 포맷으로 결합.
- **구간별 키 분리**: `Agent`↔`Collector`, `Collector`↔`APM_Console` 두 구간이 각각 별도 키를 쓴다(아래 5번) — 같은 암호 방식이라도 한 구간 키가 유출되면 다른 구간까지 영향을 주지 않게.

### 3. Nonce 관리
매 암호화 호출마다 `RAND_bytes()`로 새 Nonce(96비트)를 생성한다. `AesGcmCipher::Encrypt()`는 Nonce를 출력 전용 파라미터로만 받기 때문에, 호출자가 Nonce를 직접 지정하거나 재사용할 방법이 구조적으로 없다 — GCM은 같은 키로 Nonce를 재사용하면 인증·기밀성이 동시에 깨지는 치명적 실수라, 재사용 자체를 코드 레벨에서 차단.

### 4. 왜 ARIA-256-CBC+HMAC에서 AES-256-GCM으로 통일했는가
원래 이 구간은 KISA(국내) 표준 블록 암호인 ARIA-256-CBC를 HMAC-SHA256과 Encrypt-then-MAC으로 결합해서 썼다(패딩 오라클 공격 대응까지 포함해 실제로 구현·검증까지 마쳤음). 이후 두 가지 이유로 AES-256-GCM 한 가지로 통일했다:

1. **구조적 안전성**: CBC는 기밀성만 제공해서 별도 무결성 검증(HMAC)을 순서까지 맞춰 직접 조합해야 하고, 순서를 실수하면 패딩 오라클 공격면이 그대로 열린다. GCM은 AEAD라 이 조합 실수 자체가 애초에 발생할 수 없는 구조다.
2. **단일 암호 경로**: 구간마다 다른 암호 방식을 쓰면 검증·유지보수 대상이 두 배가 된다(실제로 이 프로젝트에서도 AES-GCM 쪽엔 있던 자동 테스트가 ARIA 쪽엔 없는 비대칭이 있었다). 하나로 합치면 `AesGcmPayload`/`AesGcmCipher` 테스트 하나가 전체 페이로드 암호화 경로를 커버한다.

ARIA-256-CBC+HMAC 구현 자체(Encrypt-then-MAC 조합, IV 관리, 패딩 오라클 대응 검증 방법)는 `AriaCipher`/`HmacUtil`/`SecurePayload`(`APM_Agent/Common/`)에 코드가 그대로 남아 있고 — 실행 경로에서만 빠졌다 — 자세한 동작 원리와 마이그레이션 전 과정은 [`Docs/ARIA_TO_AES_MIGRATION.md`](../Docs/ARIA_TO_AES_MIGRATION.md)에 정리했다.

### 5. 대칭키 배포
AES 키는 TLS 인증서와 동일한 방식으로 로컬 파일을 통해 공유한다 — `Agent`↔`Collector`는 `certs/agent_collector_aes.key`(`certs/generate_agent_collector_key.sh`로 생성), `Collector`↔`APM_Console`은 `certs/webserver_aes.key`(`certs/generate_webserver_key.sh`로 생성). 둘 다 `.gitignore`에 포함되어 커밋되지 않는다. 대칭키라 양쪽이 사전에 동일한 비밀을 알아야 하는 구조적 한계가 있으며, 실제 운영 환경에서는 별도의 키 관리/교환 체계(KMS, 주기적 로테이션 등)가 필요하다 — 이 프로젝트의 범위는 로컬 파일 공유로 단순화했다.

---

## 통신 프로토콜 — 메시지 프레이밍 + Protobuf + 타입 안전 디스패치

TCP는 스트림 프로토콜이라 메시지 경계가 없다. 한 번의 `read`가 정확히 하나의 논리적 메시지와 대응한다는 보장이 없으므로, 이를 애플리케이션 레벨에서 직접 처리해야 한다.

- **프레이밍**: 모든 패킷 앞에 `PacketHeader{uint16 size, uint16 id}`를 붙인다. 수신 측은 바이트를 누적 버퍼에 쌓다가 `header.size`만큼 모이면 패킷 하나로 확정한다(`ApmSession::ProcessAccumulated()`).
- **직렬화**: 페이로드는 Protobuf(`apm::Metric` 등)로 직렬화한다. 패킷 ID는 수동 배정 대신 **Protobuf 리플렉션**(`PacketType::descriptor()->index()`, `.proto` 파일 내 메시지 선언 순서)으로 자동 결정 — ID 충돌/오타 같은 실수가 구조적으로 발생할 수 없다.
- **타입 안전 디스패치**: `PacketHandler::Register<T>(handler)`가 ID 결정과 역직렬화(`ParseFromString`)를 전부 자동으로 처리하고, 호출자는 이미 파싱된 강타입 메시지(`const apm::Metric&`)만 받는다. `ApmSession::SendPacket<T>()`/`ResilientSender::Enqueue<T>()`도 동일한 패턴으로 직렬화+ID 결정을 템플릿 진입점 하나로 캡슐화한다.

```cpp
// 전송(Agent) - 타입만 넘기면 ID 결정 + 직렬화 + 프레이밍 + 암호화가 전부 자동
sender.Enqueue(metricPacket);

// 수신(Collector) - 이미 파싱된 강타입 메시지를 받는 핸들러만 등록
PacketHandler::Register<apm::Metric>([](const apm::Metric& pkt) { store->Store(pkt); });
```

---

## 수집 지표

`ResourceCollector`가 `/proc` 기반으로 수집(Linux):

| 항목 | 소스 | 비고 |
|---|---|---|
| CPU 사용률 | `/proc/stat` | 두 시점 간 누적 jiffies 델타로 계산 |
| 메모리 사용량 | `/proc/meminfo` | `MemFree`가 아닌 `MemAvailable` 기준(회수 가능한 캐시를 반영) |
| 디스크 사용량 | `statvfs()` | |
| 네트워크 대역폭 | `/proc/net/dev` | `lo` 제외 전체 인터페이스 합산(인터페이스 이름이 환경마다 달라 이식성 확보), `steady_clock` 실측 경과시간 기준 바이트/초 |
| TCP 연결 품질 | `getsockopt(TCP_INFO)` | `Agent`↔`Collector` 연결의 RTT/재전송/혼잡윈도우를 커널에서 직접 조회. TLS 레이어 아래 순수 TCP 계층 정보라 암호화 여부와 무관하게 조회 가능 |

수집 주기는 `MetricScheduler`(`asio::steady_timer` 기반 비동기 반복)로 설정 가능.

---

## 데이터 저장 — 선택형 백엔드 (SQLite / TimescaleDB)

저장소는 `IMetricStore` 인터페이스로 추상화되어 있고, 실제 구현체는 **컴파일 타임 CMake 옵션**으로 선택한다(런타임 전환 아님):

```bash
cmake -B build -S . -DAPM_STORAGE_BACKEND=SQLite       # 기본값
cmake -B build -S . -DAPM_STORAGE_BACKEND=TimescaleDB
```

- **SQLite**: 파일 하나로 동작하는 임베디드 저장소. `TimescaleDB`를 선택하지 않으면 `libpq`(PostgreSQL 클라이언트 라이브러리)가 빌드에 전혀 링크되지 않는다 — Postgres/TimescaleDB가 시스템에 설치되어 있지 않아도 빌드·실행이 가능하다(`ldd`로 실측 확인됨).
- **TimescaleDB**: PostgreSQL 확장. 시계열 데이터에 특화된 압축·자동 파티셔닝(하이퍼테이블)을 제공한다. `GW2_CrossPlatformCore`가 이미 갖고 있던 libpq 래퍼(`DBConnection`)를 재사용해서 별도 구현 없이 연동.

두 백엔드 모두 컴파일 타임에만 하나가 선택되므로, 선택하지 않은 쪽의 의존성(라이브러리·헤더)은 결과물에 전혀 포함되지 않는다.

---

## Collector → WebServer 전송

`Collector`는 로컬 저장(위)과 별개로, 받은 메트릭을 주기적으로 **`APM_Console`(.NET 웹 대시보드)** 로도 전송한다. 처음엔 두 프로세스가 같은 장비의 SQLite 파일을 각자 다른 시점에 여는 방식이었으나, 이는 "같은 장비에 있다"는 암묵적 전제 위에서만 동작하는 구조라 실제 네트워크 관계로 바꿨다 — 서로 다른 장비에 있어도 동작한다.

- **트리거**: 주기적(`collector_config.json`의 `push_interval_seconds`, 기본 10초) + `Collector` 콘솔에 `send` 입력 시 즉시 전송. stdin은 별도 스레드로 읽고, 실제 전송은 `asio::post()`로 io_context 스레드에 넘겨서 공유 상태를 락 없이 안전하게 다룬다.
- **재연결 복원력**: `ResilientSender`가 연결이 끊기면 큐에 계속 쌓아두고, 재연결되는 즉시 순서대로 재전송한다 — `APM_Console`이 잠깐 꺼져 있어도 데이터가 유실되지 않는다(무한정 쌓이지는 않고, 프로세스 재시작 전까지의 메모리 큐 한도 내에서).
- **암호화**: **AES-256-GCM**(`webserver_aes.key`) — Agent↔Collector 구간과 같은 방식이지만 키는 별개(위 "대칭키 배포" 참고). `IPayloadSealer` 인터페이스(`Seal`/`Open`)로 `ApmSession`/`ResilientSender`를 구체 암호 방식에 고정되지 않게 일반화해뒀고, 지금은 두 구간 모두 `AesGcmPayload`를 주입하지만 세션 생성 시점에 다른 구현체(예: 남아있는 `SecurePayload`)를 주입해도 코드 변경 없이 그대로 동작한다. 상세 설계·수신 측 구현은 [`APM_Console/README.md`](../APM_Console/README.md) 참고.
- **설정**: `Collector/collector_config.json`(JSON — 차후 웹에서 설정을 편집할 가능성을 열어두기 위해 평문 대신 JSON 채택, `nlohmann/json` 벤더링):
  ```json
  {
      "webserver_host": "127.0.0.1",
      "webserver_port": 9100,
      "push_interval_seconds": 10
  }
  ```

**설계 노트 — 두 구간을 AES-256-GCM으로 통일한 이유**: 원래는 Agent↔Collector 구간에 KISA 표준 암호(ARIA-256-CBC+HMAC)를, Collector↔WebServer 구간엔 AEAD(AES-256-GCM)를 각각 다른 이유로 채택해 의도적으로 혼용했다. 이후 GCM의 구조적 안전성(패딩 오라클 공격면 자체가 없음)과 단일 암호 경로로 검증 부담을 줄인다는 이유로 Agent↔Collector도 AES-256-GCM으로 통일했다 — `IPayloadSealer` 추상화 덕분에 `ApmSession`/`ResilientSender`는 코드 변경 없이 주입되는 구현체만 바뀌었다. ARIA-256-CBC+HMAC을 구현·검증했던 경험과 마이그레이션 배경은 [`Docs/ARIA_TO_AES_MIGRATION.md`](../Docs/ARIA_TO_AES_MIGRATION.md)에 남겼다.

---

## 성능/권한 설계

### 최소 권한 원칙
`PrivilegeDrop`(Linux `setuid`/`setgid`) — 포트 바인딩처럼 특권이 필요한 초기화가 끝나는 즉시 비특권 사용자(`nobody`)로 권한을 하향한다. 순서(gid→보조그룹 제거→uid), 하향 후 `setuid(0)` 재시도가 실패하는지 검증하는 방어적 확인까지 포함. 프로세스가 침해당했을 때 공격자가 얻는 권한 범위를 최소화하는 것이 목적 — 지금은 포트가 9000(비특권 포트)이라 실질적으로 root가 필요 없지만, root로 기동되는 배포 환경을 가정한 방어적 설계.

### 커널 모드 전환(syscall) 최소화
Asio의 `epoll` 기반 이벤트 루프를 그대로 사용한다 — 연결마다 스레드를 만들거나 블로킹 read를 하는 대신, 이벤트가 준비될 때까지 `epoll_wait()`로 블로킹 대기한다(busy-polling 없음).

`Agent`를 `strace -c`로 감싸 **5분간(수집 주기 5초 × 약 60사이클) 정상 동작 상태**로 실측했다(2026-07-15) — 최초 접속 1건만 보던 이전 측정과 달리, 반복되는 수집→직렬화→암호화→전송 사이클의 정상 상태(steady-state) 비용을 보기 위함:

| 항목 | 횟수 | 비고 |
|---|---|---|
| `epoll_wait` | 155 | 이벤트 대기 — 스핀 없이 블로킹, 사이클당 약 2.6회 |
| `read` | 460 | TLS 소켓 수신 + `/proc` 파일 읽기 |
| `openat` | 311 | 대부분 `/proc/stat`·`/proc/meminfo`(x2)·`/proc/net/dev`를 매 사이클 새로 여는 비용(파일 디스크립터를 캐싱하지 않음 — 최적화 여지로 남겨둠) |
| `close` | 309 | 위 `openat`과 쌍을 이룸 |
| `sendto`/`write` | 76 / 77 | TLS 전송 |
| `timerfd_settime` | 149 | `MetricScheduler`/재연결 타이머(`asio::steady_timer`) 재설정 |
| `getsockopt` | 75 | `TCP_INFO` 조회(6-5) |
| `statfs` | 74 | 디스크 사용량 조회(`statvfs`) |
| 동적 라이브러리 로딩 등 1회성 비용 | ~50 | 프로세스 기동 시 1회(OpenSSL/libstdc++/Protobuf 로딩) |
| **전체** | **1921** | 총 syscall 소요 시간 약 19.4ms(5분 동안) — 압도적으로 대기(`epoll_wait`)가 지배적이고 실제 syscall 처리 시간은 무시할 수준 |

`/proc` 파일들을 매 사이클 새로 열고 닫는 게(`openat`+`close`가 합쳐서 전체의 32% 비중) 유일하게 눈에 띄는 반복 비용인데, 5초 주기에서는 절대 시간이 마이크로초 단위라 실질적 영향은 없다 — 다만 수집 주기를 초 단위 이하로 훨씬 좁힐 계획이 생기면 파일 디스크립터를 캐싱하는 최적화를 고려할 지점으로 기록해둔다.

### 리소스 사용량 실측
`Agent`를 5분간 정상 동작시키며 15초 간격으로 20회 샘플링(`/proc/[pid]/status`, `/proc/[pid]/stat` 기준, 2026-07-15):
- **메모리(RSS)**: 20회 샘플 전부 **13,716~14,156KB(약 13.8MB)로 완전히 고정** — 증가 추세 없음, 메모리 누수 없음을 실측으로 확인
- **CPU 사용량**: 5분(300초) 동안 누적 `utime`+`stime` 합계 5 tick(=0.05초, `HZ=100` 기준) — 평균 CPU 사용률 약 **0.017%**. 유휴 대기(`epoll_wait` 블로킹) 구간이 거의 전부라 busy-polling이 없음을 재확인
- 같은 구간 동안 `Collector`는 74건의 메트릭을 로그/값 일치 확인하며 정상 수신·저장(SQLite) — 파이프라인 전체가 장시간 동작에서도 안정적임을 함께 확인

## 기술 스택
- C++20, Standalone Asio (Boost 미사용)
- OpenSSL 3.x (TLS, AES-256-GCM — EVP/provider 기반 최신 API. ARIA-CBC/HMAC-SHA256도 코드는 남아있음 — `Docs/ARIA_TO_AES_MIGRATION.md` 참고)
- Protobuf (메시지 직렬화)
- SQLite3 또는 PostgreSQL/TimescaleDB (선택형 시계열 저장소)
- CMake

## 빌드
`Collector`(수집 서버)와 `Agent`(수집 대상에서 도는 에이전트) 두 개의 실행 파일을 만든다.

```bash
cd APM_Agent
bash certs/generate_test_cert.sh              # 최초 1회: TLS용 테스트 자체 서명 인증서 생성
bash certs/generate_agent_collector_key.sh    # 최초 1회: Agent↔Collector용 AES-256-GCM 대칭키
bash certs/generate_webserver_key.sh          # 최초 1회: Collector→WebServer용 AES-256-GCM 대칭키
                                               # (APM_Console과 공유해야 함 — 위 "Collector → WebServer 전송" 참고)
# generate_payload_keys.sh(ARIA/HMAC 키)는 기본 실행 경로엔 더 이상 필요 없음 — Docs/ARIA_TO_AES_MIGRATION.md 참고

cmake -S . -B build -DAPM_STORAGE_BACKEND=SQLite   # 또는 TimescaleDB
cmake --build build

./build/Collector   # 수집 서버 - 반드시 APM_Agent/ 안에서 실행 (certs/ 상대경로 때문)
./build/Agent       # 에이전트 - 별도 터미널에서, 마찬가지로 APM_Agent/ 안에서 실행
```

`APM_Console`(웹 대시보드)까지 함께 실행하는 전체 시나리오는 `Docs/WEBSERVER_TEST_SCENARIO.md` 참고.

---

## 테스트

`tests/AesGcmTests.cpp`(GoogleTest) — `AesGcmCipher`(저수준, 5개: 왕복 정상 복원/매 호출마다 다른 nonce/태그 변조 시 거부/암호문 변조 시 거부/다른 키로 복호화 불가)와 `AesGcmPayload`(와이어 포맷·`IPayloadSealer`, 4개: 왕복/`[Nonce 12B][ciphertext][Tag 16B]` 크기 검증/변조된 와이어 거부/인터페이스 경유 동작)를 검증한다. 총 9개, 전부 통과 확인(2026-07-20). Agent↔Collector 구간도 AES-256-GCM으로 통일되면서, 이 테스트가 실제 페이로드 암호화 경로 전체(두 구간 모두)를 커버한다 — `ARIA_TO_AES_MIGRATION.md`에 남아있는 `SecurePayload`(ARIA+HMAC)는 실행 경로에서 빠져 있어 별도 테스트를 추가하지 않았다.

빌드는 `APM_BUILD_TESTS` 옵션(기본 ON)으로 제어된다 — `find_package(GTest QUIET)`로 찾아서 있으면 `tests/`를 빌드하고, `libgtest-dev`가 없는 환경에서는 조용히 건너뛰어 `Collector`/`Agent` 빌드 자체는 막히지 않는다.

```bash
sudo apt install libgtest-dev   # 최초 1회
cmake --build build
ctest --test-dir build          # 또는 ./build/tests/APM_Common_Tests 직접 실행
```
