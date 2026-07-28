# PROJECT_TECHNICAL_REVIEW.md

> APM_Agent / APM_Console 프로젝트 복습 + 면접 준비용 기술문서.
> "이미 다 안다"가 아니라 "설명이 필요할 때 바로 펼쳐볼 수 있다"를 목표로, 배경 지식 없이도 따라올 수 있게 하나하나 풀어 썼다.
> 각 섹션 끝의 **예상 질문**은 실제로 나올 법한 질문과, 그 자리에서 바로 말할 수 있는 답변 포인트를 정리한 것.

---

## 목차

- [0. 프로젝트 한눈에 보기](#0-프로젝트-한눈에-보기)
- [1. 아키텍처 전체 흐름](#1-아키텍처-전체-흐름)
- [2. 크로스플랫폼 코어 — Standalone Asio](#2-크로스플랫폼-코어--standalone-asio)
- [3. 보안 설계](#3-보안-설계)
- [4. 통신 프로토콜](#4-통신-프로토콜)
- [5. 데이터 저장](#5-데이터-저장)
- [6. 수집 지표 구현 상세](#6-수집-지표-구현-상세)
- [7. 에이전트 성능/권한 설계](#7-에이전트-성능권한-설계)
- [8. APM_Console 아키텍처](#8-apm_console-아키텍처)
- [9. 발견한 버그 전체 (7건 상세)](#9-발견한-버그-전체-7건-상세)
- [10. 테스트 전략](#10-테스트-전략)
- [11. 예상 면접 질문 모음 (통합)](#11-예상-면접-질문-모음-통합)
- [12. 용어 미니 사전](#12-용어-미니-사전)
- [13. 라이브 데모 실행 순서](#13-라이브-데모-실행-순서)

---

## 0. 프로젝트 한눈에 보기

### 이게 뭔가
`APM_Agent`(C++)와 `APM_Console`(.NET)로 이루어진 경량 모니터링(APM/옵저버빌리티) 시스템. 세 개의 프로세스가 순서대로 연결된다:

```
Agent(수집 대상 시스템에서 실행) → Collector(수집 서버, 로컬 저장) → APM_Console(웹 대시보드)
```

- **Agent**: CPU/메모리/디스크/네트워크/TCP 연결 품질을 주기적으로 수집해서 Collector로 전송.
- **Collector**: Agent로부터 받은 데이터를 SQLite에 저장하고, 동시에 APM_Console로도 전송(재전송).
- **APM_Console**: Collector가 보낸 데이터를 받아서 자체 DB에 저장하고, 웹 브라우저에서 실시간 그래프/표로 보여줌.

### 왜 이 프로젝트를 만들었나
원래 이 저장소(`gw2-cross`)는 IOCP 기반 게임 서버를 크로스플랫폼으로 포팅하는 프로젝트로 시작했는데, 목표가 "APM/옵저버빌리티 개발자 포지션 지원용 포트폴리오"로 재정의되면서, 기존에 만들던 크로스플랫폼 네트워크 코어(`GW2_CrossPlatformCore`) 위에 이 APM 시스템을 새로 얹었다.

### ⚠️ 네이밍 원칙 — 면접에서 반드시 지킬 것
이 저장소의 내부 문서(`WORK_STATUS.md`, `SESSION_LOG.md`)는 실제 코드 추적용이라 "게임 서버"라는 표현을 그대로 쓰지만, **대외적으로(면접 포함) 설명할 때는 절대 "게임"이라는 단어를 쓰면 안 된다.**

- "게임 서버" → **"비동기 I/O 기반 다중 클라이언트 TCP 서버"**
- "게임 클라이언트 접속 처리" → **"다중 에이전트 동시 접속 처리"**
- 만들게 된 계기를 물어보면 → **"네트워크 프로그래밍과 동시성 제어를 깊게 익히고 싶어서 직접 구현했다"**로 통일해서 답한다.
- IOCP를 배울 때 쓰는 예제 대부분이 게임 서버라는 사실 자체는 문제 없다("표준 레퍼런스였다"는 프레이밍은 가능).

이건 검열이 아니라 **포지셔닝**이다. 면접관 입장에서 "게임 서버 만들었다"보다 "다중 클라이언트를 상대하는 비동기 TCP 서버 인프라를 설계했다"가 지원 직군(APM/옵저버빌리티)과 훨씬 직접적으로 연결된다.

### 기술 스택 요약
| 영역 | 기술 |
|---|---|
| 네트워크 코어(C++) | C++20, Standalone Asio(Boost 미사용) |
| 보안(현재) | TLS(OpenSSL) + AES-256-GCM(AEAD) |
| 보안(과거 구현 경험) | ARIA-256-CBC + HMAC-SHA256(Encrypt-then-MAC) |
| 직렬화 | Protobuf |
| 저장(Agent 쪽) | SQLite / PostgreSQL·TimescaleDB(컴파일 타임 선택) |
| 웹 대시보드 | .NET 8, ASP.NET Core MVC + Razor, EF Core, SignalR |
| 플러그인 구조 | Razor Class Library + `AssemblyLoadContext` |
| 테스트 | GoogleTest(C++) 9개, xUnit(.NET) 8개 |

---

## 1. 아키텍처 전체 흐름

### 전체 파이프라인
```
[ Agent ]                                              [ Collector ]
  ResourceCollector      ─ CPU/메모리/디스크/네트워크/TCP 수집
        │  Protobuf 직렬화 (apm::Metric)
        ▼
  [PacketHeader{size,id}]  ← 프레이밍
        │  AesGcmPayload::Seal() — AES-256-GCM
        ▼
  ── TLS (asio::ssl) ──                    ── 비동기 I/O 코어 (asio::io_context) ──
                                                              │
                                                    PacketHandler ─ 타입 안전 디스패치
                                                              │
                                                    IMetricStore ─ SQLite / TimescaleDB
                                                              │
                                                    ResilientSender ─ 재전송(재연결 큐잉)
                                                              │  AesGcmPayload::Seal() — AES-256-GCM
                                                              ▼
                                                  ── TLS (Collector↔Console 전용 인증서) ──

[ APM_Console — ApmConsole.Host (.NET 8) ]
  MetricsReceiverService (TcpListener + SslStream)
        │  PacketHeader 파싱 → AesGcm 복호화 → Metric.Parser.ParseFrom
        ▼
  ApmDbContext (SQLite, webserver_apm.db) 저장
        │
  MetricsHub (SignalR) 실시간 브로드캐스트
        ▼
  브라우저 /apm/dashboard — Chart.js 그래프 5개 + 실시간 표
```

핵심은 **Collector가 두 가지 역할을 동시에 한다**는 것: (1) Agent로부터 받은 데이터를 로컬 SQLite에 저장하는 "수집 서버" 역할과, (2) 저장한 데이터를 다시 APM_Console로 넘겨주는 "중계자" 역할. 이 둘은 서로 독립적이다 — 저장이 실패해도 전송은 별개로 진행되고, 전송(WebServer 연결)이 끊겨도 로컬 저장은 계속된다.

### `GW2_CrossPlatformCore`를 왜 공유하나
이 프로젝트의 네트워크 코어(`GW2_CrossPlatformCore`)는 원래 게임 서버 포팅용으로 만든 것인데, `APM_Agent`도 이걸 그대로 가져다 쓴다. 이유는 단순하다 — **비동기 I/O 코어 자체는 도메인(게임이냐 모니터링이냐)과 무관**하기 때문이다. `asio::io_context` 기반 이벤트 루프, TLS 소켓 래핑, 스레드 풀 관리 같은 건 "TCP로 여러 클라이언트를 비동기로 상대한다"는 문제의 해법이지 게임 전용 로직이 아니다.

`CMakeLists.txt` 구조도 이 관계를 그대로 보여준다:
```cmake
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../GW2_Server/GW2_CrossPlatformCore ...)
add_subdirectory(Common)   # APM_Agent 전용 코드
add_subdirectory(Storage)  # APM_Agent 전용 코드
```
`GW2_CrossPlatformCore`는 공유 라이브러리, `APM_Agent`의 `Common`/`Storage`는 그 위에 얹힌 애플리케이션 레이어다.

### 예상 질문
- **Q. 왜 게임 서버 코드베이스에서 APM 에이전트를 만들었나요?**
  → "기존에 크로스플랫폼 비동기 네트워크 코어를 만들어둔 게 있었고, 그 코어 자체는 특정 도메인에 종속되지 않는 범용 인프라라 APM 에이전트에도 그대로 재사용할 수 있었습니다. 코어(네트워크 계층)와 애플리케이션 로직(무엇을 주고받을지)을 분리해서 설계했기 때문에 가능했던 재사용입니다."

---

## 2. 크로스플랫폼 코어 — Standalone Asio

### 원래 문제
이 프로젝트의 원본(`GW2_Server/GW2_ServerCore`)은 Windows 전용 **IOCP**(I/O Completion Port)로 짜여 있었다. IOCP는 Windows 커널이 제공하는 비동기 I/O 완료 통지 메커니즘인데, Windows API에 강하게 결합돼 있어서 Linux 등 다른 OS에서는 아예 컴파일조차 안 된다.

### 왜 직접 epoll을 짜지 않고 Asio를 썼나
Linux의 비동기 I/O 메커니즘은 `epoll`이다. IOCP와 epoll은 개념(완료 통지 vs 준비 통지)부터 다르고, 이 둘을 직접 추상화하는 레이어를 처음부터 만드는 건 상당한 공수다. **Standalone Asio**(Boost 전체가 아니라 Asio 라이브러리만 떼어 쓰는 버전)는 이미 이 추상화를 제공한다 — `asio::io_context`에 비동기 작업을 등록하면, Windows에서는 내부적으로 IOCP를, Linux에서는 `epoll`을 쓰지만 애플리케이션 코드는 이 차이를 몰라도 된다.

**설계 원칙**: 비동기 I/O·멀티스레드 처리 "구조" 자체(이벤트 루프, 콜백 기반 처리)는 기존 설계를 유지하고, 그 아래 OS별 구현 디테일만 Asio로 교체했다. 즉 상위 설계를 갈아엎은 게 아니라 하부 구현체만 표준 라이브러리로 바꾼 것.

### 검증
Linux에서 빌드+실행까지 검증했다(포트 리스닝, 무크래시). 게임 콘텐츠 레이어까지 얹어서 실제로 end-to-end로 동작함을 확인한 이력도 있다(포트 7777 리스닝, SIGTERM으로 정상 종료 확인).

### Windows 크로스플랫폼 검증 (2026-07-22)
게임 콘텐츠 포팅(`GW2_Server`) 자체의 Windows 재검증은 여전히 스코프 밖이다 — 이미 검증된 원본 IOCP 버전(`GW2_ServerCore`)이 있어 굳이 재검증할 필요를 못 느꼈다는 판단 자체는 유효하다. 하지만 포트폴리오의 실제 핵심 산출물인 `APM_Agent`는 별개로 Windows에서 실제 빌드+실행+테스트까지 검증했다.

**빌드 과정에서 겪은 실제 크로스플랫폼 이슈**:
- `ResourceCollector`(CPU/메모리/디스크/네트워크 수집)는 Linux `/proc` 가상 파일시스템 기반이라 Windows 대응 코드가 전혀 없었다 — `GetSystemTimes`/`GlobalMemoryStatusEx`/`GetDiskFreeSpaceEx`/`GetIfTable2`(IP Helper API)로 동일한 델타 계산 로직을 새로 작성.
- `ApmSession::GetConnectionInfo()`의 Linux `getsockopt(TCP_INFO)`는 Windows `WSAIoctl(SIO_TCP_INFO)`로 대체 — 단 Windows API 자체가 RTT 변동성/재전송 세그먼트 수 필드를 제공하지 않아, 왜곡된 근사치를 넣는 대신 그 필드만 정직하게 0으로 남겼다.
- Linux에서 생성된 `Metric.pb.h`/`.pb.cc`(protobuf 3.21대)가 Windows에 설치한 최신 protobuf(33.4.0)의 버전 검증 매크로에 걸려 재생성이 필요했다 — 스키마(`.proto`)는 무변경, 순수 재생성.
- MSVC가 UTF-8 소스(GTest 한글 테스트 이름 포함)를 기본적으로 시스템 코드페이지로 잘못 해석해 컴파일 에러가 남 — `/utf-8` 컴파일 옵션으로 해결.

**가장 흥미로웠던 발견 — Windows 11 Smart App Control(SAC)**: 위 문제를 전부 해결해 빌드는 성공했지만, 실행하면 `Agent.exe`가 응답 없이 멈췄다. Windows 이벤트 로그(`Microsoft-Windows-CodeIntegrity/Operational`)를 뒤져 원인을 확정했다 — SAC가 로컬에서 새로 빌드한(서명 안 된) vcpkg DLL(`libcrypto-3-x64.dll`, `libprotobufd.dll` 등)의 로딩 자체를 코드 무결성 정책 위반으로 차단하고 있었다. 시스템 보안 설정(SAC)을 끄는 대신, **vcpkg 정적 링크 트리플릿(`x64-windows-static`)으로 전환**해서 최종 실행 파일에 외부 DLL 의존성 자체를 없애는 방식으로 해결했다 — 코드는 무수정, 빌드 구성(CMake 툴체인/트리플릿 옵션)만 바꾼 것.

**검증 결과**: `Collector.exe`/`Agent.exe`가 TLS+AES-256-GCM으로 통신하며 실제 Windows 리소스 지표를 수집해 SQLite에 저장하는 전체 파이프라인이 5초 주기로 30초 이상(6+ 사이클) 크래시 없이 동작함을 확인. GTest 유닛 테스트 9/9도 Windows에서 통과.

### 예상 질문
- **Q. Standalone Asio가 정확히 뭔가요? Boost.Asio랑 다른가요?**
  → "Boost.Asio는 Boost 라이브러리 전체의 일부로 배포되는데, Standalone Asio는 Boost 의존성 없이 Asio 라이브러리 코드만 떼어낸 버전입니다. Boost 전체를 빌드에 끌어오지 않아도 되는 게 장점이고, API는 사실상 동일합니다."
- **Q. 크로스플랫폼이라면서 왜 게임 콘텐츠(GW2_Server)는 Windows에서 검증 안 했나요?**
  → "게임 콘텐츠는 이미 검증된 원본 IOCP 버전이 따로 있어서, 포팅본을 그쪽에서까지 재검증하는 건 한계효용이 낮다고 판단했습니다. 대신 포트폴리오의 실제 핵심 산출물인 APM_Agent는 Windows에서 직접 빌드하고 실행까지 검증했습니다."
- **Q. Windows 포팅에서 가장 까다로웠던 부분은 뭔가요?**
  → "코드 자체보다 배포 환경 문제였습니다. 빌드는 다 성공했는데 실행이 안 됐고, 원인을 추적해보니 Windows의 Smart App Control이 서명 안 된 로컬 빌드 DLL의 로딩을 코드 무결성 정책으로 차단하고 있었습니다. 시스템 보안 설정을 끄는 대신 정적 링크로 전환해서, 보안 태세를 낮추지 않고 문제를 근본적으로 없애는 방향을 택했습니다."

---

## 3. 보안 설계

이 프로젝트에서 가장 공들인 부분이자, 실제로 설계를 한 번 뒤집은(ARIA→AES) 이력이 있는 영역이다. 순서대로 "왜 이렇게 시작했고, 왜 바꿨는지"를 따라가면 이해가 쉽다.

### 3-1. TLS 계층 — 기반이 되는 전송 보안

TLS는 **공개키(비대칭) 암호와 대칭키 암호를 함께 쓰는 하이브리드 방식**이다.

- **대칭키 암호**: 암복호화에 같은 키 하나를 쓴다. 빠르다. 문제는 이 키를 어떻게 안전하게 나눠 갖느냐.
- **공개키 암호**: 개인키(비밀)/공개키(공개) 쌍을 쓴다. 사전에 만난 적 없는 두 상대가 도청자가 있어도 안전하게 비밀을 나눠 가질 방법을 제공하지만, 계산이 무거워서 느리다.
- **TLS의 조합**: 핸드셰이크 때 공개키 암호(또는 키 교환 알고리즘)로 이번 연결에서만 쓸 임시 대칭키(세션 키)를 안전하게 합의하고, 이후 실제 데이터는 그 대칭키로 암호화한다. **"TLS = 공개키로 전부 암호화"는 흔한 오해** — 실제로는 처음에만 잠깐 공개키를 쓰고 대량 데이터는 빠른 대칭키로 처리한다.

이 프로젝트는 `asio::ssl`(OpenSSL 백엔드)로 소켓을 감싸는 방식으로 구현했다. **자체 서명 인증서**를 쓰기 때문에(정식 CA가 서명한 게 아니라 스스로에게 서명), 클라이언트 쪽에서 `verify_none`으로 검증을 생략한다 — 이건 테스트 환경 한정이고, 프로덕션이라면 반드시 CA 인증서를 신뢰 목록에 등록해서 검증해야 한다.

### 3-2. `ApmSession` 독립 작성 — 왜 공용 `Session`을 건드리지 않았나

`GW2_CrossPlatformCore`엔 이미 `Session`이라는 클래스가 있다(원래 게임 콘텐츠 레이어도 쓰는 공유 클래스). TLS를 붙일 때 이 `Session`을 확장하는 방법도 있었지만, 그렇게 하지 않고 **`APM_Agent` 전용의 독립된 `ApmSession` 클래스를 새로 작성**했다.

- **문제**: 공유 `Session` 클래스에 TLS를 가상 함수 등으로 "수술"해서 넣으면, TLS를 쓰지 않는 다른 소비자(게임 콘텐츠 레이어)까지 그 변경의 영향을 받는다. 클래스 하나의 책임 범위가 원치 않게 넓어지는 것.
- **판단**: 이 시점에 TLS가 필요한 소비자는 `APM_Agent` 하나뿐이었다. 하나뿐인 소비자를 위해 공용 클래스의 책임 범위를 미리 넓혀두는 건 **과설계(YAGNI — You Aren't Gonna Need It)**라고 판단했다.
- **해결**: `Session`은 완전히 무수정으로 남기고, `ApmSession`이 메시지 프레이밍·페이로드 암호화·재연결까지 전부 전담하는 새 클래스로 작성했다.

이 결정은 뒤에 나올 여러 설계(4절의 프레이밍, 3-4의 AES-GCM 마이그레이션)가 전부 `ApmSession` 안에서만 일어나고 `GW2_CrossPlatformCore`의 `Session`엔 손을 대지 않는 이유이기도 하다 — **처음부터 "APM 전용 관심사는 APM 전용 클래스에 담는다"는 경계를 그어둔 것.**

### 3-3. 왜 TLS만으로 안 끝내고 페이로드를 한 번 더 암호화하나

TLS는 **네트워크 구간만** 보호한다. 로드밸런서나 프록시를 거치면 그 지점에서 TLS가 해제되기도 하고, 무엇보다 이 프로젝트에서는 "애플리케이션 레벨 암호화 경험 자체"를 보여주고 싶었다. 그래서 페이로드(실제 메트릭 데이터) 자체를 애플리케이션 레벨에서 한 번 더 감싸서, TLS가 어딘가에서 해제되더라도 그 필드는 여전히 보호되게 설계했다.

### 3-4. 처음 선택 — ARIA-256-CBC + HMAC-SHA256 (구현 경험으로 코드에 남아있음)

**ARIA**는 KISA(한국인터넷진흥원)가 제정한 국내 표준 블록 암호다. 왜 이걸 골랐냐면, 국내 APM/보안 회사 포지션을 지원하는 포트폴리오에서 "국가 표준 암호를 실무 수준으로 다뤄본 경험"이 차별화 포인트가 될 거라고 판단했기 때문이다.

**CBC 모드의 근본적 한계**: CBC(Cipher Block Chaining)는 **기밀성만 제공하고 무결성은 보장하지 않는다.** 복호화할 때 마지막 블록의 PKCS7 패딩이 유효한지를 별도로 검증하는데, 만약 공격자가 암호문을 조금씩 조작해가며 "패딩이 유효했는지 아닌지"를 서버 응답(에러 종류, 응답 시간 등)으로 구별할 수 있다면, 이걸 블록 단위로 반복해서 평문을 한 바이트씩 복원할 수 있다. 이게 **패딩 오라클 공격**이다.

**대응 — Encrypt-then-MAC**: CBC를 단독으로 쓰지 않고 HMAC-SHA256으로 감쌌다.
- `AriaCipher`: OpenSSL EVP API(`EVP_aria_256_cbc`)로 암복호화.
- `HmacUtil`: OpenSSL 3.x `EVP_MAC` API로 HMAC-SHA256 계산. 태그 비교는 `CRYPTO_memcmp`(상수 시간 비교)로 해서 타이밍 공격까지 방지.
- `SecurePayload`: 위 둘을 `[IV(16B)][ciphertext][HMAC tag(32B)]` 와이어 포맷으로 결합. 암호화 키와 MAC 키는 **서로 다른 키**를 쓴다(동일 키를 암호화·인증에 함께 쓰는 건 알려진 암호학적 실수).
- **순서가 핵심**: `SecurePayload::Open()`은 **HMAC 검증을 먼저 하고, 통과한 경우에만 ARIA 복호화(및 패딩 검증)를 시도**한다. 변조된 암호문은 CBC 복호화 루틴에 도달하기도 전에 HMAC 단계에서 균일하게 거부되므로, 공격자가 패딩 유효성에 대한 어떤 정보도 얻을 수 없다.
- **IV 관리**: 매 암호화 호출마다 `RAND_bytes()`로 새 IV 생성. `AriaCipher::Encrypt()`는 IV를 출력 전용 파라미터로만 받게 만들어서, 호출자가 IV를 직접 지정하거나 재사용할 방법이 **구조적으로 없다.**

**실제 검증**: 암호문을 변조한 뒤 `Open()`을 호출하면 `AriaCipher::Decrypt()`가 아니라 `HmacUtil::Verify()` 단계에서 먼저 예외가 발생함을 확인. 실제 파이프라인에서도 대칭키를 일부러 불일치시켜(HMAC 키만 일치·ARIA 키만 불일치) 같은 현상(HMAC 통과 → ARIA 복호화 단계에서 실패 → 연결 종료)을 재현해서, Encrypt-then-MAC의 두 계층이 각각 올바르게 동작함을 실증했다.

### 3-5. AES-256-GCM으로 마이그레이션 — 왜 뒤집었나

두 구간(Agent↔Collector는 ARIA, Collector↔Console은 AES-GCM)에 서로 다른 암호를 쓰다가, **두 구간 모두 AES-256-GCM으로 통일**했다. 이유:

1. **구조적 안전성**: GCM은 **AEAD**(Authenticated Encryption with Associated Data)다. 암호화와 동시에 인증 태그가 계산되고, 복호화 시 태그 검증에 실패하면 평문이 아예 나오지 않는다. `SecurePayload`처럼 "암호화 따로, MAC 따로 만들어서 특정 순서로 검증"하는 코드를 직접 짤 필요가 없다 — **그 조합·순서를 실수할 여지 자체가 라이브러리 레벨에서 사라진다.** CBC+HMAC은 "올바르게 구현하면" 안전하지만, GCM은 "잘못 조합할 방법이 없어서" 안전하다는 근본적인 차이가 있다.
2. **단일 검증 경로**: 두 구간에 다른 암호를 쓰면 검증·유지보수 대상도 두 배가 된다. 실제로 이 프로젝트에서 `AesGcmCipher`/`AesGcmPayload`는 GoogleTest 9개로 검증돼 있었지만, 더 복잡한 `AriaCipher`/`HmacUtil`/`SecurePayload`는 자동 테스트가 없는 비대칭이 있었다. 하나로 합치면 이미 있던 테스트 9개가 두 구간 모두를 커버한다.

**잃은 것과 보존 방법**: ARIA는 KISA 표준이라 국내 문맥에서 의미가 있었는데, 실행 경로에서는 더 이상 쓰이지 않는다. 이 경험 자체를 지우지 않기 위해:
- `AriaCipher`/`HmacUtil`/`SecurePayload` **코드는 삭제하지 않고 그대로 남김**(빌드는 되지만 `Agent`/`Collector`의 실행 경로에서는 더 이상 참조하지 않음). 각 파일에 "참고용, 실행 경로 미사용" 주석을 남겨서 죽은 코드로 오해받지 않게 함.
- 별도 문서(`Docs/ARIA_TO_AES_MIGRATION.md`)에 구현·검증 경험을 전부 정리.

**IPayloadSealer 추상화 덕에 마이그레이션이 쉬웠다**: 세션(`ApmSession`/`ResilientSender`)이 구체 암호 방식에 고정되지 않도록 `Seal()`/`Open()` 인터페이스로 일반화해뒀기 때문에, 실제 마이그레이션은 `Collector/main.cpp`/`Agent/main.cpp`에서 **주입하는 구현체 몇 줄만 바꾸는 것**으로 끝났다:

```cpp
// 수정 전
AriaCipher::Key encKey = LoadKeyFromHexFile("certs/aria.key");
HmacUtil::Key macKey = LoadKeyFromHexFile("certs/hmac.key");
...
std::make_unique<SecurePayload>(encKey, macKey)

// 수정 후
AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");
...
std::make_unique<AesGcmPayload>(agentCollectorKey)
```
세션 생성 로직, 프레이밍 로직(`ApmSession::Send`/`ProcessAccumulated`), 재연결 로직(`ResilientSender`)은 **단 한 줄도 안 바뀌었다.** 이게 인터페이스로 관심사를 분리해두는 것의 실질적 가치다 — 마이그레이션 자체가 이 설계의 증거가 됐다.

### 3-6. 구간별 키 분리

Agent↔Collector와 Collector↔Console은 **같은 암호 방식(AES-256-GCM)을 쓰지만 키는 서로 다르다**:
- Agent↔Collector: `certs/agent_collector_aes.key`
- Collector↔Console: `certs/webserver_aes.key`

한 구간의 키가 유출돼도 다른 구간은 안전하게 유지하기 위한 결정이다. 대칭키 배포 자체는 로컬 파일 공유(TLS 인증서와 같은 패턴)로 단순화했고, 실제 운영 환경이라면 KMS·주기적 로테이션 같은 별도 키 관리 체계가 필요하다는 한계도 인지하고 있다.

### 3-7. Collector↔Console 관계의 진화 — 암묵적 전제에서 명시적 네트워크 관계로

처음엔 `Collector`와 `APM_Console` 두 프로세스가 **같은 장비의 SQLite 파일을 각자 다른 시점에 여는** 방식이었다. 이건 "두 프로세스가 항상 같은 장비에 있다"는 암묵적 전제 위에서만 동작하는 구조다. 이걸 실제 네트워크 관계(Collector가 능동적으로 데이터를 전송, Console이 자기 소유 DB에 저장)로 바꿨다 — 서로 다른 장비에 있어도 동작하도록.

- **트리거**: 주기적(`collector_config.json`의 `push_interval_seconds`, 기본 10초) + `Collector` 콘솔에 `send` 입력 시 즉시 전송.
- **왜 하필 JSON 설정 파일인가**: `collector_config.json`은 평문 텍스트(예: `key=value` 한 줄짜리)로도 충분했을 텐데, 굳이 JSON 파서(`nlohmann/json` 벤더링)까지 들여서 썼다. 이유는 **차후 웹에서 이 설정을 편집할 가능성을 열어두기 위해서** — JSON은 웹 쪽(자바스크립트/REST API)과 자연스럽게 주고받을 수 있는 포맷이라, 나중에 "대시보드에서 push 주기를 바꾼다" 같은 기능을 얹을 때 파일 포맷을 다시 설계할 필요가 없다.
- **동시성 설계**: stdin은 별도 스레드로 읽고, 실제 전송(`flushToWebServer`)은 `asio::post()`로 `io_context` 스레드에 넘긴다 — 공유 상태(`pendingMetrics`, `webServerSender`)를 항상 단일 스레드에서만 건드리게 되어 **락이 아예 불필요**해진다.

### 예상 질문
- **Q. 왜 기존 `Session` 클래스를 확장하지 않고 `ApmSession`을 새로 만들었나요?**
  → "TLS가 필요한 소비자가 이 시점엔 APM_Agent 하나뿐이었습니다. 하나의 소비자를 위해 여러 소비자가 공유하는 클래스의 책임 범위를 미리 넓혀두는 건 YAGNI 원칙에 어긋난다고 판단했고, 그래서 공유 Session은 완전히 무수정으로 두고 APM 전용 관심사(TLS, 프레이밍, 페이로드 암호화)는 전부 별도 클래스에 담았습니다. 나중에 소비자가 늘어나면 그때 공용화를 재검토하면 됩니다."
- **Q. 왜 두 구간에 서로 다른 암호를 섞어 쓰다가 통일했나요?**
  → "Agent↔Collector는 KISA 표준 암호(ARIA)를 실무 수준으로 다뤄보는 게 목적이었고, Collector↔Console은 .NET과의 상호운용성 때문에 AES-GCM을 썼습니다. 나중에 되짚어보니 CBC+HMAC의 수동 조합은 구조적으로 실수 여지가 있고, 테스트도 AES-GCM에만 있는 비대칭이 있어서, GCM의 AEAD 구조적 안전성과 단일 검증 경로라는 이유로 통일했습니다. ARIA 구현 경험 자체는 코드와 문서로 남겨뒀습니다."
- **Q. AEAD가 CBC+HMAC보다 왜 더 안전한가요?**
  → "CBC+HMAC은 두 개의 독립된 연산(암호화, MAC)을 개발자가 특정 순서(먼저 MAC 검증, 그다음 복호화)로 직접 조합해야 하고, 이 순서를 실수하면 패딩 오라클 공격면이 그대로 열립니다. GCM 같은 AEAD는 암호화와 인증을 라이브러리 내부에서 하나의 연산으로 처리해서, 개발자가 조합 순서를 실수할 방법 자체가 없습니다. '올바르게 구현하면 안전한 것'과 '잘못 구현할 방법이 없는 것'의 차이입니다."
- **Q. 패딩 오라클 공격이 정확히 뭔가요?**
  → "CBC 모드는 마지막 블록의 패딩(PKCS7) 유효성을 복호화 시 검증하는데, 공격자가 암호문을 한 바이트씩 조작해가며 '패딩이 유효했는지'를 서버 응답 차이(에러 종류, 응답 시간 등)로 구별할 수 있으면, 이 정보를 오라클(oracle)처럼 반복 질의해서 평문을 한 바이트씩 복원할 수 있는 공격입니다. Encrypt-then-MAC으로 감싸서 변조된 암호문을 패딩 검증 단계에 도달하기 전에 차단하면 이 공격면 자체가 사라집니다."
- **Q. IV/Nonce를 재사용하면 왜 안 되나요?**
  → "CBC에서 IV를 재사용하면 같은 평문 블록이 같은 암호문 블록을 만들어서 패턴이 노출됩니다. GCM에서는 더 치명적인데, 같은 키로 Nonce를 재사용하면 인증 태그를 위조할 수 있는 방법이 알려져 있어서 기밀성과 무결성이 동시에 깨집니다. 그래서 이 프로젝트는 IV/Nonce를 호출자가 지정할 방법 자체를 없애고(`RAND_bytes()`로 매번 내부 생성, 출력 전용 파라미터), 재사용을 코드 레벨에서 원천 차단했습니다."

---

## 4. 통신 프로토콜

### 4-1. 메시지 프레이밍 — TCP는 스트림이라 경계가 없다

TCP는 **스트림 프로토콜**이라 "한 번의 `read`가 정확히 하나의 논리적 메시지"라는 보장이 없다. 이 프로젝트도 초기엔 이 사실을 놓쳐서, `read_some()` 결과 하나를 메시지 하나로 가정하고 있었다 — 우연히(메시지가 작고 빈도가 낮아서) 동작했을 뿐인 설계 공백이었다.

**해결**: 모든 패킷 앞에 `PacketHeader{uint16 size, uint16 id}`(4바이트)를 붙인다. 수신 측(`ApmSession::ProcessAccumulated()`)은 들어오는 바이트를 `_accumulated` 버퍼에 계속 쌓다가, `header.size`만큼 모이면 그제서야 패킷 하나로 확정한다. 아직 다 안 모였으면 다음 `recv`를 기다린다.

```cpp
void ApmSession::ProcessAccumulated()
{
    while (true)
    {
        size_t remaining = _accumulated.size() - processed;
        if (remaining < HEADER_SIZE) break;
        const PacketHeader* header = ...;
        if (remaining < header->size) break;   // 아직 페이로드까지 다 안 모임
        // 완전한 패킷 하나 확정 → 복호화 → 콜백 호출
        ...
    }
}
```

### 4-2. Protobuf 리플렉션으로 패킷 ID 자동 결정

패킷 ID를 수동으로 배정하면(`enum { LOGIN = 1, METRIC = 2, ... }`) 오타나 충돌이 생길 수 있다. 대신 **Protobuf 리플렉션**을 쓴다: `PacketType::descriptor()->index()` — `.proto` 파일 안에서 메시지가 선언된 순서를 그대로 ID로 쓴다. 개발자가 ID를 직접 관리할 필요가 없어서, ID 충돌이나 오타가 **구조적으로 발생할 수 없다.**

### 4-3. `PacketHandler` — 타입 안전 디스패치

```cpp
template<typename PacketType>
static void Register(TypedHandler<PacketType> handler)
{
    uint16 id = static_cast<uint16>(PacketType::descriptor()->index());
    Handlers()[id] = [handler](const String& payload)
    {
        PacketType pkt;
        if (!pkt.ParseFromString(payload)) { /* 에러 로그 */ return; }
        handler(pkt);   // 호출자는 이미 파싱된 강타입 메시지만 받음
    };
}
```
`Register<T>()`가 ID 결정과 역직렬화(`ParseFromString`)를 전부 자동으로 처리한다. 호출자(`Collector`)는 `const apm::Metric&`처럼 이미 파싱된 강타입 메시지만 받는다 — 문자열 파싱을 직접 할 일이 없다. `Dispatch(id, payload)`(비템플릿)는 `ApmSession::Start()`의 콜백으로 그대로 전달된다.

### 4-4. 크로스 언어 스키마 공유 — `Metric.proto`

`APM_Agent`(C++)와 `APM_Console`(.NET)은 **같은 `.proto` 파일**(`Metric.proto`)에서 각자의 언어로 코드젠한다. C++ 쪽은 `protoc --cpp_out`, .NET 쪽은 `Grpc.Tools`+`Google.Protobuf` NuGet 패키지를 쓰는데, **실제 gRPC 서비스는 쓰지 않는다**(`GrpcServices="None"`) — Protobuf의 메시지 직렬화 기능만 재사용하는 것. 스키마를 양쪽에 각각 손으로 복사해서 관리하면 시간이 지나며 반드시 어긋나게 되는데(스키마 이원화), 같은 원본 파일에서 코드젠하기 때문에 이 위험이 구조적으로 없다.

프레이밍(`PacketHeader{uint16 size, uint16 id}`)도 C++/​.NET 양쪽이 같은 와이어 포맷을 수동으로 맞춰 파싱한다 — `.NET` 쪽 `ReadExactAsync`가 부분 수신(partial read)까지 처리하는 루프로, 정확히 `header.size`만큼 모일 때까지 누적하는 건 C++의 `ProcessAccumulated`와 동일한 발상이다. `BinaryPrimitives.ReadUInt16LittleEndian`으로 리틀엔디안을 명시해서, C++ 구조체의 메모리 레이아웃(리틀엔디안 플랫폼 기준)과 정확히 맞춘다.

### 4-5. `ResilientSender` — 재연결/큐잉 설계

이건 C++(`APM_Agent`) 클래스다 — Agent→Collector, Collector→Console **양쪽 모두**에서 재사용된다.

**문제**: 연결이 끊기면(네트워크 단절, 상대 프로세스 재시작 등) 그 사이 수집한 데이터가 그냥 유실된다. `ApmSession::Send()`가 실패해도 아무도 감지·재시도하지 않으면 모니터링 시스템의 존재 의미가 없다.

**설계**: ① 연결 끊김 감지 → ② 끊긴 동안 보낼 데이터를 로컬 큐(메모리, `std::deque`)에 쌓음 → ③ 재연결되면 큐에 쌓인 걸 순서대로 재전송.

```cpp
void ResilientSender::EnqueueRaw(uint16 id, String payload)
{
    if (_queue.size() >= _maxQueueSize)
    {
        _queue.pop_front();   // 큐가 가득 차면 가장 오래된 것부터 버림
    }
    _queue.push_back(QueuedPacket{ id, std::move(payload) });
    if (_connected && !_sending)
        FlushNext();
}
```
- 큐가 가득 차면 **가장 오래된 것부터 버린다** — 모니터링 특성상 최신 데이터가 더 중요하다는 가정.
- 재연결은 5초 타이머로 재시도(`ScheduleReconnect`).
- `FlushNext()`는 큐의 맨 앞(front)부터 하나씩 보내고, 전송 성공 시에만 `pop_front()`한다 — 실패하면 큐에 그대로 남기고, `OnSessionDisconnected`가 재연결을 예약해서 나중에 이 항목부터 다시 재시도한다.
- **주의(스코프)**: 메모리 큐만 구현했다 — 프로세스 자체가 죽으면 큐도 같이 사라진다(디스크 영속화는 범위 밖). "네트워크만 끊겼을 때" 케이스를 먼저 해결한 것.

**검증**: `Collector` 강제 종료 → 단절 중 여러 건 큐잉 → `Collector` 재시작 → 재연결 즉시 버퍼링분 재전송 + 신규 수집분까지 정상 수신, 순서/손실 문제 없음을 확인.

### 예상 질문
- **Q. TCP가 스트림이라는 게 정확히 무슨 뜻인가요?**
  → "TCP는 메시지 단위가 아니라 바이트 스트림을 전달합니다. 송신 측이 `send()`를 3번 호출해도 수신 측은 `recv()` 한 번에 다 받을 수도 있고, 반대로 한 번에 보낸 걸 여러 번에 나눠 받을 수도 있습니다. 그래서 '메시지의 경계가 어디인지'는 애플리케이션이 직접 정의해야 하고, 이게 메시지 프레이밍이 필요한 이유입니다."
- **Q. 큐가 가득 차면 오래된 것부터 버리는 게 데이터 유실 아닌가요?**
  → "맞습니다, 의도적인 트레이드오프입니다. 무한정 쌓아두면 메모리가 계속 늘어나서 에이전트 자체가 무거워지는 게 더 큰 문제라고 판단했고, 모니터링 데이터는 최신 상태가 가장 중요하다는 도메인 특성상 오래된 데이터부터 버리는 게 합리적이라고 봤습니다."

---

## 5. 데이터 저장

### 5-1. `IMetricStore` — 컴파일 타임 저장소 선택 (C++)

SQLite(경량, 임베디드)와 TimescaleDB(PostgreSQL 확장, 시계열 특화 압축·파티셔닝) 둘 다 지원하고 싶은데, **안 쓰는 백엔드의 의존성이 결과물에 남으면 안 된다** — "가벼운 에이전트"라는 목표에 어긋나기 때문이다.

**해결**: 런타임 분기(`if (backend == "sqlite") ...`) 대신, 인터페이스(`IMetricStore`) + CMake 옵션(`APM_STORAGE_BACKEND`)으로 **컴파일 타임에 하나만 선택**한다.

```cpp
// MetricStoreFactory.cpp
#if defined(APM_STORAGE_SQLITE)
#include "SqliteMetricStore.h"
#elif defined(APM_STORAGE_TIMESCALEDB)
#include "TimescaleMetricStore.h"
#else
#error "APM_STORAGE_SQLITE 또는 APM_STORAGE_TIMESCALEDB 중 하나가 정의되어야 함"
#endif

std::unique_ptr<IMetricStore> CreateMetricStore(const String& connectionInfo)
{
#if defined(APM_STORAGE_SQLITE)
    return std::make_unique<SqliteMetricStore>(connectionInfo);
#elif defined(APM_STORAGE_TIMESCALEDB)
    return std::make_unique<TimescaleMetricStore>(connectionInfo);
#endif
}
```
선택 안 한 쪽의 헤더/구현은 아예 컴파일되지 않는다(전처리기 분기). **검증**: SQLite 선택 시 `ldd`로 `libpq`(PostgreSQL 클라이언트 라이브러리)가 결과 바이너리에 전혀 링크되지 않음을 실측 확인 — Postgres가 시스템에 설치돼 있지 않아도 빌드·실행이 가능하다.

### 5-2. .NET(APM_Console)의 런타임 선택과의 대비 — 언어별 컴파일 모델 차이

`ApmModule.cs`(.NET)는 **같은 문제(SQLite vs TimescaleDB 선택)를 런타임에** 푼다:

```csharp
public void RegisterServices(IServiceCollection services, IConfiguration configuration)
{
    var backend = configuration["Apm:StorageBackend"] ?? "SQLite";
    services.AddDbContext<ApmDbContext>(options =>
    {
        if (backend == "TimescaleDB")
            options.UseNpgsql(connectionString);
        else
            options.UseSqlite(connectionString);
    });
}
```
왜 이래도 되나 — **.NET은 두 EF Core 프로바이더(Npgsql, Sqlite)를 둘 다 참조해둬도 빌드에 지장이 없다.** C++처럼 "안 쓰는 라이브러리가 결과물에 딸려온다"는 문제가 없어서, 굳이 컴파일 타임 분기라는 복잡함을 감수할 이유가 없다. 그래서 `appsettings.json`의 `Apm:StorageBackend` 값 하나로 런타임에 전환 가능하게 단순화했다.

**같은 설계 문제를 두 언어가 다르게 푸는 이유가 "언어의 컴파일 모델 차이"라는 걸 이해하고 있다는 게 포인트다** — C++은 정적 링크가 기본이라 안 쓰는 의존성도 결과물에 남지만, .NET(관리 코드) 생태계는 여러 구현체를 동시에 참조해도 되는 유연성이 있다.

### 예상 질문
- **Q. 왜 C++은 컴파일 타임, .NET은 런타임으로 저장소를 고르나요?**
  → "C++은 링크 시점에 안 쓰는 라이브러리(예: libpq)까지 결과물에 딸려오는 걸 막으려면 컴파일 타임 분기가 필요합니다. 반면 .NET은 두 EF Core 프로바이더를 다 참조해도 배포 크기나 의존성 문제가 크지 않아서, 설정 파일 값 하나로 런타임에 유연하게 전환하는 쪽이 더 실용적이었습니다. 언어의 빌드/배포 모델 차이를 반영한 의도적 선택입니다."

---

## 6. 수집 지표 구현 상세

`ResourceCollector`(C++, Linux 전용)가 `/proc` 파일시스템을 파싱해서 지표를 만든다. "어떻게 계산하는지"를 알아두면 관련 질문에 훨씬 자신 있게 답할 수 있다.

### 6-1. CPU 사용률 — jiffies 델타

`/proc/stat`의 첫 줄(`cpu  user nice system idle iowait irq softirq steal ...`)은 부팅 이후 누적된 시간(jiffies 단위)이다. 한 시점의 값만으로는 "지금 사용률"을 알 수 없다 — **두 시점 사이의 델타**를 봐야 한다.

```cpp
double ResourceCollector::ComputeCpuUsage()
{
    // ... /proc/stat 파싱 ...
    uint64 idleAll = idle + iowait;
    uint64 totalAll = user + nice + system + idle + iowait + irq + softirq + steal;

    double usage = 0.0;
    if (_hasPrevSample && totalAll > _prevTotal)
    {
        uint64 idleDelta = idleAll - _prevIdle;
        uint64 totalDelta = totalAll - _prevTotal;
        usage = 100.0 * (1.0 - (double)idleDelta / (double)totalDelta);
    }
    _prevIdle = idleAll; _prevTotal = totalAll; _hasPrevSample = true;
    return usage;
}
```
"idle이 아닌 시간의 비율"이 곧 사용률이다. 첫 호출은 이전 샘플이 없어서 0을 반환하고(`_hasPrevSample` 플래그), 그다음부터는 델타 계산이 가능해진다. `idle`뿐 아니라 `iowait`(디스크 I/O 대기)도 "쉬고 있는 시간"으로 합산하는 게 표준적인 계산 방식이다.

### 6-2. 메모리 — `MemFree`가 아니라 `MemAvailable`

```cpp
uint64 ResourceCollector::GetMemUsed()
{
    // /proc/meminfo에서 MemTotal, MemAvailable 파싱
    return (total - available) * 1024;
}
```
`MemFree`(완전히 비어있는 메모리)를 쓰면 안 된다 — Linux는 남는 메모리를 페이지 캐시 등으로 적극 활용하기 때문에, `MemFree`만 보면 "메모리가 거의 다 찼다"는 착시가 생긴다. `MemAvailable`은 캐시처럼 **필요하면 즉시 회수 가능한 메모리까지 포함**해서 계산한 "실질적으로 새 프로세스가 쓸 수 있는 메모리" 추정치다(커널이 직접 계산해서 제공). 그래서 "사용량 = 전체 - MemAvailable"이 실제 체감 사용량에 더 가깝다.

### 6-3. 네트워크 대역폭 — 델타 + 루프백 제외

```cpp
void ResourceCollector::ComputeNetworkUsage(uint64& rxBytesPerSec, uint64& txBytesPerSec)
{
    // /proc/net/dev 파싱, 인터페이스별 rx/tx bytes 누적값 합산
    // "lo"(루프백)는 실제 네트워크 트래픽이 아니므로 제외
    // ... 이전 샘플과의 델타 / 경과 시간(steady_clock) = 초당 바이트
}
```
`/proc/net/dev`도 CPU와 마찬가지로 **누적값**이라 델타가 필요하다. 여기선 `std::chrono::steady_clock`으로 실제 경과 시간을 측정해서(고정 주기라고 가정하지 않고) 정확한 바이트/초를 계산한다. 인터페이스 이름이 환경마다 다르므로(`eth0`, `ens33`, `wlan0`...) `lo`만 제외하고 나머지는 전부 합산 — 이식성을 확보하는 방식이다.

### 6-4. TCP 연결 품질 — 커널에서 직접 조회

```cpp
TcpConnectionInfo ApmSession::GetConnectionInfo()
{
    struct tcp_info tcpInfo;
    socklen_t len = sizeof(tcpInfo);
    int fd = _sslStream.lowest_layer().native_handle();
    if (::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &tcpInfo, &len) == 0)
    {
        info.rttMicros = tcpInfo.tcpi_rtt;
        info.rttVarMicros = tcpInfo.tcpi_rttvar;
        info.retransmits = tcpInfo.tcpi_retransmits;
        info.totalRetrans = tcpInfo.tcpi_total_retrans;
        info.sndCwnd = tcpInfo.tcpi_snd_cwnd;
    }
    return info;   // 실패해도 전부 0인 기본값 반환 - 호출자가 예외처리 안 해도 됨
}
```
`getsockopt(fd, IPPROTO_TCP, TCP_INFO, ...)`로 **커널이 그 소켓에 대해 이미 추적하고 있는** RTT(왕복시간)/재전송 횟수/혼잡 윈도우 정보를 직접 읽어온다. 별도로 ping을 보내거나 직접 측정할 필요가 없다 — 커널의 TCP 스택이 어차피 흐름 제어를 위해 계속 추적하고 있는 값을 그냥 꺼내 쓰는 것. **TLS 레이어 아래의 순수 TCP 계층 정보**라서 암호화 여부와 무관하게 조회 가능하다.

### 6-5. 디스크 사용량

```cpp
void ResourceCollector::GetDiskUsage(uint64& totalBytes, uint64& usedBytes, const String& path)
{
    struct statvfs stat;
    ::statvfs(path.c_str(), &stat);
    totalBytes = stat.f_blocks * stat.f_frsize;
    usedBytes = totalBytes - (stat.f_bfree * stat.f_frsize);
}
```
`statvfs()`는 파일시스템 통계를 직접 조회하는 POSIX 표준 함수다. 블록 개수(`f_blocks`)와 블록 크기(`f_frsize`)를 곱해서 바이트 단위로 변환한다.

### 예상 질문
- **Q. 왜 MemFree가 아니라 MemAvailable을 쓰나요?**
  → "MemFree는 아무것도 안 쓰고 있는 순수 여유 메모리인데, Linux는 남는 메모리를 페이지 캐시로 적극 활용하기 때문에 MemFree만 보면 실제로는 여유가 있는데도 메모리가 부족한 것처럼 보이는 착시가 생깁니다. MemAvailable은 캐시처럼 필요시 즉시 회수 가능한 부분까지 커널이 계산해서 제공하는 값이라, 실제 체감 여유 메모리에 훨씬 가깝습니다."
- **Q. TCP 재전송률 같은 걸 왜 애플리케이션에서 직접 측정 안 하고 커널 값을 쓰나요?**
  → "커널의 TCP 스택은 흐름 제어와 혼잡 제어를 위해 RTT, 재전송 횟수 같은 값을 이미 실시간으로 추적하고 있습니다. 이걸 getsockopt(TCP_INFO)로 그냥 읽어오면 되는데, 굳이 애플리케이션 레벨에서 별도로 재측정하면 정확도도 떨어지고 불필요한 오버헤드만 생깁니다."

---

## 7. 에이전트 성능/권한 설계

### 7-1. `PrivilegeDrop` — 최소 권한 원칙

**동기**: 모니터링 에이전트는 시스템 구석구석(프로세스 정보, 네트워크 상태 등)을 들여다봐야 해서 종종 높은 권한으로 실행되는데, 만약 이 에이전트 자체가 침해당하면(취약점 등으로) 공격자가 그 권한을 그대로 물려받는다. **침해당했을 때 피해 범위를 최소화하는 것**이 목적.

```cpp
void PrivilegeDrop::DropTo(const String& targetUser)
{
    if (::geteuid() != 0) { /* 이미 비-root면 스킵 */ return; }

    // 순서가 중요: uid를 먼저 낮추면 root 권한이 사라져서
    // 이후 setgid/setgroups가 실패한다.
    ::setgid(pw->pw_gid);      // 1. gid 먼저
    ::setgroups(0, nullptr);   // 2. 상속된 보조 그룹 제거 (흔히 빠뜨리는 단계)
    ::setuid(pw->pw_uid);      // 3. uid 마지막

    // 검증: 하향이 "되돌릴 수 없는" 상태인지 확인
    if (::setuid(0) == 0)
        throw std::runtime_error("setuid(0) 재획득이 가능함(권한 하향 실패)");
}
```
- **순서가 핵심**: gid → 보조 그룹 제거 → uid. uid를 먼저 낮추면 root 권한이 사라져서 그 뒤의 `setgid`/`setgroups` 호출 자체가 권한 부족으로 실패한다.
- **보조 그룹(supplementary groups) 제거**: `setuid`/`setgid`만 하면 uid/gid는 낮아지지만, 원래 root가 속해있던 보조 그룹 권한이 남아있을 수 있다 — 흔히 놓치는 단계.
- **재상승 불가 검증**: 권한 하향 후 `setuid(0)`을 다시 시도해서 **성공하면 오히려 문제**다 — 이게 성공한다는 건 "saved-set-uid"가 여전히 0(root)으로 남아있어서 언제든 root로 되돌아갈 수 있다는 뜻이고, 하향이 불완전했다는 증거다. 실패해야(즉 재상승이 불가능해야) 정상.

포트 바인딩처럼 특권이 필요한 초기화가 끝나는 즉시 호출한다. 지금은 포트가 9000(비특권 포트)이라 실질적으로 root가 필요 없지만, root로 기동되는 배포 환경을 가정한 방어적 설계다.

### 7-2. syscall 최소화 — 커널 모드 전환 비용

**개념 구분(면접에서 헷갈리기 쉬운 부분)**: 최소 권한(위 7-1)은 **보안** 관점, syscall 최소화는 **성능** 관점 — 완전히 독립된 두 개념이다.

Asio의 `epoll` 기반 이벤트 루프를 그대로 쓴다 — 연결마다 스레드를 만들거나 블로킹 read를 하는 대신, 이벤트가 준비될 때까지 `epoll_wait()`로 블로킹 대기한다(busy-polling 없음, CPU를 계속 태우지 않음).

**실측**(`strace -c`로 Agent를 5분간 감쌈, 수집 주기 5초 × 약 60사이클 — 정상 동작 상태의 steady-state 비용을 보기 위함):

| 항목 | 횟수 | 비고 |
|---|---|---|
| `epoll_wait` | 155 | 스핀 없이 블로킹, 사이클당 약 2.6회 |
| `read` | 460 | TLS 소켓 수신 + `/proc` 파일 읽기 |
| `openat` | 311 | `/proc/stat`·`/proc/meminfo`(x2)·`/proc/net/dev`를 매 사이클 새로 여는 비용 |
| `close` | 309 | 위 `openat`과 쌍을 이룸 |
| `sendto`/`write` | 76/77 | TLS 전송 |
| `getsockopt` | 75 | `TCP_INFO` 조회 |
| `statfs` | 74 | 디스크 사용량 조회(`statvfs`) |
| **전체** | **1,921** | 총 19.4ms(5분 동안) — 압도적으로 `epoll_wait` 대기가 지배적 |

`/proc` 파일을 매 사이클 새로 열고 닫는 게(`openat`+`close`가 합쳐서 전체의 32%) 유일하게 눈에 띄는 반복 비용인데, 5초 주기에서는 절대 시간이 마이크로초 단위라 실질적 영향은 없다 — **다만 수집 주기를 훨씬 좁힐 계획이 생기면 파일 디스크립터를 캐싱하는 최적화를 고려할 지점**으로 남겨뒀다(솔직하게 "다 완벽하다"가 아니라 알려진 최적화 여지를 스스로 인지하고 있다는 걸 보여주는 부분).

### 7-3. 리소스 사용량 실측 — "가볍다"를 수치로 증명

5분간 정상 동작시키며 15초 간격으로 20회 샘플링(`/proc/[pid]/status`, `/proc/[pid]/stat` 기준):
- **RSS(메모리)**: 20회 샘플 전부 **13,716~14,156KB(약 13.8MB)로 완전히 고정** — 증가 추세 없음 = 메모리 누수 없음을 실측으로 증명.
- **CPU 사용량**: 5분(300초) 동안 누적 `utime`+`stime` 합계 5 tick(0.05초, `HZ=100` 기준) — 평균 **약 0.017%**. `epoll_wait` 블로킹이 거의 전부라 busy-polling이 없음을 재확인.

"가볍다"는 주장을 말로만 하지 않고, 실제로 프로세스를 오래 돌려서 수치로 뒷받침한 게 포인트다.

### 7-4. Collector 스케일 테스트 — `LoadTester`로 병목 찾기

**동기**: 위 7-2/7-3은 Agent 하나가 정상 동작할 때 "가볍다"는 걸 증명한 것 — 이 프로젝트의 진짜 스케일 병목은 N개의 Agent 연결을 동시에 받아 처리하는 **Collector** 쪽에 있을 거라는 가설이 코드 리딩 단계(§1)에서부터 있었다: `Collector::main()`이 `ioContext.run()`을 메인 스레드에서 단일 호출 — 접속 수가 늘어도 복호화/파싱/SQLite 저장은 전부 한 스레드에서 순차 처리된다. 이 가설을 말로만 두지 않고 자체 제작한 `LoadTester`로 실제로 검증했다.

**방법**: `LoadTester`가 N개의 Agent 연결을 in-process로 동시에 시뮬레이션(각각 별도 프로세스를 fork하는 대신, 기존 `ResilientSender`/`AesGcmPayload`/`apm::Metric`을 그대로 재사용해 실제 Agent와 동일한 와이어 프로토콜로 통신) → Collector를 `strace -c` + RSS/CPU 1초 간격 샘플링으로 감싼 채 60초간 부하를 걸고 → 결과를 파일로 수집.

**실측 결과** (`agents`=동시 연결 시도 수, `duration`=60초, `interval`=100ms):

| agents | 실제 접속 성공 | 전송 건수 | p50 | p95 | p99 |
|---|---|---|---|---|---|
| 1 | 1 | 593 | - | 0ms | 1ms |
| 10 | 10 | 5,958 | 0ms | 0ms | 1ms |
| 50 | 50 | 29,800 | 0ms | **12,051ms** | **22,177ms** |
| 100 | **72** | 42,900 | 0ms | 34,212ms | 48,412ms |
| 100(ramp-up 5s) | **72** | 41,652 | 0ms | 31,572ms | 45,213ms |
| 300(ramp-up 5s) | **71** | 41,981 | 0ms | 33,467ms | 47,732ms |

**발견 1 — 동시 접속 ~71~72개에서 하드 리밋**: 100을 요청하든 300을 요청하든 실제 접속 성공 수는 71~72개에서 멈춘다(60초 테스트 구간 내 TLS 핸드셰이크를 못 끝낸 나머지는 아예 집계가 안 됨). 연결을 5초에 걸쳐 서서히 늘리는 ramp-up도 이 리밋을 바꾸지 못했다 — "한꺼번에 몰려서 생긴 순간적인 문제"가 아니라 구조적인 처리량 한계라는 뜻이다.

**발견 2 — 지연시간이 50개부터 절벽처럼 치솟음**: p95가 0ms(10개) → 12초(50개) → 34초(100개+)로 폭증한다. 그런데 재전송 큐(`ResilientSender`)의 `queue_drop`은 전 구간에서 0 — 데이터가 버려지진 않고, 그냥 한없이 밀린다.

**발견 3 — CPU/메모리는 범인이 아니다**: RSS는 13~19MB 선에서 안정적이고(요청한 연결 수가 아니라 실제 접속 수에 비례, 시간에 따른 증가 추세 없음 = 누수 아님), CPU 사용량도 61초 동안의 누적 tick 총합이 100 요청과 300 요청에서 거의 동일(평균 약 28%) — 즉 CPU도 놀고 있다.

**발견 4 — `strace` 시스템콜 요약이 진범을 지목**: 전체 syscall 시간의 70% 이상이 `pwrite64`(21%) + `fcntl`(16%) + **`fdatasync`(16%, 약 9,200회)** + `write`(12%) + 저널 파일 관리(`openat`/`unlink`/`newfstatat`)에 쓰였다. 이건 **SQLite 기본 롤백 저널 모드가 메트릭 1건을 저장할 때마다 저널 파일을 열고 → 쓰고 → `fdatasync`로 디스크에 강제 flush하고 → 지우는** 전형적인 패턴이다. 그리고 이 `SqliteMetricStore::Store()` 호출이 네트워크 I/O(accept/read)와 **같은 단일 `io_context` 스레드**에서 동기적으로 실행되므로, 디스크 fsync 한 번이 그 순간 전체 이벤트 루프를 블로킹한다 — 그동안 신규 접속 수락도, 다른 소켓 데이터 처리도 전부 밀린다.

**결론**: §1(코드 리딩)에서 세운 "단일 스레드 `io_context` 병목" 가설이, "**SQLite의 동기 `fdatasync`가 그 단일 스레드를 블로킹한다**"는 훨씬 구체적이고 실측으로 검증된 형태로 확인됐다. CPU/메모리가 병목이 아니라는 것까지 실측으로 배제했기 때문에 "느리다"가 아니라 정확히 "왜" 느린지(디스크 I/O 대기, 그것도 SQLite의 저널링 방식 때문)를 짚을 수 있다.

**알려진 개선 방향**: `PRAGMA journal_mode=WAL` + `synchronous=NORMAL`로 전환하면 저널 파일 open/close/unlink 오버헤드와 fsync 빈도를 크게 줄일 수 있다 — WAL은 커밋마다 별도 저널 파일을 새로 만들고 지우는 대신 하나의 WAL 파일에 append만 하고, 체크포인트 시점에만 fsync하기 때문이다. 정합성/내구성 트레이드오프(`synchronous=NORMAL`은 OS 크래시 시 최후 커밋 몇 건을 잃을 수 있음)가 있어 이때는 바로 바꾸지 않았으나, 7-5(저장 워커 스레드 분리) 이후 `strace -f`로 재측정해 실제 도입했다 — 상세: §7-6.

### 7-5. 저장 작업 비동기화 — 실제로 병목을 고치고 재실측으로 검증

**동기**: 7-4에서 "왜 느린지"까지는 실측으로 증명했지만 고치지는 않았다. 관찰만 하고 넘길 문제가 아니라고 판단해, `SqliteMetricStore::Store()` 호출을 네트워크 스레드에서 떼어내 전용 워커 스레드로 위임하는 개선을 실제로 적용했다(시계열 append성 데이터라 원자성/경합 문제가 없다는 판단).

**시행착오 — 검증된 서브시스템이라고 안전하지 않았다**: 1차로 이 프로젝트가 이미 의존 중인 `GW2_CrossPlatformCore/Thread/JobQueue`를 재사용해 적용했는데, 재실측 도중 Collector가 시작 후 약 10초 만에 반복적으로 크래시(`LOCK_TIMEOUT`)하는 걸 발견했다. 원인은 `Thread/Lock.cpp`의 `Lock::WriteUnlock()`이 언락 시 소유 스레드 비트를 지우지 않는 버그 — 이 서브시스템이 이 프로젝트에서 한 번도 크로스 스레드로 쓰인 적이 없어서 지금껏 드러나지 않았던 것이다. "여러 프로젝트에서 검증된 코드"라는 전제만으로 새로운 사용 패턴(크로스 스레드)까지 안전하다고 보증되지 않는다는 걸 실제로 겪었다. 기존 `Lock.cpp`는 건드리지 않고, `std::mutex`/`condition_variable`만 쓰는 자체 `WorkerQueue`(단일 워커 스레드가 큐를 순차 소비)를 새로 만들어 대체했다.

**재실측 결과** (1-5와 동일한 6단계 매트릭스, `agents`=동시 연결 시도 수, `duration`=60초):

| agents | 개선 전 접속 성공 | 개선 전 p95/p99 | 개선 후 접속 성공 | 개선 후 p95/p99 |
|---|---|---|---|---|
| 1 | 1 | 0ms/1ms | 1 | 0ms/1ms |
| 10 | 10 | 0ms/1ms | 10 | 0ms/1ms |
| 50 | 50 | **12,051ms/22,177ms** | 50 | **0ms/1ms** |
| 100 | **72**(하드 리밋) | 34,212ms/48,412ms | **100**(전원 성공) | 0ms/1,010ms |
| 100(ramp-up 5s) | **72** | 31,572ms/45,213ms | **100**(전원 성공) | 0ms/1ms |
| 300(ramp-up 5s) | **71** | 33,467ms/47,732ms | **229**(3배 이상↑) | 20,512ms/26,994ms |

**검증 1 — syscall 레벨로 원인 제거 확인**: `strace -c` 요약에서 300-agent 기준 네트워크 스레드의 `fdatasync` 호출이 9,270회(17%)→8회(0.01%), `pwrite64`가 23,340회→11회로 급감 — SQLite 저장이 실제로 워커 스레드로 옮겨가 네트워크 스레드에서 사라졌다는 걸 syscall 카운트로 직접 확인했다.

**검증 2 — 처리량 자체가 늘었다**: 같은 60초 동안 Collector가 처리·로그로 남긴 메트릭 수가 300-agent 기준 9,625건→90,276건(약 9.4배)으로 증가 — 이전엔 대부분의 시간을 fsync 대기로 흘려보내고 있었다는 방증이다.

**발견 — 병목을 치우니 다음 병목이 드러났다(300-agent 한정)**: 처리량이 늘어나자 `strace` 요약에서 이번엔 `write` syscall이 92%(10.5초)로 압도적 1위가 됐다. 원인은 `PacketHandler::Register` 핸들러의 `std::cout << ... << std::endl` — 메트릭 1건마다 콘솔에 동기 flush가 걸린다. 100 이하 구간에서는 처리량이 낮아 드러나지 않다가, 300-agent 규모로 처리량이 커지자 처음으로 병목 순위에 올라왔다. `queue_drop`은 전 구간 0(유실 없음)이라 심각도는 낮지만, 극한 동시접속 시나리오에서의 다음 개선 후보로 기록해둔다(이번 라운드 범위 밖).

**결론**: "관찰만 하고 넘어가지 않는다"는 원칙으로 실제 코드 개선까지 진행했고, 100개 이하 동시 접속 구간의 하드 리밋(71~72개)을 완전히 해소했다(전원 접속 성공 + p95/p99 사실상 0ms 수준)는 걸 재실측으로 증명했다. 300-agent 같은 극한값에서는 개선 전보다 3배 이상 처리하지만 여전히 지연이 남아있는데, 이건 이제 SQLite가 아니라 콘솔 로깅이 원인이라는 것까지 syscall 분석으로 새로 짚어냈다 — 병목은 계층별로 순서대로 나타난다는 걸 보여주는 사례.

### 7-6. `strace -f`로 워커 스레드까지 추적 + WAL 도입 — "사라졌다"는 착시를 걷어내다

**동기**: 7-5의 `strace -c` 요약은 기본적으로 메인(네트워크) 스레드만 추적한다 — `fdatasync` 호출 수가 9,270회→8회로 급감한 걸 "문제가 해결됐다"고 읽기 쉽지만, 사실은 그 호출이 사라진 게 아니라 **워커 스레드로 옮겨가 추적 범위 밖으로 나갔을 뿐**일 수 있다. 실제로 사라졌는지, 그냥 안 보이게 됐을 뿐인지 구분하려면 새로 생긴 워커 스레드까지 추적해야 한다.

**방법**: `strace -f`(fork/clone까지 따라가기) + `-c`(요약)로 300-agent 재실측. 리눅스는 스레드가 프로세스와 PID를 공유하므로 `-f`를 붙여도 기존 pid 탐지 로직(`pgrep -P`)은 그대로 작동한다.

**측정 1 — 착시였음을 확인**: `-f`로 워커 스레드까지 잡으니 `fdatasync`가 다시 등장했다(0.01%→17.1%, 9,008회, 10.8초) — 7-5에서 "사라졌다"고 봤던 게 실은 추적 범위 밖으로 이동한 것이었을 뿐, 워커 스레드 자체는 여전히 SQLite 기본 롤백 저널의 동기 flush 비용을 그대로 지고 있었다. `fdatasync`+`pwrite64`+`fcntl`을 합치면 전체 syscall 시간의 약 25%.

**결정**: 이번엔 측정으로 뒷받침된 근거가 있으므로 `PRAGMA journal_mode=WAL` + `PRAGMA synchronous=NORMAL`을 `SqliteMetricStore` 생성자에 적용(`Storage/SqliteMetricStore.cpp`). `synchronous=NORMAL`은 매 커밋마다 fsync하지 않으므로 OS 크래시/정전 시 마지막 몇 건의 커밋을 잃을 수 있는 트레이드오프가 있지만(앱 크래시엔 WAL 자체의 원자성으로 안전), 메트릭은 계속 흘러들어오는 시계열 관측 데이터라 이 손실 범위를 감내할 수 있다고 판단했다.

**측정 2 — WAL 적용 후 같은 조건(300-agent, `strace -f`) 재실측**:

| syscall | 적용 전 | 적용 후 | 변화 |
|---|---|---|---|
| `fdatasync` | 10.81초(17.1%), 9,008회 | **0.34초(0.7%), 54회** | **-97% 시간, -99% 호출** |
| `pwrite64` | 3.05초(4.8%), 22,677회 | 4.92초(9.9%), 34,212회 | +61% (큰 저널 파일 대신 -wal 파일에 잦은 append) |
| `fcntl` | 2.31초(3.7%), 20,280회 | **12.41초(24.9%), 98,972회** | **+437%** |
| `futex` | 30.41초(48.0%), 46회 | 18.19초(36.5%), 223회 | -40% |
| `write`(콘솔 로깅) | 11.34초(17.9%) | 12.75초(25.6%) | 거의 동일 |
| **전체 syscall 시간** | **63.33초** | **49.81초** | **-21%** |
| connect_success | 246/300 | 250/300 | 유의미한 차이 없음 |
| p95/p99 | 19,179ms/25,510ms | 19,890ms/26,044ms | 유의미한 차이 없음 |

**발견 — WAL은 공짜가 아니라 트레이드오프였다**: `fdatasync`는 사실상 사라졌지만(9,008회→54회), 대신 **WAL의 공유 메모리 인덱스(`-shm`)를 관리하는 `fcntl` 락 호출이 5배 넘게 늘었다**(20,280→98,972회) — fsync 비용의 상당 부분이 락 비용으로 형태를 바꿨을 뿐이라는 뜻이다. 전체 syscall 시간은 21% 줄었지만, 300-agent 극단값에서 `connect_success`/지연시간처럼 사용자가 체감하는 지표는 눈에 띄게 개선되지 않았다 — 이 규모에서는 이미 `write`(7-5절에서 찾은 콘솔 로깅 병목)와 `futex`(락 대기)가 더 크게 지배하고 있어서, fsync 감소분만으로는 병목 순위를 못 바꿨다.

**결론**: "syscall이 안 보인다"가 "비용이 사라졌다"를 의미하지 않는다는 걸 멀티스레드 프로파일링 관점에서 실증했고(7-5 결론을 스스로 재검증), WAL을 도입해 SQLite 저널링의 핵심 비용(동기 fsync)은 실측으로 거의 제거했다. 다만 WAL도 완전한 해법이 아니라 **fsync 비용을 fcntl 락 비용으로 상당 부분 맞바꾸는 트레이드오프**라는 것, 그리고 300-agent 같은 극한값에서는 다른 병목(콘솔 로깅, 락 대기)이 이미 더 크게 자리 잡고 있어 하나를 고친다고 전체 그림이 바로 좋아지진 않는다는 것까지 데이터로 확인했다. 100개 이하 구간(7-5에서 이미 해소)에는 WAL 자체의 체감 효과는 크지 않지만, 지속적으로 메트릭이 쌓이는 장기 운영 시나리오에서 디스크 I/O 총량을 줄인다는 의미가 있다.

### 예상 질문
- **Q. 최소 권한과 syscall 최소화가 왜 다른 개념인가요?**
  → "최소 권한은 프로세스가 침해당했을 때의 피해 범위(보안)를 줄이는 거고, syscall 최소화는 커널 모드 전환 비용(성능)을 줄이는 겁니다. 완전히 독립된 축이라 같이 묶어서 설명하면 오히려 이해를 방해할 수 있어서 의도적으로 분리해서 설계·설명했습니다."
- **Q. 메모리 누수가 없다는 걸 어떻게 확신하나요?**
  → "5분간 15초 간격으로 20번 RSS를 샘플링했는데, 20개 값이 13.8MB 근처에서 완전히 고정돼 있었습니다. 만약 누수가 있었다면 시간이 지나며 RSS가 지속적으로 증가하는 추세가 보였을 텐데, 그런 패턴이 전혀 없었다는 게 실측 근거입니다. 물론 5분이라는 관측 시간의 한계는 있어서, 훨씬 긴 시간 스케일의 아주 느린 누수까지 배제한다고는 말할 수 없습니다."
- **Q. 왜 100개를 요청했는데 72개만 접속되나요? 버그 아닌가요?**
  → "버그라기보다는 60초라는 테스트 관측 시간의 한계에 가깝습니다. Collector가 단일 스레드에서 기존 접속들의 SQLite 저장(디스크 fsync)을 처리하느라 바빠서, 새 연결의 TLS 핸드셰이크를 그 시간 안에 못 끝낸 것뿐이라 더 오래 관측하면 결국 다 붙었을 가능성이 높습니다. 다만 그 자체가 지금 구조의 실질적인 처리량 한계를 보여주는 증거이기도 했고, 실제로 이 원인(SQLite 저장)을 네트워크 스레드에서 분리하자 100개 전원이 접속에 성공하는 걸로 재실측에서 확인했습니다(7-5절)."
- **Q. 이 병목을 실제로 어떻게 고치셨나요?**
  → "SQLite 저장 호출을 네트워크 스레드에서 전용 워커 스레드로 옮겼습니다. 처음엔 이 프로젝트가 이미 쓰던 `JobQueue` 서브시스템을 재사용했는데, 재실측 중 크로스 스레드 사용에서만 드러나는 락 버그로 크래시가 났습니다. 그 서브시스템은 이미 검증된 코드라 손대지 않고, 표준 라이브러리(`mutex`/`condition_variable`)만 쓰는 훨씬 작은 전용 큐로 새로 만들어 대체했습니다. 재실측으로 100개 이하 구간의 하드 리밋이 완전히 풀렸다는 것까지 확인했고, 이어서 `strace -f`로 워커 스레드 자체까지 추적해 SQLite 저널 비용이 실제로 얼마나 남아있는지 재측정한 뒤 WAL로 전환했습니다(7-6절)."
- **Q. `strace -c`에서 `fdatasync`가 안 보이길래 없어졌다고 결론 내리면 안 되나요?**
  → "네, 그게 정확히 제가 처음에 빠질 뻔한 함정이었습니다. `strace`는 기본적으로 새로 생기는 스레드를 따라가지 않아서, 워커 스레드로 옮긴 작업의 syscall은 안 보일 뿐이지 사라진 게 아니었습니다. `-f` 옵션으로 워커 스레드까지 추적하니 `fdatasync`가 다시 나타났고, 실제로 전체 syscall 시간의 약 25%를 차지하고 있었습니다. '측정 도구가 안 보여준다'와 '실제로 없다'를 구분하지 않으면 잘못된 결론에 도달할 수 있다는 걸 직접 겪은 사례입니다."
- **Q. WAL을 적용한 뒤 효과가 어땠나요?**
  → "`fdatasync` 호출은 9,008회에서 54회로 사실상 사라졌지만, 대신 WAL의 공유 메모리 인덱스를 관리하는 `fcntl` 락 호출이 5배 넘게 늘었습니다(20,280→98,972회) — WAL이 fsync 비용을 없애준 게 아니라 락 비용으로 상당 부분 형태를 바꾼 거였습니다. 전체 syscall 시간은 21% 줄었지만, 300-agent 같은 극단값에서 접속 성공 수나 지연시간처럼 사용자가 체감하는 지표엔 눈에 띄는 변화가 없었습니다 — 그 규모에선 이미 콘솔 로깅과 락 대기가 더 크게 병목이라서요. '무조건 좋아지는 개선'은 없고, 항상 어떤 비용을 다른 비용으로 맞바꾸는 트레이드오프라는 걸 실측으로 보여주는 사례라고 생각합니다."

---

## 8. APM_Console 아키텍처

### 8-1. 플러그인 아키텍처 — 왜 이렇게까지 나눴나

**배경**: 처음엔 Apm 도메인(모니터링 대시보드)과 Game 도메인(로그인/MMR)을 하나의 프로젝트에 넣는 안(A안, 단일 프로젝트 + 리플렉션 자동 등록)을 검토했다. 빠르지만 **컴파일 타임에 강제되는 격리가 없다** — 몇 달에 걸쳐 간헐적으로 진행되는 프로젝트 특성상, "미래의 내가 컨벤션을 까먹고 다른 도메인 타입을 실수로 참조"할 위험이 있다고 판단했다. 그래서 공수가 3~5배 더 들어도 **B안(도메인별 별도 프로젝트 + 런타임 플러그인 로딩)**을 선택했다.

**구조**:
```
ApmConsole.Host/            ← 실행 진입점(합성 루트), EnabledDomains로 도메인 온/오프
ApmConsole.Contracts/       ← IDomainModule 인터페이스만(의존성 최소)
Domains/
  Apm/ApmConsole.Domain.Apm/     (Sdk="Microsoft.NET.Sdk.Razor")
  Game/ApmConsole.Domain.Game/   (Sdk="Microsoft.NET.Sdk.Razor", 현재 휴면)
```
**왜 Razor Class Library인가**: 포트폴리오는 "실제로 렌더링되는 화면"이 있어야 설득력이 있다는 판단 때문에, API만이 아니라 뷰(View)까지 있어야 했다. 프로젝트를 `Sdk="Microsoft.NET.Sdk.Razor"`로 만들면 `.cshtml`이 어셈블리 안에 컴파일되어 내장되고, Host가 로드 후 `AddApplicationPart(assembly)`로 등록하면 Controller/View가 자동 인식된다(Orchard Core 같은 CMS가 쓰는 표준 플러그인 MVC 패턴).

**정적 파일(wwwroot)**: 도메인 DLL 안에 임베디드 리소스로 내장(`EmbeddedResource Include="wwwroot\**\*"`)하고, Host가 자신의 물리 `wwwroot`와 `CompositeFileProvider`로 합쳐서 서빙한다. DLL 하나로 완결되는 "진짜 플러그인" 형태.

### 8-2. `DomainLoadContext` — 타입 아이덴티티 함정 회피 (면접 단골 포인트)

.NET에서 어셈블리를 동적으로 로드할 때 흔히 겪는 함정: **같은 이름의 타입도 `AssemblyLoadContext`(ALC)가 다르면 완전히 다른 타입으로 취급된다.** 예를 들어 Host가 로드한 `IDomainModule`과, 도메인 DLL이 별도 ALC에서 로드하면서 다시 로드한 `IDomainModule`은 이름은 같아도 CLR 입장에서는 별개 타입이라, `is`/캐스팅이 전부 실패한다.

```csharp
internal class DomainLoadContext : AssemblyLoadContext
{
    private readonly AssemblyDependencyResolver _resolver;

    protected override Assembly? Load(AssemblyName assemblyName)
    {
        // 호스트(Default ALC)가 이미 로드해 둔 어셈블리는 그대로 재사용 -
        // 공유 계약 타입(IDomainModule 등)의 아이덴티티가 갈라지는 걸 막기 위함.
        var existing = AssemblyLoadContext.Default.Assemblies
            .FirstOrDefault(a => a.GetName().Name == assemblyName.Name);
        if (existing != null)
            return existing;

        // 호스트가 모르는 도메인 전용 패키지(EF Core, SQLite 등)는
        // 여기서 도메인 폴더 기준으로 정상 로드
        var assemblyPath = _resolver.ResolveAssemblyToPath(assemblyName);
        return assemblyPath != null ? LoadFromAssemblyPath(assemblyPath) : null;
    }
}
```
`AssemblyDependencyResolver`는 도메인 DLL 옆의 `.deps.json`을 읽어서, 그 도메인이 필요로 하는 관리 어셈블리/네이티브 라이브러리(예: SQLite의 `.so`)를 정확히 그 도메인 폴더 기준으로 찾아준다(Microsoft 공식 플러그인 패턴). 여기에 **"호스트가 이미 로드한 건 재사용"** 로직을 추가해서, `IDomainModule`처럼 호스트와 도메인이 공유하는 계약 타입은 항상 같은 ALC(Default)에서 온 것으로 취급되게 만들었다 — 반면 EF Core, Npgsql, SQLite처럼 호스트가 모르는 도메인 전용 패키지는 이 재사용 로직에 안 걸리고 정상적으로 도메인 폴더에서 로드된다.

### 8-3. SignalR 실시간 갱신 + 대시보드

메트릭을 DB에 저장한 **직후 곧바로** SignalR(`MetricsHub`, `/apm/hub/metrics`)로 브로드캐스트한다 — **폴링이 아니라 push**다. 저장과 푸시가 한 번의 흐름 안에서 일어난다:

```csharp
private async Task StoreAndBroadcastAsync(Metric metric)
{
    var record = new MetricRecord { /* ... */ };
    db.Metrics.Add(record);
    await db.SaveChangesAsync();
    await _hubContext.Clients.All.SendAsync("NewMetric", record);   // 저장 직후 즉시 push
}
```
대시보드(`/apm/dashboard`)의 JS 클라이언트가 `NewMetric` 이벤트를 구독해서 표와 그래프(Chart.js)를 동시에 갱신한다. 페이지 로드 시엔 최근 20건으로 초기화하고, 이후 SignalR 이벤트로 계속 갱신(최대 20개 유지, 오래된 점은 밀려남). 그래프 5개(CPU/메모리/디스크 사용률, 네트워크 RX·TX, TCP RTT+RTT 분산)와 최근 메트릭 표(11컬럼).

**왜 폴링이 아니라 push인가**: 폴링은 (1) 갱신이 없어도 계속 요청을 보내는 낭비, (2) 폴링 주기만큼의 지연이 항상 존재한다는 단점이 있다. SignalR push는 이벤트가 실제로 발생한 순간(저장 직후)에만 클라이언트로 데이터가 흘러가서 두 문제 다 해결된다.

### 예상 질문
- **Q. .NET 플러그인 아키텍처에서 타입 충돌이 왜 생기나요?**
  → ".NET의 `AssemblyLoadContext`는 어셈블리 격리 단위입니다. 같은 이름·같은 코드의 타입이라도 서로 다른 ALC에서 각각 로드되면 CLR은 이걸 별개 타입으로 취급합니다. 그래서 호스트에서 만든 인터페이스 타입과 플러그인이 별도 ALC에서 다시 로드한 같은 이름의 인터페이스 타입은 `is` 캐스팅이 실패하게 됩니다. 이걸 피하려면 공유해야 하는 계약 타입은 항상 같은 ALC(보통 Default)에서 오도록 로딩 로직을 짜야 합니다."
- **Q. 왜 폴링 대신 SignalR을 썼나요?**
  → "폴링은 변경이 없어도 계속 요청을 보내는 낭비가 있고, 폴링 주기만큼의 지연이 항상 발생합니다. SignalR은 서버가 저장 직후 곧바로 클라이언트로 push하기 때문에 불필요한 요청도 없고 지연도 최소화됩니다. 대량의 클라이언트가 자주 조회하는 대시보드류 UI에 적합한 패턴입니다."

---

## 9. 발견한 버그 전체 (7건 상세)

이 프로젝트 전체를 통틀어 "제안 단계에선 맞아 보였지만 실제 빌드/실행/렌더링에서 발견된" 버그 7건. 전부 **왜 발견됐는지(어떤 검증 방법으로)**까지 같이 기억해두면 "디버깅 어떻게 하세요" 류 질문에 구체적으로 답할 수 있다.

### 버그 1 — Razor HTML 인코딩이 JS 타임스탬프를 깨뜨림
- **증상**: 대시보드 초기 렌더링에서 차트가 빈 그래프로 뜸.
- **원인**: 서버에서 ISO 8601 문자열(`"o"` 포맷, `+09:00` 시간대 오프셋 포함)을 그대로 JS 문자열 리터럴에 심었는데, Razor의 `@` 표현식은 `<script>` 태그 안이라도 **항상 HTML 인코딩을 거친다.** `+`가 `&#x2B;`로 깨져서 `new Date(...)`가 파싱 불가능한 문자열을 받게 됨.
- **발견 과정**: **실제 렌더링된 페이지를 `curl`로 확인**하는 과정에서 발견 — 설계 검토만으로는 못 잡는 유형.
- **수정**: ISO 문자열 대신 유닉스 밀리초(순수 숫자)를 전달. 숫자는 인코딩할 특수문자가 없어서 이 문제 자체가 없다. `new Date(p.t)`는 밀리초 숫자를 그대로 받으므로 이후 코드는 수정 불필요.
- **교훈**: 이후 "실제 실행/렌더링으로 검증"을 모든 단계의 기본 절차로 삼는 계기가 됨.

### 버그 2 — TLS 핸드셰이크 모드 파라미터 누락
- **증상**: `Agent`(client) 모드가 실질적으로 동작하지 않음(미정의 동작).
- **원인**: `Collector`(accept)만 있던 시절엔 핸드셰이크가 항상 `server`로 하드코딩돼 있었다. `Agent`(connect)가 추가되며 `SessionMode`를 도입했지만, 적용된 코드에서 생성자가 `mode` 파라미터를 받지 않아 `_mode` 멤버가 초기화 리스트에서 채워지지 않은 채 남아있었다(미정의 동작) — 게다가 `handshakeType`은 계산만 해두고 실제 `async_handshake()` 호출은 여전히 `server`를 그대로 썼다.
- **발견 과정**: 코드 리뷰(직접 대조)로 발견.
- **수정**: 생성자에 `SessionMode mode` 파라미터 추가, 계산한 `handshakeType`을 실제로 `async_handshake()`에 전달.
- **검증**: `nm`으로 생성자 심볼에 `SessionMode` 파라미터가 실제로 반영됐는지 확인 후, `Collector`+`Agent` 동시 실행으로 송수신 값이 정확히 일치함을 검증.
- **참고**: 이 코드는 이후 `IPayloadSealer` 추상화가 추가되며 생성자에 `sealer` 파라미터가 하나 더 붙어서(현재 4개 파라미터), 위 코드는 "이 버그가 수정된 시점"의 스냅샷임.

### 버그 3 — `ApmSession.h`/`.cpp`가 0바이트 빈 파일로 방치
- **증상**: "빌드 성공"이 곧 "코드가 제대로 있다"는 착각을 만듦.
- **원인**: 파일이 0바이트 빈 채로 남아있었는데, **빈 `.cpp`도 정상적으로 컴파일된다** — 그래서 빌드 성공만으로는 이 문제를 잡을 수 없었다.
- **발견 과정**: `nm`으로 오브젝트 파일에 `ApmSession::` 관련 심볼이 실제로 존재하는지 직접 확인하다가 발견.
- **수정**: 내용을 다시 채우고, 이번엔 심볼 존재까지 직접 재확인.
- **교훈**: 이게 바로 "빌드 성공 ≠ 동작"이라는 이 프로젝트 전체의 핵심 검증 원칙이 생긴 계기.

### 버그 4 — Storage 계층 3종 동시 결함
- **증상**: 컴파일 자체가 안 됨.
- **원인**: `IMetricStore.h` include 오타, `Storage/CMakeLists.txt`가 빈 파일, `TimescaleMetricStore.h`에 `.cpp` 내용이 잘못 섞여 들어감 — 3가지 결함이 동시에 존재.
- **수정**: 3건 모두 직접 수정 후 클린 빌드로 재검증.

### 버그 5 — `ComputeNetworkUsage` 중복 정의
- **증상**: `redefinition of 'void ResourceCollector::ComputeNetworkUsage(...)'` 컴파일 에러.
- **원인**: 파일 맨 끝에 빈 껍데기 형태의 중복 정의가 남아있었음. 같은 적용 과정에서 `ComputeCpuUsage()` 함수 자체가 통째로 누락되기도 했다.
- **수정**: 중복 정의 삭제 + 누락된 `ComputeCpuUsage()` 복원.

### 버그 6 — TCP_INFO 연동 시 파일 누락 + 오타
- **증상**: 링크/컴파일 에러.
- **원인**: `ApmSession`/`ResilientSender` 관련 파일 4개가 누락됐고, `Agent/main.cpp`에 타입명 오타(`TcpConnectonInfo` — 철자가 틀림)까지 있었다. `.pb.h`(Protobuf 생성 헤더)도 재생성이 안 돼 있었음.
- **수정**: 파일 복원, 오타 수정, `.pb.h` 재생성.
- **검증**: 클린 빌드 + `nm` 심볼 검증 + 실제 실행 검증(`tcp=rtt:32us,var:26us,retrans:0,cwnd:10` — 루프백 환경 특성과 일치하는 값 확인).

### 버그 7 — `ManifestEmbeddedFileProvider`가 매니페스트를 못 찾음
- **증상**: 정적 리소스(JS 파일 등)가 404 — 그런데 예외가 조용히 삼켜져서 원인 파악이 어려웠음.
- **원인**: `GetManifestResourceNames()`로 확인해보니 파일 자체는 정확히 임베딩됐는데, `GenerateEmbeddedFilesManifest=true`가 만들어야 할 매니페스트 리소스가 생성 안 됨 — `ManifestEmbeddedFileProvider` 생성자가 던지는 `InvalidOperationException`을 `Program.cs`의 `catch` 블록이 "이 도메인엔 정적 자산이 없다"는 정상 케이스로 오인해서 조용히 삼켰다. 근본 원인은 `FrameworkReference="Microsoft.AspNetCore.App"`가 런타임 타입만 제공하고, 매니페스트를 빌드 시점에 실제로 생성하는 MSBuild 타겟은 별도 NuGet 패키지(`Microsoft.Extensions.FileProviders.Embedded`)에 있었다는 것.
- **발견 과정**: 별도 콘솔 앱으로 `GetManifestResourceNames()`/`ManifestEmbeddedFileProvider.GetFileInfo()`를 직접 호출해서 실측으로 원인 확정. 이후 실제 렌더링 결과를 **`curl`로 확인**하는 과정에서 최종 검증.
- **수정**: `PackageReference Include="Microsoft.Extensions.FileProviders.Embedded"` 추가.

### 예상 질문
- **Q. 버그를 발견하는 본인만의 원칙이 있나요?**
  → "'빌드 성공은 동작을 증명하지 않는다'는 원칙입니다. 실제로 빈 파일이 정상 컴파일되는 걸 겪은 뒤로는, 심볼이 실제 존재하는지 `nm`으로 확인하고, 웹 쪽은 실제 렌더링된 결과를 `curl`로 직접 확인하는 걸 기본 절차로 삼았습니다. '설계상 맞다'와 '실제로 동작한다' 사이에는 항상 검증이 필요하다는 걸 여러 번 겪으면서 체화한 원칙입니다."
- **Q. 예외가 조용히 삼켜져서 디버깅이 어려웠던 경험 있나요?**
  → "`ManifestEmbeddedFileProvider` 케이스가 정확히 그랬습니다. 특정 예외(`InvalidOperationException`)를 '정상적으로 있을 수 있는 케이스'로 오인해서 캐치 블록이 조용히 삼키고 있었는데, 실제로는 진짜 설정 오류였습니다. 이후로는 catch 블록을 쓸 때 '이 예외가 정말 무해한 케이스만 걸러내는지'를 더 의심하게 됐습니다."

---

## 10. 테스트 전략

### 무엇이 있나
- **C++(`APM_Agent/tests/AesGcmTests.cpp`, GoogleTest) 9개**:
  - `AesGcmCipher`(저수준) 5개: 왕복 정상 복원 / 매 호출마다 다른 nonce 생성 / 태그 변조 시 거부 / 암호문 변조 시 거부 / 다른 키로 복호화 불가.
  - `AesGcmPayload`(와이어 포맷·`IPayloadSealer`) 4개: 왕복 / `[Nonce 12B][ciphertext][Tag 16B]` 크기 검증 / 변조된 와이어 거부 / 인터페이스 경유 동작(구체 타입이 아니라 `IPayloadSealer*`로 호출해도 문제없는지).
- **.NET(`APM_Console/tests/`, xUnit) 8개**:
  - `ReadExactAsyncTests` 4개: 한 번에 전부 수신 / 여러 조각으로 분할 수신(커스텀 `ChunkedStream`으로 TCP 부분 수신 재현) / 조기 종료 시 `null` / 길이 0이면 빈 배열.
  - `DecryptAndParseTests` 4개: 정상 왕복(필드값 복원) / 태그 변조·암호문 변조·잘못된 키 전부 `AuthenticationTagMismatchException`으로 거부.

### 테스트 가능하게 만들기 위한 리팩터링
`MetricsReceiverService`의 `ReadExactAsync`/`DecryptAndParse`는 원래 `private`이었는데, `internal`로 가시성을 넓히고(`InternalsVisibleTo`로 테스트 프로젝트에만 노출) `DecryptAndParse`는 인스턴스 필드(`_aesKey`, 생성자가 파일 I/O로 채움) 대신 **키를 파라미터로 받는 `static` 메서드**로 전환했다 — DI로 서비스 전체를 구성하지 않아도 순수 로직만 테스트할 수 있게. 이건 "테스트 가능한 설계"가 저절로 되는 게 아니라 **의도적으로 리팩터링해야 얻어지는 것**이라는 걸 보여주는 사례.

### 왜 이 두 개만 골랐나 (전체 커버리지가 목표가 아니었던 이유)
암호화 관련 코드(가장 실수하기 쉽고, 실수하면 조용히 실패하는 영역)와, TCP 프레이밍(부분 수신이라는 비결정적 상황을 재현해야 하는 영역)을 우선했다. 전체 코드베이스에 대한 100% 커버리지를 목표로 하지 않은 이유:
1. 포트폴리오 프로젝트 특성상 시간 예산이 제한적이었고, "핵심 위험 지점 우선"이 합리적인 우선순위였다.
2. Agent↔Collector 구간이 AES-GCM으로 통일되면서, `AesGcmCipher`/`AesGcmPayload` 테스트 9개가 **실제 운영 경로 전체(두 구간 모두)**를 커버하게 됐다 — 남아있는 `AriaCipher`/`HmacUtil`/`SecurePayload`는 실행 경로에서 빠진 참고용 코드라 테스트를 추가하지 않았다.
3. `ApmSession::ProcessAccumulated`(프레이밍 재조립 로직)처럼 실제 소켓에 강결합된 부분은 아직 자동 테스트가 없다 — 이건 인지하고 있는 갭이고, 우선순위상 뒤로 미룬 것.

### 예상 질문
- **Q. 왜 100% 테스트 커버리지를 목표로 하지 않았나요?**
  → "포트폴리오 프로젝트라 시간 예산이 정해져 있었고, 모든 코드를 동일한 강도로 테스트하는 것보다 '실수하면 조용히 실패하고 파급력이 큰' 영역(암호화, 네트워크 프레이밍)을 우선하는 게 더 합리적인 투자라고 판단했습니다. 어디에 테스트가 없는지도 명확히 인지하고 있고, 그 이유(아직 리팩터링이 안 돼서 테스트하기 어렵다 등)도 설명할 수 있습니다."
- **Q. 테스트하기 어려운 코드를 어떻게 테스트 가능하게 만드나요?**
  → "`MetricsReceiverService`의 경우, 인스턴스 필드에 의존하던 로직을 파라미터를 받는 static 메서드로 바꿔서 DI 컨테이너 전체를 구성하지 않아도 순수 로직만 단위 테스트할 수 있게 만들었습니다. 접근 제한자는 private에서 internal로 최소한만 넓히고, InternalsVisibleTo로 테스트 프로젝트에만 노출해서 공개 API 표면은 그대로 유지했습니다."

---

## 11. 예상 면접 질문 모음 (통합)

각 섹션에 흩어진 질문들을 주제별로 다시 모았다. 실전에선 이 목록만 훑어봐도 충분하도록.

**아키텍처/설계 철학**
- 왜 이 코어를 다른 프로젝트(APM)에도 재사용했나 → 1절
- Standalone Asio가 뭔가, Boost.Asio와 차이는 → 2절
- 크로스플랫폼이라면서 게임 콘텐츠는 Windows에서 왜 검증 안 했나 / Windows 포팅에서 가장 까다로웠던 부분 → 2절
- TCP가 스트림이라는 게 무슨 뜻인가, 왜 프레이밍이 필요한가 → 4절

**보안**
- 왜 두 암호를 섞어 쓰다 통일했나 → 3절
- AEAD가 CBC+HMAC보다 왜 더 안전한가 → 3절
- 패딩 오라클 공격이 뭔가 → 3절
- IV/Nonce 재사용이 왜 위험한가 → 3절
- TLS만으로 안 되나, 왜 페이로드를 또 암호화하나 → 3절

**저장/데이터**
- 왜 C++은 컴파일 타임, .NET은 런타임으로 저장소를 고르나 → 5절

**리소스 수집**
- MemFree 대신 MemAvailable을 쓰는 이유 → 6절
- TCP 품질을 왜 커널 값으로 조회하나 → 6절

**성능/권한**
- 최소 권한과 syscall 최소화가 왜 다른가 → 7절
- 메모리 누수 없다는 걸 어떻게 확신하나 → 7절

**플러그인/웹**
- .NET 플러그인 아키텍처에서 타입 충돌이 왜 생기나 → 8절
- 왜 폴링 대신 SignalR인가 → 8절

**디버깅/검증 철학**
- 버그를 발견하는 본인만의 원칙이 있나 → 9절
- 예외가 조용히 삼켜진 경험이 있나 → 9절

**테스트**
- 왜 100% 커버리지를 목표로 안 했나 → 10절
- 테스트하기 어려운 코드를 어떻게 테스트 가능하게 만드나 → 10절

**포지셔닝(가장 중요)**
- 이 프로젝트를 만들게 된 계기가 뭔가 → **"네트워크 프로그래밍과 동시성 제어를 깊게 익히고 싶어서 직접 구현했다"** (0절 네이밍 원칙 반드시 준수 — "게임" 언급 금지)

---

## 12. 용어 미니 사전

| 용어 | 뜻 |
|---|---|
| **IOCP** | Windows의 비동기 I/O 완료 통지(Completion Port) 메커니즘. |
| **epoll** | Linux의 비동기 I/O 준비 통지(readiness notification) 메커니즘. |
| **AEAD** | Authenticated Encryption with Associated Data — 암호화와 인증(무결성 검증)을 하나의 연산으로 처리하는 암호 방식(예: AES-GCM). |
| **Encrypt-then-MAC** | 암호화한 뒤 그 암호문에 대해 별도로 MAC(메시지 인증 코드)을 계산·검증하는 패턴. 복호화 전에 MAC부터 검증해야 안전하다. |
| **패딩 오라클 공격** | CBC 모드에서 패딩 유효성 정보가 새어나가는 걸 악용해 평문을 복원하는 공격. |
| **KISA** | 한국인터넷진흥원(Korea Internet & Security Agency) — ARIA 등 국내 표준 암호를 제정. |
| **jiffies** | Linux 커널이 시간을 세는 단위(HZ 설정에 따라 달라짐). `/proc/stat`의 CPU 시간이 이 단위로 누적된다. |
| **AssemblyLoadContext(ALC)** | .NET에서 어셈블리를 격리해서 로드하는 단위. 플러그인 아키텍처의 핵심 메커니즘. |
| **Razor Class Library(RCL)** | `.cshtml`(Razor 뷰)과 정적 파일을 어셈블리(DLL) 안에 컴파일해 내장할 수 있게 해주는 .NET 프로젝트 타입. |
| **AEAD Nonce vs IV** | 같은 개념(암호화마다 달라져야 하는 값)을 GCM 계열에서는 보통 Nonce, CBC 계열에서는 IV라고 부른다. |

---

## 13. 라이브 데모 실행 순서

면접에서 실제 시연을 요청받을 경우를 대비한 실행 순서(상세 시나리오는 `Docs/WEBSERVER_TEST_SCENARIO.md` 참고).

1. **인증서/키 생성**(최초 1회):
   ```bash
   cd APM_Agent
   bash certs/generate_test_cert.sh
   bash certs/generate_agent_collector_key.sh
   bash certs/generate_webserver_key.sh
   cd ../APM_Console
   bash certs/generate_webserver_cert.sh
   ```
2. **빌드**:
   ```bash
   cd APM_Agent && cmake -S . -B build -DAPM_STORAGE_BACKEND=SQLite && cmake --build build
   cd ../APM_Console && dotnet build ApmConsole.sln
   ```
3. **실행 순서**(터미널 3개):
   ```bash
   # 1) APM_Console (웹 대시보드) 먼저 - Collector가 연결할 대상
   cd APM_Console && dotnet run --project src/ApmConsole.Host/ApmConsole.Host.csproj

   # 2) Collector (반드시 APM_Agent/ 안에서 - certs/ 상대경로 때문)
   cd APM_Agent && ./build/Collector

   # 3) Agent
   cd APM_Agent && ./build/Agent
   ```
4. 브라우저로 `/apm/dashboard` 접속 — 5초마다 그래프/표가 갱신되는 걸 실시간으로 보여줄 수 있다.
5. **복원력 시연**(선택): `Collector`를 강제 종료했다가 재시작 — `Agent` 로그에 `[ResilientSender] queued (size=N)`가 쌓이다가, `Collector` 재시작 즉시 버퍼링된 데이터부터 순서대로 재전송되는 걸 보여줄 수 있다.
