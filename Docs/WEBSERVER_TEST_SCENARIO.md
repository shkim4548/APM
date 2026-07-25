# Collector → WebServer 테스트 시나리오

`Agent → Collector(ARIA) → WebServer(AES-GCM) → SignalR` 전체 파이프라인을 직접 실행하고 검증하는 절차. Claude가 2026-07-19에 한 번 전 구간 검증을 완료했으나, 사용자가 직접 재현하며 확인하고 싶을 때 쓰는 문서.

---

## 0. 사전 준비물 (새 PC에서 처음 세팅할 때, 한 번만)

이 저장소를 새 머신(다른 PC/새 WSL 등)에서 처음 여는 경우, 아래 세 가지가 전부 있어야 합니다: **① OS 패키지(C++ 빌드용) ② .NET 8 SDK ③ 키/인증서**. 이미 세팅된 머신이면 이 절은 건너뛰고 바로 1번(빌드)로 가면 됩니다.

### 0-1. OS 패키지 (APM_Agent, C++ 빌드용)

```bash
sudo apt install build-essential cmake libssl-dev protobuf-compiler libprotobuf-dev libsqlite3-dev
```

| 패키지 | 왜 필요한가 |
|---|---|
| `build-essential`, `cmake` | 컴파일러/빌드 시스템 |
| `libssl-dev` | TLS(`asio::ssl`), ARIA/AES-GCM/HMAC(OpenSSL EVP API) |
| `protobuf-compiler`, `libprotobuf-dev` | `.proto` 파일을 `.pb.h`/`.pb.cc`로 컴파일(`protoc`) + 런타임 라이브러리 |
| `libsqlite3-dev` | 기본 저장소 백엔드(SQLite) |

`openssl` CLI(인증서/키 생성용, 아래 0-3)는 대부분의 배포판에 기본 설치되어 있음 — `openssl version`으로 확인, 없으면 `sudo apt install openssl`.

TimescaleDB 백엔드를 쓸 경우에만 추가로 `libpq-dev`(PostgreSQL 클라이언트)와 로컬 Postgres+`timescaledb` 확장이 필요함 — 기본값(SQLite)은 이게 전혀 없어도 됨. `nlohmann/json`(Collector의 JSON config 파싱)은 저장소에 이미 벤더링되어 있어(`APM_Agent/third_party/nlohmann/json.hpp`) 별도 설치 불필요.

### 0-2. .NET 8 SDK (APM_Console, .NET 빌드/실행용)

`dotnet` 명령이 없다면(`Command 'dotnet' not found`), OS가 추천하는 `snap install`(sudo 필요, 버전도 8.0이 아닐 수 있음) 대신 **사용자 디렉토리에 sudo 없이 설치**하는 공식 스크립트를 권장(정확히 8.0 채널을 지정할 수 있고, 이 프로젝트의 모든 `.csproj`가 `net8.0`을 타겟팅하므로 버전이 정확히 맞아야 함):

```bash
curl -sSL https://dot.net/v1/dotnet-install.sh -o dotnet-install.sh
bash dotnet-install.sh --channel 8.0 --install-dir "$HOME/.dotnet"
```

현재 터미널에 바로 적용:
```bash
export PATH="$HOME/.dotnet:$PATH"
export DOTNET_ROOT="$HOME/.dotnet"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1   # libicu 미설치 시 필요(sudo 없이 우회)
```

매번 새 터미널마다 다시 export하지 않으려면 `~/.bashrc`에 추가:
```bash
echo 'export PATH="$HOME/.dotnet:$PATH"
export DOTNET_ROOT="$HOME/.dotnet"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1' >> ~/.bashrc
```

설치 확인:
```bash
dotnet --version   # 8.0.x가 나와야 함
```

### 0-3. 키/인증서 확인

```bash
# 키/인증서가 이미 있는지 확인 (없으면 아래 명령으로 생성)
ls APM_Agent/certs/*.key APM_Agent/certs/*.crt
ls APM_Console/certs/*.key APM_Console/certs/*.crt
```

없는 파일이 있다면:
```bash
cd APM_Agent
bash certs/generate_test_cert.sh        # server.crt/server.key (Agent<->Collector TLS)
bash certs/generate_payload_keys.sh     # aria.key/hmac.key (Agent<->Collector 페이로드 암호화)
bash certs/generate_webserver_key.sh    # webserver_aes.key (Collector<->WebServer AES-GCM 키)
cd ../APM_Console
bash certs/generate_webserver_cert.sh   # webserver.crt/webserver.key (Collector<->WebServer TLS)
```
**주의**: 키를 이미 실행 중인 상태에서 다시 생성하면 기존 연결 상대와 키가 안 맞아서 통신이 끊깁니다. 처음 한 번만 하면 됩니다. 또한 이 키/인증서 파일들은 `.gitignore`로 커밋 대상에서 제외되어 있어서, **새 PC에는 저장소를 클론해도 자동으로 안 따라오고 매번 새로 생성해야 합니다**(Agent/Collector/WebServer가 전부 같은 키를 봐야 하므로, 한 대의 PC에서 생성한 뒤 나머지 컴포넌트도 같은 PC에서 실행하는 게 가장 간단함).

---

## 1. 빌드

```bash
# APM_Agent (C++)
cd APM_Agent
cmake -B build -S . -DAPM_STORAGE_BACKEND=SQLite
cmake --build build -j$(nproc)

# APM_Console (.NET)
cd ../APM_Console
dotnet build ApmConsole.sln
```
둘 다 **0 Warning(s), 0 Error(s)**가 떠야 정상입니다.

---

## 2. 실행 순서 (중요 — 반드시 이 순서로)

WebServer가 9100번 포트에서 먼저 듣고 있어야 Collector가 접속할 수 있습니다. 터미널 3개를 준비하세요.

### 터미널 1 — WebServer 먼저

```bash
cd APM_Console/src/ApmConsole.Host
dotnet run --urls "http://localhost:5299"
```

**정상이면 이렇게 뜹니다** (테이블 생성 SQL은 최초 1회만):
```
[ApmConsole.Host] 도메인 로드됨: Apm
[ApmConsole.Host] 도메인 로드됨: Game
...
[MetricsReceiverService] 9100번 포트에서 Collector 연결 대기 중 (TLS)
info: Microsoft.Hosting.Lifetime[14]
      Now listening on: http://localhost:5299
```

### 터미널 2 — Collector

```bash
cd APM_Agent
./build/Collector
```

**정상이면 이렇게 뜹니다**:
```
[PrivilegeDrop] 이미 비-root 권한으로 실행 중 - 하향 불필요
Collector listening on port 9000 (TLS)
[Collector] WebServer(127.0.0.1:9100)로 10초마다 전송 (콘솔에 'send' 입력 시 즉시 전송)
[ResilientSender] connected
[ResilientSender] TLS handshake complete
```
**여기서 `connected`가 아니라 `connect failed: Connection refused`가 반복되면 → 터미널 1(WebServer)이 안 떠 있거나, `APM_Console/src/ApmConsole.Host/appsettings.json`의 `Apm:ReceiverPort`(9100)와 Collector의 `collector_config.json`의 `webserver_port`(9100)가 다른지 확인하세요.**

### 터미널 3 — Agent

```bash
cd APM_Agent
./build/Agent
```

**정상이면 이렇게 뜹니다**:
```
[ResilientSender] connected
[ResilientSender] TLS handshake complete
[ResilientSender] queued (size=1)
```

---

## 3. 기본 동작 확인

터미널 2(Collector)에 5초마다 이렇게 찍히는지 확인:
```
[Collector] metric stored: cpu=...% mem=.../... disk=.../... net=rx:...B/s,tx:...B/s tcp=rtt:...us,...
```

10초마다(기본 `push_interval_seconds`) 이렇게 찍히는지 확인:
```
[Collector] WebServer로 N건 전송 시도
```

터미널 1(WebServer)에 그 직후 이렇게 찍히는지 확인:
```
[MetricsReceiverService] Collector 연결됨
info: Microsoft.EntityFrameworkCore.Database.Command[20101]
      Executed DbCommand (...) INSERT INTO "Metrics" (...)
[MetricsReceiverService] 저장 완료: cpu=...%
```

브라우저(또는 WSL이면 Windows 쪽 브라우저)로 접속:
```
http://localhost:5299/apm/dashboard
```
**2026-07-19부터 화면이 바뀌었습니다** — 예전엔 순수 표(table)뿐이었는데, 이제 카드 레이아웃 + 그래프 5개 + 상태 배지가 있는 스타일 적용된 화면입니다:
- 상단에 연결 상태를 나타내는 알약 모양 배지(연결 성공 시 초록색 "실시간 연결됨"으로 바뀜)가 보이면 정상.
- **그래프 카드 5개**(반응형 그리드로 배치)가 전부 실제 데이터로 그려져 있어야 함(빈 화면이면 `/lib/chartjs/chart.umd.js`가 제대로 로드됐는지 브라우저 개발자도구 네트워크 탭에서 확인 — 여기가 이번에 새로 생긴 임베디드 리소스 경로라 실패 가능성이 상대적으로 있는 지점):
  - CPU 사용률 추이
  - 메모리 사용률 추이 (%)
  - 디스크 사용률 추이 (%)
  - 네트워크 트래픽 추이 (수신/송신 B/s, 2라인)
  - TCP RTT 추이 (RTT/RTT 분산, 2라인)
- 그 아래 "최근 메트릭" 카드에 CPU/메모리/디스크/네트워크/RTT/RTT 분산/총 재전송/cwnd 값이 담긴 표(컬럼 10개)가 보이면 정상(placeholder 문구가 아님).
- 페이지를 새로고침하지 않고 가만히 두면, Collector가 다음 배치를 보낼 때(10초 주기) **표 맨 위에 새 행이 추가되고, 5개 그래프 전부에 새 점이 동시에 찍히는지** 확인 — 이게 SignalR 실시간 갱신입니다(표와 그래프 전부 같은 이벤트로 갱신됨).
- OS 다크모드 설정을 바꿔보면 배경/글자색이 자동으로 반전되는지도 확인해볼 수 있습니다(다크모드 지원 포함).

`http://localhost:5299/game/dashboard`도 같이 열어서 Apm 쪽 변경이 Game 도메인에 영향을 안 주는지(여전히 무장식 placeholder 화면) 확인하면 더 확실합니다.

---

## 4. 추가 시나리오 (선택)

### 4-1. CLI 즉시 전송 트리거
터미널 2(Collector)가 떠 있는 상태에서, 그 터미널에 직접 다음을 입력하고 Enter:
```
send
```
10초를 기다리지 않고 즉시 `[Collector] WebServer로 N건 전송 시도`가 뜨면 정상입니다.

### 4-2. WebServer 재시작 시 복원력
1. 터미널 1(WebServer)을 Ctrl+C로 종료.
2. 터미널 2(Collector) 로그에 `[ResilientSender] disconnected, will retry` → `connect failed: Connection refused`가 5초 간격으로 반복되는지 확인(죽지 않아야 함). 이 사이에도 Agent는 계속 데이터를 보내고, Collector는 큐에 계속 쌓아둡니다(`queued (size=N)`이 계속 늘어남).
3. 터미널 1을 다시 실행(`dotnet run --urls "http://localhost:5299"`).
4. Collector가 자동으로 재연결(`connected`)하고, 그동안 쌓인 데이터가 한꺼번에 전송되는지 확인.

### 4-3. 처음부터 다시 (DB 초기화)
누적된 테스트 데이터를 지우고 싶으면, WebServer를 끈 상태에서:
```bash
rm APM_Console/webserver_apm.db*
```
다시 `dotnet run`하면 빈 테이블로 새로 시작합니다(`EnsureCreated()`가 자동으로 스키마를 다시 만듦).

---

## 5. 뭔가 안 될 때 참고

| 증상 | 원인/확인할 곳 |
|---|---|
| Collector가 계속 `connect failed` | WebServer(터미널 1)가 안 떠 있음, 또는 포트/설정 불일치(`collector_config.json`의 `webserver_port` vs `appsettings.json`의 `Apm:ReceiverPort`) |
| WebServer 실행 즉시 예외로 죽음 | `appsettings.json`의 `Apm:ReceiverPort`/`WebServerAesKeyPath`/`ReceiverCertPath`/`ReceiverKeyPath` 4개 설정 중 하나라도 누락 |
| `/apm/dashboard` 500 에러 | (이미 수정됨) SQLite `DateTimeOffset` `ORDER BY` 문제 — `DashboardController.cs`가 `OrderByDescending(m => m.Id)`를 쓰는지 확인 |
| Collector 로그에 복호화 실패 | Collector의 `certs/webserver_aes.key`와 WebServer의 `appsettings.json`이 가리키는 키 파일 내용이 다름(대칭키라 반드시 일치해야 함) |
| 빌드 자체가 안 됨(.NET) | `dotnet build` 전에 `find . -name bin -o -name obj \| xargs rm -rf`로 클린 재빌드 시도 |

---

**참고 문서**: `APM_Agent/HOW_TO_RUN.md`(Agent/Collector 단독 실행), `APM_Console/HOW_TO_RUN.md`(WebServer 단독 실행 + Windows 검증 이력), `SESSION_LOG.md`의 "Collector→WebServer AES-256-GCM" 관련 항목들(설계 배경/버그 이력 전체).
