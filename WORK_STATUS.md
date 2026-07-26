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
- Agent 쪽에 미사용 수신 경로, Collector 쪽에 세션 레지스트리 부재 — 2순위(원격 명령 실행) 착수 시 채워야 함.
- `Metric.pb.h`/`.pb.cc`는 Linux protoc 생성본이 커밋되어 있음. 새 메시지 타입 추가 시 Linux에서 재생성 후 커밋 필수(Windows vcpkg protobuf 버전 다름 — 재생성 금지).
- 저장소 백엔드는 컴파일 타임 선택(`APM_STORAGE_BACKEND=SQLite|TimescaleDB`). 현재 SQLite 스키마: `metrics` 단일 테이블(`SqliteMetricStore.cpp`).

---

## 로드맵 (우선순위 순, 2026-07-26 확정)

```
1순위 : 부하/스케일 테스트 툴                         🟡 설계 중
2순위 : 알림(alerting) + 원격 명령 실행 기능           ⬜ 미착수
3순위 : 데이터 보존 정책(retention)                    ⬜ 미착수
4순위 : 함수/트랜잭션 레벨 계측                         ⬜ 미착수
5순위 : 백분위/집계 통계                                ⬜ 미착수
6순위 : OpenTelemetry — 구현 보류, 면접용 답변 정리만  ⬜ 미착수
```

---

## 작업 목록

### 1순위 — 부하/스케일 테스트 툴 🟡 설계 중

| # | 작업 | 상태 | 메모 |
|---|---|---|---|
| 1-1 | 기존 코드 구조 파악 | ✅ 완료 (2026-07-26) | `Agent`/`Collector` `main.cpp`, `ApmSession`, `ResilientSender`, `PacketHandler` 확인. **발견**: `Collector`가 `ioContext.run()`을 메인 스레드에서 단일 호출(단일 스레드 io_context) — 접속 수가 늘어도 복호화/파싱/SQLite 저장은 한 스레드에서 순차 처리. 스케일 병목의 1차 가설. |
| 1-2 | LoadTester 아키텍처 설계 제안 | ✅ 완료 (2026-07-26) | in-process asio 다중 연결 시뮬레이터(별도 프로세스 N개 fork 대신), 기존 `ResilientSender`/`AesGcmPayload`/`apm::Metric` 재사용. 상세: `SESSION_LOG.md` 2026-07-26 항목 |
| 1-3 | 코드 스켈레톤(멤버 변수/함수 시그니처 전체) 제시 | 대기 | 사용자 확인 후 진행 |
| 1-4 | 구현 + 빌드 검증 | 대기 | 사용자의 "적용해줘" 이후 |
| 1-5 | 실측 (N-agent 스케일 syscall/RSS/CPU/처리량/지연) | 대기 | |
| 1-6 | README/`Docs/PROJECT_TECHNICAL_REVIEW.md`에 결과 반영 | 대기 | |

### 2순위 — 알림 + 원격 명령 실행 ⬜ 미착수

착수 시 임계치 기반 알림을 먼저, 이후 원격 명령 실행 — Agent 미사용 수신 경로/Collector 세션 레지스트리 부재를 먼저 채워야 함.

### 3순위 — 데이터 보존 정책(retention) ⬜ 미착수

### 4순위 — 함수/트랜잭션 레벨 계측 ⬜ 미착수

"시스템 리소스 모니터링"과 "APM"의 정체성 갭을 메우는 항목 — SDK/매크로 형태 계측 지점 고려.

### 5순위 — 백분위/집계 통계 ⬜ 미착수

### 6순위 — OpenTelemetry ⬜ 미착수

**구현하지 않음** — "왜 자체 프로토콜을 만들었는가"에 대한 면접용 답변 포인트만 정리.
