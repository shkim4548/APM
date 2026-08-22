# APM_Viewer에서 쓴 MFC 매크로 정리

지금까지 작성한 코드(`ApmViewerApp.h/.cpp`, `ApmViewerDlg.h/.cpp`, `pch.h`)에 등장한
매크로를 종류별로 정리. "왜 필요한지"와 "우리 코드 어디서 쓰였는지"를 같이 적음.

---

## 1. 메시지 맵 관련

MFC의 자체 메시지 디스패치 테이블을 만드는 매크로 세트. 자세한 동작 원리(매크로 확장 결과,
런타임 디스패치 흐름)는 `SESSION_LOG.md`의 "2026-08-22 — 메시지 맵 매크로 설명" 항목 참고.
여기서는 각 매크로가 뭘 하는지만 짧게.

| 매크로 | 위치(헤더) | 역할 |
|---|---|---|
| `DECLARE_MESSAGE_MAP()` | `.h`의 클래스 선언 안 | 이 클래스가 메시지 맵을 갖는다고 선언(`GetMessageMap()` 오버라이드 + `messageMap` 정적 멤버 선언만, 정의는 안 함) |
| `BEGIN_MESSAGE_MAP(클래스, 부모클래스)` | `.cpp` | `messageMap`/`_messageEntries[]`를 실제로 정의하기 시작. 두 번째 인자(부모클래스)가 "여기서 못 찾으면 이쪽으로 체이닝"할 대상 |
| `END_MESSAGE_MAP()` | `.cpp` | `_messageEntries[]` 배열을 종료 마커로 닫음 |
| `ON_WM_TIMER()` | `.cpp`, BEGIN/END 사이 | `WM_TIMER` 메시지 → `void OnTimer(UINT_PTR nIDEvent)` 연결 항목 추가 |
| `ON_WM_DESTROY()` | `.cpp`, BEGIN/END 사이 | `WM_DESTROY` 메시지 → `void OnDestroy()` 연결 항목 추가 |
| `afx_msg` | `.h`, 핸들러 함수 선언 앞 | 컴파일에는 아무 영향 없는 빈 매크로(`#define afx_msg`). "이건 메시지 핸들러다"라는 표식용 - 사람이 읽을 때, 그리고 예전 ClassWizard가 소스를 스캔할 때 식별하는 용도 |

**쓰인 곳**:
- `ApmViewerApp.h/.cpp`: `DECLARE_MESSAGE_MAP()` + `BEGIN_MESSAGE_MAP(CApmViewerApp, CWinApp) ... END_MESSAGE_MAP()` (핸들러는 없음 - 빈 메시지 맵)
- `ApmViewerDlg.h/.cpp`: 위 표 전부 사용. `OnTimer`/`OnDestroy` 두 핸들러 등록

**주의(직접 쓰면서 겪은 함정)**: `ON_WM_TIMER()`/`ON_WM_DESTROY()`가 매핑해주는 함수 이름(`OnTimer`,
`OnDestroy`)과 시그니처(`UINT_PTR nIDEvent` / 인자 없음)는 고정임 - 임의로 바꾸면 안 됨.
빠뜨리면 컴파일 에러가 아니라 "타이머는 도는데 아무 반응 없음" 같은 조용한 런타임 버그가 됨.

---

## 2. 데이터 교환(DDX) 관련

| 매크로/함수 | 위치 | 역할 |
|---|---|---|
| `DDX_Control(pDX, nID, control)` | `DoDataExchange()` 안 | 리소스 ID(`IDC_LIST_METRICS` 등)로 정의된 다이얼로그 컨트롤과, C++ 멤버 변수(`m_listMetrics` 등)를 연결. 다이얼로그가 뜰 때 `GetDlgItem(nID)`로 찾은 핸들을 `control`에 자동으로 물려줌 - 이게 없으면 `m_listMetrics.InsertColumn(...)` 같은 호출이 아직 비어있는 `CListCtrl` 객체에 대고 하는 셈이라 동작 안 함 |

**쓰인 곳**: `ApmViewerDlg.cpp`의 `DoDataExchange()`
```cpp
DDX_Control(pDX, IDC_LIST_METRICS, m_listMetrics);
DDX_Control(pDX, IDC_STATIC_STATUS, m_staticStatus);
```
(참고로 `DDX_`로 시작하는 함수가 여러 개 있음 - `DDX_Text`, `DDX_Check` 등은 컨트롤 자체가
아니라 그 안의 *값*을 멤버 변수와 동기화하는 용도라 우리 코드에서 쓴 `DDX_Control`과는 목적이
다름. 우리는 컨트롤 객체 자체를 다루므로 `DDX_Control`만 필요)

---

## 3. 문자열/유니코드 관련

| 매크로 | 역할 |
|---|---|
| `_T("문자열")` | 프로젝트가 Unicode(`CharacterSet=Unicode`, 우리 vcxproj 설정)면 `L"문자열"`(`wchar_t`)로, ANSI 빌드면 그냥 `"문자열"`(`char`)로 바뀜. MFC/ATL 프로젝트가 두 빌드 모드를 다 지원하던 시절의 흔적 - 요즘은 거의 항상 Unicode라 사실상 `L"..."`과 동일하게 동작하지만, 관례상 계속 씀 |

**쓰인 곳**: `ApmViewerDlg.cpp`의 `InsertColumn`/`SetWindowText`/`Format` 등 문자열 리터럴을
넘기는 모든 곳(`_T("시각")`, `_T("상태: 연결됨")` 등, `SESSION_LOG.md` 완성 예제 참고)

---

## 4. 프로젝트 설정 매크로 (`pch.h`)

| 매크로 | 역할 |
|---|---|
| `VC_EXTRALEAN` | `<afxwin.h>` include 전에 정의하면 거의 안 쓰는 옛날 API들을 빼고 헤더를 컴파일해서 빌드를 좀 더 가볍게/빠르게 함 |
| `_ATL_CSTRING_EXPLICIT_CONSTRUCTORS` | `CString`이 다른 문자열 타입으로부터 암묵적으로 변환되는 걸 막음(의도치 않은 타입 변환 버그 예방) - MFC 위저드가 관례적으로 항상 켜두는 옵션 |

이 둘은 함수처럼 동작하는 매크로가 아니라, `<afxwin.h>`가 내부적으로 `#ifdef`로 참조하는
"기능 스위치"용 매크로임 - `#include <afxwin.h>` **이전에** 정의돼 있어야 의미가 있음.

---

## 참고: 아직 안 나온 것

`ON_COMMAND`, `ON_BN_CLICKED` 같은 버튼/메뉴 클릭 계열 매크로는 아직 안 씀(지금 다이얼로그엔
버튼이 없음). 나중에 버튼을 추가하게 되면 그때 같은 표 형식으로 추가하면 됨.
