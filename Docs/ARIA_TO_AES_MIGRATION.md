# ARIA_TO_AES_MIGRATION.md

> `APM_Agent`의 Agent↔Collector 페이로드 암호화를 ARIA-256-CBC+HMAC-SHA256에서 AES-256-GCM으로 통일한 배경과, 그 전에 구현·검증했던 ARIA-CBC+HMAC 경험을 정리한 문서(2026-07-20).
> 관련 파일: `APM_Agent/Common/AriaCipher.h/.cpp`, `HmacUtil.h/.cpp`, `SecurePayload.h/.cpp`(현재 실행 경로에서는 빠졌지만 코드는 남아있음), `AesGcmCipher.h/.cpp`, `AesGcmPayload.h/.cpp`(현재 실행 경로).

---

## 1. 원래 구조 — 왜 구간마다 다른 암호를 썼는가

`APM_Agent`는 두 개의 암호화 구간을 갖고 있었다.

| 구간 | 원래 암호 | 이유 |
|---|---|---|
| Agent ↔ Collector | ARIA-256-CBC + HMAC-SHA256 | KISA(국내) 표준 블록 암호를 실무처럼 다뤄본 경험을 만들고 싶었음 |
| Collector ↔ APM_Console | AES-256-GCM | .NET(`System.Security.Cryptography.AesGcm`)과의 상호운용, AEAD의 구조적 안전성 |

두 구간 모두 `IPayloadSealer`(`Seal`/`Open`) 인터페이스로 추상화되어 있어서, `ApmSession`/`ResilientSender`는 어느 쪽 암호를 쓰는지 몰라도 되는 구조였다 — 실제 코드가 다른 게 아니라 세션 생성 시점에 주입하는 구현체만 달랐다.

---

## 2. ARIA-256-CBC + HMAC-SHA256 구현 경험

### 2-1. 왜 CBC 단독으로는 안 되는가 — 패딩 오라클

CBC 모드는 **기밀성만 제공하고 무결성은 보장하지 않는다.** 복호화 시 마지막 블록의 PKCS7 패딩이 유효한지 여부가 별도로 검증되는데, 공격자가 암호문을 조금씩 조작해가며 "패딩이 유효했는지"를 서버 응답(에러 종류, 응답 시간 등)으로 추측할 수 있으면, 이를 반복해 평문을 한 바이트씩 복원할 수 있다 — 이것이 **패딩 오라클 공격**이다.

### 2-2. 대응 — Encrypt-then-MAC

CBC를 단독으로 쓰지 않고 HMAC-SHA256으로 감쌌다:

- **암호화**: `AriaCipher` — OpenSSL EVP API(`EVP_aria_256_cbc`)로 ARIA-256-CBC 암복호화.
- **무결성**: `HmacUtil` — OpenSSL 3.x `EVP_MAC` API로 HMAC-SHA256 계산. 태그 비교는 `CRYPTO_memcmp`(상수 시간 비교)로 수행해 타이밍 공격을 방지.
- **결합**: `SecurePayload` — 위 둘을 `[IV(16B)][ciphertext][HMAC tag(32B)]` 와이어 포맷으로 결합. 암호화 키와 MAC 키는 서로 다른 키를 사용(동일 키를 암호화·인증에 함께 쓰는 건 알려진 암호학적 실수).
- **순서 강제**: `SecurePayload::Open()`은 **HMAC 검증을 먼저 수행하고, 통과한 경우에만 ARIA 복호화(및 패딩 검증)를 시도**한다. 변조된 암호문은 CBC 복호화 루틴에 도달하기도 전에 HMAC 단계에서 균일하게 거부되므로, 공격자가 패딩 유효성에 대한 정보를 얻을 방법이 없다.
- **IV 관리**: 매 암호화 호출마다 `RAND_bytes()`로 새 IV를 생성. `AriaCipher::Encrypt()`는 IV를 출력 전용 파라미터로만 받기 때문에 호출자가 IV를 직접 지정하거나 재사용할 방법이 구조적으로 없다.

### 2-3. 검증 이력

- 별도 테스트 프로그램으로 암복호화 왕복 일치, 잘못된 IV 시 실패 확인(2026-07-10).
- 암호문을 변조한 뒤 `Open()`을 호출하면 `AriaCipher::Decrypt()`가 아니라 `HmacUtil::Verify()` 단계에서 먼저 예외가 발생하는 것을 확인 — HMAC 검증이 실제로 복호화보다 먼저 실행됨을 증명.
- 실제 파이프라인에서 대칭키를 일부러 불일치시켜(HMAC 키만 일치·ARIA 키만 불일치) 같은 현상(HMAC 통과 → ARIA 복호화 단계에서 실패 → 연결 종료)이 재현됨을 확인 — Encrypt-then-MAC의 두 계층이 각각 올바르게 동작함을 실제 배선으로 실증(2026-07-15).
- 다만 이 검증은 전부 **1회성 수동 테스트**였다 — `AesGcmTests.cpp`(GoogleTest)처럼 저장소에 남아 반복 실행되는 자동 회귀 테스트는 끝내 추가되지 않았다(아래 4번 참고).

---

## 3. AES-256-GCM으로 통일한 이유

### 3-1. 구조적 안전성 — AEAD는 조합 실수 자체가 없다

GCM은 AEAD(Authenticated Encryption with Associated Data)다. 암호화와 동시에 인증 태그가 계산되고, 복호화 시 태그 검증에 실패하면 평문이 아예 나오지 않는다. `SecurePayload`처럼 "암호화 따로, MAC 따로 만들어서 특정 순서로 검증"하는 코드를 직접 짤 필요가 없다 — 그 조합·순서를 실수할 여지 자체가 라이브러리 레벨에서 사라진다. CBC+HMAC은 "올바르게 구현하면" 안전하지만, GCM은 "잘못 조합할 방법이 없어서" 안전하다는 차이가 있다.

### 3-2. 단일 암호 경로 — 검증 부담이 절반이 된다

두 구간에 서로 다른 암호를 쓰면 검증·유지보수 대상도 두 배가 된다. 실제로 이 프로젝트에서 `AesGcmCipher`/`AesGcmPayload`는 GoogleTest 9개(`tests/AesGcmTests.cpp`)로 검증돼 있었지만, 더 복잡한 `AriaCipher`/`HmacUtil`/`SecurePayload`는 자동 테스트가 없는 비대칭이 있었다. 하나로 합치면 이미 있던 테스트 9개가 Agent↔Collector, Collector↔Console 두 구간 모두를 커버하게 된다.

### 3-3. 트레이드오프 — 잃은 것

ARIA는 KISA 표준 국산 블록 암호라 국내 공공/금융권 문맥에서 의미가 있었는데, 실행 경로에서는 더 이상 쓰이지 않는다. 이 경험 자체를 지우지 않기 위해 이 문서를 남겼고, `AriaCipher`/`HmacUtil`/`SecurePayload` 코드도 삭제하지 않고 `APM_Agent/Common/`에 참고용으로 남겨뒀다(빌드는 되지만 `Agent`/`Collector`의 실행 경로에서는 더 이상 참조하지 않음).

---

## 4. 실제로 무엇이 바뀌었나

| 항목 | 이전 | 이후 |
|---|---|---|
| Agent↔Collector 암호 | `SecurePayload`(ARIA-256-CBC+HMAC-SHA256) | `AesGcmPayload`(AES-256-GCM) |
| Agent↔Collector 키 파일 | `certs/aria.key` + `certs/hmac.key` | `certs/agent_collector_aes.key` |
| 키 생성 스크립트 | `certs/generate_payload_keys.sh` | `certs/generate_agent_collector_key.sh` |
| Collector↔Console 암호/키 | AES-256-GCM / `webserver_aes.key` (변경 없음) | 동일 |
| `IPayloadSealer` 추상화 | 두 구간이 서로 다른 구현체 주입 | 두 구간 모두 `AesGcmPayload`를 주입하지만, 인터페이스 자체는 유지(다른 구현체로 교체 가능한 구조는 그대로) |
| `AriaCipher`/`HmacUtil`/`SecurePayload` | 실행 경로에서 사용 | 코드는 남지만 실행 경로에서 참조 제거(참고용) |

수정된 파일별 diff(수정 전/후 전문 + 사유)는 `SESSION_LOG.md`의 "2026-07-20 — Agent↔Collector 구간을 ARIA-CBC+HMAC → AES-256-GCM으로 통일" 항목에 기록되어 있다. 마이그레이션 후 `Collector`+`Agent`를 함께 실행해 TLS 핸드셰이크·AES-GCM 복호화·`[Collector] metric stored: ...]` 로그로 실제 수치가 정상 수신됨을 재검증했다(2026-07-20).

---

## 5. 요약 비교

| | ARIA-256-CBC + HMAC-SHA256 | AES-256-GCM |
|---|---|---|
| 방식 | Encrypt-then-MAC (수동 조합) | AEAD (라이브러리가 통합 처리) |
| 무결성 검증 실수 가능성 | 있음(순서를 잘못 짜면 패딩 오라클 재발) | 구조적으로 없음 |
| 국내 표준 여부 | KISA 표준(ARIA) | 아님(범용 국제 표준) |
| .NET과의 상호운용성 | 별도 P/Invoke·라이브러리 필요 | `System.Security.Cryptography.AesGcm`으로 기본 지원 |
| 이 프로젝트에서의 테스트 커버리지 | 수동 검증만, 자동 테스트 없음 | GoogleTest 9개 |
| 이 프로젝트에서의 현재 상태 | 코드 보존, 실행 경로 제외 | 두 구간 모두 사용 중 |
