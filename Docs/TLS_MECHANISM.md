# TLS_MECHANISM.md

> `APM_Agent`의 TLS(Phase 2) 작업 중 다룬 인증서 생성 스크립트와 TLS/공개키 암호 개념을 정리한 참고 문서.
> 관련 파일: `APM_Agent/certs/generate_test_cert.sh`, `APM_Agent/Common/ApmSession.h/.cpp`.

## 1. `generate_test_cert.sh` 한 줄씩 설명

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

openssl req -x509 -newkey rsa:2048 \
    -keyout server.key -out server.crt \
    -days 365 -nodes \
    -subj "/CN=localhost"

echo "생성 완료: certs/server.key, certs/server.crt"
```

### `#!/usr/bin/env bash`
셔뱅(shebang) 줄. 이 파일을 `./generate_test_cert.sh`처럼 직접 실행하면, OS가 이 줄을 보고 "bash로 해석해서 실행하라"고 판단한다. `env bash`는 시스템마다 다를 수 있는 bash의 실제 경로를 `PATH`에서 찾아 쓰는 방식(고정 경로 `/bin/bash`보다 이식성이 좋음).

### `set -euo pipefail`
안전장치 3개를 한 번에 켜는 관용구:
- `-e` : 어떤 명령이 실패(0이 아닌 종료 코드)하면 스크립트를 즉시 중단. 없으면 앞 명령이 실패해도 다음 줄이 계속 실행됨.
- `-u` : 정의되지 않은 변수를 참조하면 에러로 취급 (오타 방지).
- `-o pipefail` : `A | B`처럼 파이프로 이었을 때, 기본은 마지막 명령(B)의 성공 여부만 보는데 이 옵션을 켜면 A가 실패해도 파이프 전체를 실패로 처리.

### `cd "$(dirname "$0")"`
`$0`은 이 스크립트 자신의 경로. `dirname`으로 디렉토리 부분만 뽑아 그 폴더로 이동. 사용자가 어느 위치에서 스크립트를 실행하든 항상 스크립트가 있는 폴더(`certs/`) 안에 결과물이 생기도록 하기 위함.

### `openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt -days 365 -nodes -subj "/CN=localhost"`
실제 인증서 생성 명령. 옵션별:
- `openssl req` : 인증서 관련 작업을 하는 OpenSSL 하위 명령.
- `-x509` : 원래 `req`는 CSR(인증서 서명 *요청*)만 만드는 게 기본인데, 이 옵션을 주면 별도 CA 없이 바로 **자체 서명된 인증서**를 만든다.
- `-newkey rsa:2048` : 2048비트 RSA 키 쌍을 새로 생성.
- `-keyout server.key` : 개인키 저장 경로.
- `-out server.crt` : (공개) 인증서 저장 경로.
- `-days 365` : 유효기간 1년.
- `-nodes`("no DES") : 개인키를 암호화(패스프레이즈)하지 않음. 서버가 무인으로 시작할 때마다 사람이 비밀번호를 입력할 필요가 없어짐.
- `-subj "/CN=localhost"` : 국가/조직명 등을 대화형으로 물어보는 과정을 건너뛰고 한 번에 지정. `CN`(Common Name)은 "이 인증서가 누구 것인가"를 나타내는 핵심 필드 — 로컬 테스트라 `localhost`로 고정.

---

## 2. TLS는 공개키 암호인가?

정확히는 **공개키(비대칭) 암호와 대칭키 암호를 함께 쓰는 하이브리드 방식**이다.

### 2-1. 대칭키 암호 (Symmetric)
- 암호화·복호화에 **같은 키 하나**를 쓴다. A가 이 키로 잠그면 B도 같은 키로 열어야 한다.
- 빠르다 — 대량 데이터를 실시간으로 주고받기에 적합(이 프로젝트의 Phase 3 **ARIA-CBC**가 이 방식).
- 문제: 이 키를 어떻게 안전하게 나눠 갖느냐. 네트워크로 그냥 보내면 도청당하는 순간 끝난다.

### 2-2. 공개키 암호 (Asymmetric)
- 수학적으로 짝을 이루는 **개인키**(나만 보관)와 **공개키**(누구에게나 공개 가능) 두 개를 쓴다. `server.key`가 개인키, `server.crt`(안에 공개키 포함)가 공개키에 해당.
- 공개키로 암호화한 건 짝인 개인키로만 복호화 가능(또는 반대로 개인키 서명을 공개키로 검증).
- 장점: 사전에 만난 적 없는 두 상대가 도청자가 있어도 안전하게 비밀을 나눠 가질 방법을 제공.
- 단점: 계산이 무거워서 느림 — 이걸로 모든 트래픽을 암호화하면 성능이 나쁨.

### 2-3. TLS의 조합 방식
1. **핸드셰이크(연결 시작 시 1회)**: 클라이언트 접속 → 서버가 인증서(`server.crt`, 공개키 포함) 제시 → 클라이언트가 검증(정식 서비스는 CA 서명 체인 확인, 자체 서명은 이 단계가 사실상 생략/수동 신뢰 — 3번 참고).
2. 공개키 암호(또는 Diffie-Hellman류 키 교환)로 **이번 연결에서만 쓸 임시 대칭키(세션 키)**를 도청자 없이 합의. 서버의 개인키(`server.key`)는 이 단계에서 "내가 이 인증서의 진짜 주인"임을 증명하는 데 쓰임.
3. **이후 실제 데이터 통신**은 합의된 대칭키로 암호화(보통 AES 계열, OpenSSL이 협상) — 여기서부터 빠른 대칭키 방식으로 전환.

핵심: **공개키 암호는 "처음에 안전하게 비밀키를 나눠 갖기 위해서"만 잠깐 쓰이고, 실제 대량 데이터 전송은 그렇게 만든 대칭키로 처리**된다. "TLS = 공개키로 전부 암호화"는 흔한 오해.

---

## 3. 그런데 왜 Phase 3(ARIA-CBC+HMAC)이 또 필요한가

TLS가 이미 전송 구간을 대칭키로 암호화하는데 애플리케이션이 또 ARIA-CBC로 암호화하는 이유는 **다른 계층을 보호하기 위해서**다. TLS는 "네트워크 구간"만 보호한다(로드밸런서/프록시를 거치면 그 지점에서 TLS가 해제되기도 함). ARIA-CBC는 **페이로드(민감 필드) 자체**를 애플리케이션 레벨에서 한 번 더 감싸서, TLS가 어딘가에서 해제되더라도 그 필드는 여전히 보호되게 한다. 또한 ARIA는 KISA(국내) 표준 암호라, 국내 공공/금융권 요구사항 대응이라는 의미도 있다(TLS 기본 암호인 AES와는 별개 트랙).

---

## 4. "자체 서명"의 의미와 한계

정식 인증서는 브라우저/OS가 이미 신뢰하는 CA(인증기관)가 서명해줘서, 클라이언트가 "이 CA가 보증하니 믿는다"는 식으로 자동 신뢰한다. `server.crt`는 **우리가 우리 스스로에게 서명**한 것이라 아무도 이 인증서를 공식적으로 신뢰하지 않는다. 그래서 테스트 클라이언트(Phase 2-5)는:
- 검증을 건너뛰거나(`verify_none`, 테스트 전용) 명시적으로 지정하거나,
- 이 특정 인증서를 미리 신뢰 목록에 넣어줘야

핸드셰이크가 통과한다 — Phase 2-4/2-5에서 실제로 맞닥뜨릴 지점.

**보안 원칙**: `server.key`(개인키)는 자체 서명 테스트 키라 실제 보안 가치는 없지만, git에는 커밋하지 않는다(`APM_Agent/.gitignore`). 스크립트(`generate_test_cert.sh`)만 커밋해서 누구든 재실행으로 재현 가능하게 관리 — "비밀키를 커밋하지 않고 재현 가능하게 만드는" 습관 자체가 포트폴리오에서 좋은 인상을 준다.

---

## 5. 이 프로젝트의 모든 인증서/키 종류와 "PC 간 이동" 문제 (2026-07-18 정리)

여러 PC를 오가며 작업하다 보면 "다른 PC에서 만든 cert/key가 이 PC엔 없는데 작업해도 되는가?"라는 의문이 자연스럽게 생긴다. 결론: **문제 없다** — 이유를 아래에 정리한다.

### 5-1. 프로젝트에 존재하는 키 자료 전체 목록

| 파일 | 생성 스크립트 | 성격 | 무엇을 보호하는가 |
|---|---|---|---|
| `APM_Agent/certs/server.key`/`server.crt` | `generate_test_cert.sh` | 자체 서명 TLS 인증서(공개키 암호, RSA 2048) | Agent↔Collector TLS 핸드셰이크 (Phase 2) |
| `APM_Agent/certs/aria.key`/`hmac.key` | `generate_payload_keys.sh` | 무작위 대칭키(각 32바이트) | Agent↔Collector 페이로드 ARIA-CBC 암호화 + HMAC 무결성 (Phase 3) |
| `APM_Agent/certs/webserver_aes.key` | `generate_webserver_key.sh` | 무작위 대칭키(32바이트) | Collector→WebServer AES-256-GCM 암호화 |
| `APM_Console/certs/webserver.key`/`webserver.crt` (3단계, 미적용) | `generate_webserver_cert.sh` | 자체 서명 TLS 인증서 | WebServer가 Collector의 TLS 연결을 받는 쪽 |

### 5-2. 왜 다른 PC의 값과 똑같을 필요가 없는가

1. **전부 그 자리에서 새로 만드는 값이다** — CA에게 발급받는 정식 인증서(예: 도메인 인증서)처럼 "한 번 발급받아 여러 환경에 배포"하는 게 아니라, `openssl req -x509`(자체 서명)와 `openssl rand`(무작위 대칭키)로 **실행할 때마다 완전히 새 값**이 나온다. "이 값이어야만 한다"는 정답이 애초에 없다.
2. **통신하는 두 프로세스가 같은 환경 안에 있다** — Agent/Collector/(예정된) WebServer는 전부 이 프로젝트 기준 **같은 PC(WSL) 안에서** 서로 접속한다. TLS·ARIA·AES 키는 "그 순간 서로 통신하는 두 프로세스끼리만" 값이 일치하면 되고, 그 조건은 "같은 PC에서 같은 스크립트를 방금 실행했다"는 것만으로 충족된다. 다른 PC의 예전 값을 알 필요가 없다.
3. **그래서 git에 안 올린다(`.gitignore`)** — 반대로 말하면, 만약 이 값들이 "여러 PC가 공유해야 하는 진짜 비밀"이었다면 애초에 안전한 별도 경로(예: 비밀 관리 시스템)로 배포해야지 git으로 옮기는 것 자체가 보안 사고다. 이 프로젝트는 그런 상황이 아니라서 스크립트만 커밋하고 산출물은 각자 재생성하는 쪽을 택했다.

### 5-3. 그렇다면 "정말로" PC 이동이 문제가 되는 경우는?

아래 조건에 해당하면 이야기가 달라지는데, **이 프로젝트는 둘 다 해당 없음**을 확인해 둔다:
- **키로 이미 암호화해서 저장해 둔 데이터가 있고, 그 데이터를 다른 PC에서 복호화해야 하는 경우** — 이 프로젝트의 키들은 전부 **전송 구간(in-transit)** 암호화용이고, `apm_metrics.db`(저장 데이터)는 평문 저장이라 이 문제 자체가 없다. `apm_metrics.db`는 데이터라서 실제로 git에 커밋되어 PC 간 그대로 이동한다(cert/key와는 반대로 취급).
- **정식 CA가 발급한 인증서라 재발급 시 비용/시간이 드는 경우** — 전부 자체 서명이라 재발급 비용이 `./script.sh` 한 번뿐.

### 5-4. 결론 — 새 PC에서 처음 작업할 때 할 일

```bash
cd APM_Agent/certs
chmod +x generate_test_cert.sh generate_payload_keys.sh generate_webserver_key.sh
./generate_test_cert.sh      # server.key / server.crt
./generate_payload_keys.sh   # aria.key / hmac.key
./generate_webserver_key.sh  # webserver_aes.key
```
이 세 스크립트만 실행하면 그 PC 안에서 Agent/Collector가 정상적으로 통신할 수 있는 자기완결적인 키 세트가 만들어진다.
