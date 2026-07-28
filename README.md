# APM Agent & Console

경량 모니터링(APM/옵저버빌리티) 에이전트와 실시간 웹 대시보드. 비동기 I/O 기반 다중 클라이언트 TCP 서버 코어 위에 TLS·AES-256-GCM 페이로드 암호화·메시지 프레이밍을 갖춘 수집 파이프라인(C++20)과, 그 데이터를 실시간으로 시각화하는 웹 대시보드(.NET 8)로 구성된다.

## 구성

| 디렉토리 | 역할 |
|---|---|
| [`GW2_CrossPlatformCore/`](GW2_CrossPlatformCore/) | OS별 비동기 I/O 메커니즘(Windows IOCP / Linux epoll)을 [Standalone Asio](https://think-async.com/Asio/)로 추상화한 크로스플랫폼 네트워크 코어 |
| [`APM_Agent/`](APM_Agent/) | 리소스 수집 에이전트(`Agent`) + 수집 서버(`Collector`). TLS + AES-256-GCM, Protobuf 직렬화, 메시지 프레이밍, SQLite/TimescaleDB 선택형 저장소. [자세히 →](APM_Agent/README.md) |
| [`APM_Console/`](APM_Console/) | `Collector`로부터 수신한 데이터를 저장하고 SignalR로 실시간 시각화하는 .NET 8 웹 대시보드. 도메인별 플러그인 아키텍처(Razor Class Library + `AssemblyLoadContext`). [자세히 →](APM_Console/README.md) |
| [`Docs/`](Docs/) | 포트폴리오 문서(`portfolio_apm.html`), 기술 리뷰(`PROJECT_TECHNICAL_REVIEW.md`), 보안/TLS 딥다이브 문서 |

## 검증된 것

- **파이프라인**: 수집(CPU/메모리/디스크/네트워크/TCP) → 직렬화(Protobuf) → 암호화(AES-256-GCM) → 프레이밍 → TLS 전송 → 저장(SQLite) → 실시간 대시보드까지 end-to-end 동작
- **크로스플랫폼**: Linux(WSL)뿐 아니라 Windows에서도 빌드+실행 검증 완료(`APM_Agent`) — WinAPI 기반 리소스 수집, `SIO_TCP_INFO`, Windows 11 Smart App Control 대응(정적 링크)까지 포함
- **테스트**: C++ GoogleTest 9개 + .NET xUnit 18개, 전부 통과
- **실측 성능(Agent 정상 동작)**: syscall 1,921회/19.4ms(5분), RSS 13.8MB 고정(누수 없음), CPU 평균 0.017%
- **스케일 테스트(Collector)**: 자체 제작 `LoadTester`로 최대 300 동시 연결까지 부하 실측 — 동시 접속 ~72개에서 하드 리밋 발견, `strace` 분석으로 원인이 SQLite 동기 `fdatasync`(단일 스레드 이벤트 루프를 블로킹)임을 확인(CPU/메모리는 병목 아님). 상세: `Docs/PROJECT_TECHNICAL_REVIEW.md` §7-4
- **스케일 병목 개선 + 재실측**: SQLite 저장 호출을 전용 워커 스레드로 분리해 100개 이하 동시 접속의 하드 리밋을 완전히 해소(전원 접속 성공, p95/p99 사실상 0ms). syscall 분석으로 SQLite 관련 호출이 네트워크 스레드에서 실제로 사라졌음을 확인. 300개 규모에서는 접속 성공이 71→229로 3배 이상 늘었으나, 이번엔 콘솔 로깅이 새 병목으로 드러남(후속 과제). 상세: `Docs/PROJECT_TECHNICAL_REVIEW.md` §7-5

자세한 아키텍처·설계 의사결정·발견한 버그는 [`Docs/PROJECT_TECHNICAL_REVIEW.md`](Docs/PROJECT_TECHNICAL_REVIEW.md) 또는 [`Docs/portfolio_apm.html`](Docs/portfolio_apm.html)을 참고.

## 빌드/실행

각 프로젝트의 `HOW_TO_RUN.md` 참고: [`APM_Agent/HOW_TO_RUN.md`](APM_Agent/HOW_TO_RUN.md) · [`APM_Console/HOW_TO_RUN.md`](APM_Console/HOW_TO_RUN.md)
