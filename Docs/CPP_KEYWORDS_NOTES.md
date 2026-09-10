# C++ 키워드 정리 — 자주 헷갈리는 것들

> Qt/MFC 트랙 진행 중 반복해서 헷갈린 키워드를 정리한다. 새로 헷갈리는 게 나오면 여기에 추가한다.
> 최초 작성 2026-09-10 (`constexpr`, `explicit`).

---

## constexpr

### 한 줄 요약

**"이 값/함수를 컴파일 타임 상수 표현식으로 쓸 수 있다"** 는 지정자. `const`(불변)보다 강한 조건이다.

### 자주 하는 오해

| 오해 | 실제 |
| --- | --- |
| "`constexpr` = 무조건 컴파일 타임에 실행된다" | **변수**는 맞다(초기화식이 컴파일 타임에 안 풀리면 컴파일 에러). **함수**는 아니다 — "상수 표현식 문맥에서 쓸 수 있게 허용"일 뿐, 런타임 인자로 부르면 그냥 런타임 함수로 실행된다. |
| "`constexpr`은 그냥 더 빠른 `const`다" | 목적이 다르다. `const`는 "안 바꾼다", `constexpr`은 "컴파일 타임 상수여야 한다". `constexpr` 변수는 `const`를 함의하지만 역은 아니다. |
| "아무 타입이나 `constexpr`로 만들 수 있다" | 리터럴 타입(literal type)만 가능하다 — `int`/`double`/포인터/enum, 그리고 `constexpr` 생성자를 가진 클래스. `QString`, `std::string`, `std::vector`는 런타임 생성자를 쓰므로 **불가**. |

### 실제 동작

```cpp
int r = getFromSomewhere();
const int a = r;          // OK — const는 "안 바꾼다", 값은 런타임에 정해짐
constexpr int b = r;      // 컴파일 에러 — 초기화식이 상수 표현식이 아님
constexpr int c = 20;     // OK

constexpr int square(int x) { return x * x; }
constexpr int d = square(5);   // 컴파일 타임에 계산
int e = square(r);             // 런타임에 일반 함수로 호출 — 이것도 정상
```

컴파일 타임 상수만 들어갈 수 있는 자리:

```cpp
constexpr int kSize = 20;
int arr[kSize];                 // 배열 크기
std::array<int, kSize> arr2;    // 템플릿 인자
static_assert(kSize > 0);       // static_assert
```

### 관련 키워드

- **`consteval`** (C++20): "항상 컴파일 타임에 실행" 강제. `constexpr` 함수의 강화판.
- **`inline constexpr`** (C++17): 헤더에 상수를 두고 여러 번역 단위가 **같은** 정의를 공유하게 함(ODR 안전). 헤더 상수의 표준 관용구.

### 링크(linkage)와의 관계 — 별개 규칙

네임스페이스 스코프의 `const`/`constexpr` 변수는 **기본이 internal linkage**(그 `.cpp`에서만 보임)다. 이건 `constexpr`의 의미가 아니라 C++가 `const`에서 물려받은 링크 기본값이고, `constexpr` 변수는 `const`이므로 같이 적용받는다. → 파일 스코프에 `constexpr int k = 20;`을 그냥 둬도 다른 파일과 이름 충돌이 안 난다(익명 네임스페이스로 안 감싸도 됨). 반면 자유 **함수**는 기본이 external linkage라, 파일 로컬로 만들려면 `static`이나 익명 네임스페이스가 필요하다.

### 이 프로젝트에서

```cpp
// APM_QtDashboard/MetricsWorker.cpp
namespace
{
constexpr int kFetchLimit = 20;      // int, 리터럴 타입 → constexpr OK
}

// APM_QtDashboard/MainWindow.cpp
namespace
{
const QString kConsoleDbPath = "/home/shkim/dev/APM/APM_Console/webserver_apm.db";
//  ^^^^^ constexpr 아님 — QString은 힙 할당 런타임 생성자를 가짐. const가 최선.
//  이 객체의 초기화는 프로그램 시작 시점에 일어난다.
}
```

---

## explicit

### 한 줄 요약

**생성자/변환 연산자가 "암시적 타입 변환"에 쓰이는 것을 막는** 지정자. 링크·가시성과 무관.

### 자주 하는 오해

| 오해 | 실제 |
| --- | --- |
| "`explicit`은 심볼을 전역으로 노출하는 키워드다" | 전혀 아니다. 노출/링크는 `extern`, (모듈의) `export`, 헤더 선언의 영역. `explicit`은 **타입 변환 규칙**만 건드린다. |
| "생성자에 붙이나 마나 똑같다" | 인자 1개로 호출 가능한 생성자는 `explicit`이 없으면 **암시적 변환 연산자를 겸한다**. 붙이면 그 경로가 막힌다. |

### 실제 동작

```cpp
struct Foo { Foo(int x); };           // explicit 아님
void take(Foo f);
take(42);        // OK — int 42 → Foo 암시적 변환
Foo f = 42;      // OK — 복사 초기화

struct Bar { explicit Bar(int x); };
void take2(Bar b);
take2(42);       // 컴파일 에러
Bar b = 42;      // 컴파일 에러
Bar b(42);       // OK — 직접 초기화
take2(Bar(42));  // OK — 명시적 생성
```

**기본 규칙**: 인자 1개로 호출 가능한 생성자에는 일단 `explicit`을 붙인다. 암시적 변환을 **의도한** 경우(드묾)에만 뺀다.
"기본 인자 때문에 인자 0개로도 호출 가능"한 생성자도, 인자 1개로 호출 가능하면 변환 후보가 되므로 `explicit` 대상이다.

### 관련 키워드

- **`explicit operator bool()`**: 변환 연산자에도 붙는다. `if (obj)` 같은 문맥에서만 허용하고 `int x = obj;` 같은 건 막는다.
- **`explicit(bool)`** (C++20): 조건부 explicit. 템플릿에서 조건에 따라 켜고 끈다.

### 이 프로젝트에서

```cpp
// APM_QtDashboard/MetricsRepository.h
explicit MetricsRepository(const QString& dbPath);
//  QString 하나가 실수로 MetricsRepository(DB 연결 객체 전체)로 조용히 변환되는 걸 막음

// APM_QtDashboard/MainWindow.h
explicit MainWindow(QWidget* parent = nullptr);
//  Qt 관례. QWidget* → MainWindow 암시적 변환 차단. uic 생성 코드도 항상 이렇게 함
```

---

## 두 키워드 나란히 비교

| | `constexpr` | `explicit` |
| --- | --- | --- |
| 붙는 대상 | 변수, 함수, 생성자 | 생성자, 변환 연산자 |
| 하는 일 | 컴파일 타임 상수 표현식 자격 부여 | 암시적 변환 차단 |
| 링크/가시성 | **무관** (단, `const`라서 변수는 internal linkage 기본값을 받음) | **완전 무관** |
| 안 붙이면 | 런타임에 값이 정해질 수 있음 | 인자 1개 생성자가 암시적 변환 연산자를 겸함 |
