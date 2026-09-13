## 2026-09-13 — Qt/MFC 트랙 5단계 설계·코드 제안: QtCharts 실시간 갱신

### 배경

사용자가 "직접 작성하며 학습"하겠다고 했다가, 이어서 "이전처럼 SESSION_LOG에 예제 코드 포함해서 작성해달라"고 명시 요청 — 2~4단계와 같은 방식(제안만, 실제 파일은 미작성 — 원칙 2)으로 되돌아감.

계획 §3-3 ④: "시계열 데이터를 일정 주기로 갱신하며 오래된 점을 버린다. 갱신 주기와 데이터 보관 개수를 어떻게 정했는지가 질문거리가 된다." 이 요구사항을 그대로 만족하려면 지금 구조에 없는 것 하나를 추가해야 한다 — **자동 주기 갱신**이다. 지금까지는 "초기 1회 + 새로고침 버튼 클릭"만 있고, 타이머로 자동 갱신하는 경로가 없다.

### 설계 결정 1 — 자동 새로고침 주기: `APM_Agent`의 수집 주기(5초)에 맞춘다

`APM_Agent/Agent/main.cpp`를 직접 확인: `MetricScheduler scheduler(ioContext, std::chrono::seconds(5), ...)` — Agent는 5초마다 지표를 수집해서 저장한다. 대시보드가 이보다 뜸하게 갱신하면 새 데이터를 반영하는 데 지연이 생기고, 이보다 훨씬 잦으면 같은 데이터를 헛되이 반복 조회하게 된다. **그래서 새로고침 타이머 주기도 5초로 맞춘다** — "왜 5초인가"에 "백엔드 수집 주기와 일치시켰다"고 답할 수 있는 지점.

구현은 `QTimer`를 만들어 기존 `OnRefreshClicked()`(이미 있는 "상태 표시 + `emit RefreshRequested()`" 경로)에 연결 — 버튼과 타이머가 **같은 슬롯을 공유**한다(새 경로를 따로 안 만듦).

### 설계 결정 2 — 차트의 "버퍼 크기"는 테이블의 `LIMIT 20`과 별개로 정한다

테이블은 "최신 20개 스냅샷을 통째로 보여주기"(4단계까지의 정책)이고, 차트는 "화면에 최근 몇 개의 점을 스크롤시키며 보여줄 것인가"라는 **별개의 질문**이다. 둘을 같은 값으로 묶지 않고 차트 전용 상수 `kMaxPoints = 30`을 새로 둔다 — 5초 주기 × 30개 = **최근 2분 30초 구간**을 보여준다는 뜻. 이게 백엔드의 retention 정책(Metrics 30일/AlertRecord 180일 — "오래된 데이터를 언제·왜 버리는가")과 계획서가 요구한 연결 지점이다: **저장 계층의 보존 기간 결정과 화면 표시 계층의 버퍼 크기 결정은 "얼마나 오래 된 데이터가 아직 쓸모 있는가"라는 같은 질문에 대한, 계층만 다른 답**이다.

### 설계 결정 3 — 매 갱신마다 "가장 최근 값 1개"만 차트에 누적한다

`MetricsWorker::FetchLatestMetrics()`는 매번 최신 20개를 통째로 다시 가져온다(4단계까지: 매번 테이블 전체 교체). 차트는 다르게 쓴다 — `samples`의 0번째(= `ORDER BY Id DESC`이므로 가장 최신 행) **한 점만** 취해서 차트에 이어붙이고, `kMaxPoints`를 넘으면 가장 오래된 점을 지운다. 같은 시그널(`MetricsReady`)을 받아도 테이블은 "전체 교체", 차트는 "한 점 누적"으로 **다르게 소비**하는 셈 — 소비자마다 같은 데이터를 다른 방식으로 써도 된다는 것을 보여주는 지점이기도 하다.

### 설계 결정 4 — 시그널 팬아웃(fan-out): 시그널 하나에 슬롯 두 개

`MetricsWorker::MetricsReady`에는 이미 `MainWindow::PopulateMetricsTable`이 연결돼 있다. 여기에 `MetricsChartWidget::AppendLatest`를 **추가로** 연결한다 — Qt의 signal/slot은 `connect()`를 여러 번 호출하면 시그널 하나가 연결된 슬롯을 전부(연결한 순서대로) 호출한다. 콜백 함수 포인터 하나만 담을 수 있는 전통적인 콜백 방식과 다른 지점이라, "왜 콜백 대신 signal/slot인가"(계획 §3-3 ①)에 대한 구체적 근거가 하나 더 생긴다.

### 제안 — MetricsChartWidget.h (신규)

```cpp
#pragma once
#include <QWidget>
#include <QVector>

#include "MetricsRepository.h"

class QChartView;
class QLineSeries;
class QValueAxis;

// step 5 : CPU/메모리 사용률 실시간 시계열 차트.
// QChart 자체는 QGraphicsWidget이라 화면에 못 올린다 - QChartView(QWidget 파생)에 얹는다.
class MetricsChartWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MetricsChartWidget(QWidget* parent = nullptr);

public slots:
    // MetricsWorker::MetricsReady에 그대로 연결한다. samples[0]이 최신값
    // (FetchLatestMetrics가 "ORDER BY Id DESC"이므로)이라 그 한 점만 누적하고,
    // kMaxPoints를 넘으면 가장 오래된 점을 버린다.
    void AppendLatest(const QVector<MetricsSample>& samples);

private:
    QChartView* _chartView;
    QLineSeries* _cpuSeries;
    QLineSeries* _memSeries;
    QValueAxis* _axisX;
    QValueAxis* _axisY;
    qint64 _nextX = 0;
};
```

### 제안 — MetricsChartWidget.cpp (신규)

```cpp
#include "MetricsChartWidget.h"

#include <algorithm>

#include <QChart>
#include <QChartView>
#include <QLineSeries>
#include <QPainter>
#include <QValueAxis>
#include <QVBoxLayout>

namespace
{
// 화면에 유지할 최대 점 개수. Agent가 5초마다 수집하므로(APM_Agent/Agent/main.cpp의
// MetricScheduler), 30개면 최근 2분 30초 구간을 보여준다 - 백엔드 retention 정책과
// 같은 사고방식: "화면에 얼마나 오래 된 데이터를 보여줄 가치가 있는가"를 명시적으로 정한 값.
constexpr int kMaxPoints = 30;
}

MetricsChartWidget::MetricsChartWidget(QWidget* parent)
    : QWidget(parent)
{
    _cpuSeries = new QLineSeries(this);
    _cpuSeries->setName("CPU %");

    _memSeries = new QLineSeries(this);
    _memSeries->setName("Mem %");

    auto* chart = new QChart();
    chart->addSeries(_cpuSeries);
    chart->addSeries(_memSeries);
    chart->setTitle("실시간 사용률");

    _axisX = new QValueAxis(this);
    _axisX->setLabelFormat("%d");
    _axisX->setTitleText("샘플 순번");
    _axisX->setRange(0, kMaxPoints);

    _axisY = new QValueAxis(this);
    _axisY->setRange(0, 100);
    _axisY->setTitleText("%");

    chart->addAxis(_axisX, Qt::AlignBottom);
    chart->addAxis(_axisY, Qt::AlignLeft);
    _cpuSeries->attachAxis(_axisX);
    _cpuSeries->attachAxis(_axisY);
    _memSeries->attachAxis(_axisX);
    _memSeries->attachAxis(_axisY);

    _chartView = new QChartView(chart, this);
    _chartView->setRenderHint(QPainter::Antialiasing);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(_chartView);
    setLayout(layout);
}

void MetricsChartWidget::AppendLatest(const QVector<MetricsSample>& samples)
{
    if (samples.isEmpty())
        return;

    // samples[0]이 최신값(FetchLatestMetrics가 "ORDER BY Id DESC"로 가져오므로).
    const MetricsSample& latest = samples.first();
    const double memPercent = latest.memTotalBytes > 0
        ? latest.memUsedBytes * 100.0 / latest.memTotalBytes
        : 0.0;

    _cpuSeries->append(_nextX, latest.cpuUsagePercent);
    _memSeries->append(_nextX, memPercent);
    ++_nextX;

    if (_cpuSeries->count() > kMaxPoints)
    {
        _cpuSeries->removePoints(0, _cpuSeries->count() - kMaxPoints);
        _memSeries->removePoints(0, _memSeries->count() - kMaxPoints);
    }

    // 스크롤하는 것처럼 보이도록 X축 범위를 항상 "최근 kMaxPoints개" 구간으로 맞춘다.
    const qint64 rangeStart = std::max<qint64>(0, _nextX - kMaxPoints);
    _axisX->setRange(rangeStart, rangeStart + kMaxPoints);
}
```

**변경 사유**: `QChart`는 `QGraphicsWidget` 파생이라(일반 `QWidget`이 아님) 직접 레이아웃에 못 넣는다 — `QChartView`(진짜 `QWidget`)에 얹어야 화면에 놓을 수 있다. 축을 두 시리즈가 공유하도록 `chart->addAxis()` 한 번 + 각 시리즈의 `attachAxis()` 두 번씩 호출 — Qt Charts는 이 절차를 안 밟으면(`createDefaultAxes()`를 안 쓰는 이상) 축이 자동으로 안 생긴다. `QPainter::Antialiasing`을 쓰려면 `<QPainter>`를 명시적으로 include해야 한다(3단계 `<QMetaType>` 누락 사고 이후 확립한 원칙 — 쓰는 건 전이적 include에 기대지 않고 직접 include).

### 제안 — MainWindow.h (수정)

**수정 전** (파일 전문):
```cpp
#pragma once
#include <QMainWindow>
#include <QVector>
#include <QThread>

#include "MetricsRepository.h"

class QLabel;
class QPushButton;
class QTableView;
class MetricsWorker;
class MetricsTableModel;
class AlertsTableModel;

// step 4 : 데이터(모델)와 표시(뷰)를 분리한다. QTableWidget → QTableView + Model.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    // 워커 스레드의 Refresh() 슬롯에 큐 연결된다. emit 후 즉시 반환한다.
    void RefreshRequested();

private slots:
    void OnRefreshClicked();
    void OnWorkerInitialized(bool ok);
    void OnRefreshFailed(const QString& reason);
    void PopulateMetricsTable(const QVector<MetricsSample>& samples);
    void PopulateAlertTable(const QVector<AlertSample>& alerts);

private:
    void SetStatus(const QString& text);

private:
    QThread _workerThread;
    MetricsWorker* _worker = nullptr;

    MetricsTableModel* _metricsModel = nullptr;
    AlertsTableModel* _alertsModel = nullptr;
    QTableView* _metricsView = nullptr;
    QTableView* _alertsView = nullptr;
    QPushButton* _refreshButton = nullptr;
    QLabel* _statusLabel = nullptr;
};
```

**수정 후**:
```cpp
#pragma once
#include <QMainWindow>
#include <QVector>
#include <QThread>

#include "MetricsRepository.h"

class QLabel;
class QPushButton;
class QTableView;
class QTimer;
class MetricsWorker;
class MetricsTableModel;
class AlertsTableModel;
class MetricsChartWidget;

// step 5 : QTimer로 주기 자동 새로고침 + 실시간 차트(MetricsChartWidget) 추가.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    // 워커 스레드의 Refresh() 슬롯에 큐 연결된다. emit 후 즉시 반환한다.
    void RefreshRequested();

private slots:
    void OnRefreshClicked();
    void OnWorkerInitialized(bool ok);
    void OnRefreshFailed(const QString& reason);
    void PopulateMetricsTable(const QVector<MetricsSample>& samples);
    void PopulateAlertTable(const QVector<AlertSample>& alerts);

private:
    void SetStatus(const QString& text);

private:
    QThread _workerThread;
    MetricsWorker* _worker = nullptr;
    QTimer* _refreshTimer = nullptr;

    MetricsTableModel* _metricsModel = nullptr;
    AlertsTableModel* _alertsModel = nullptr;
    QTableView* _metricsView = nullptr;
    QTableView* _alertsView = nullptr;
    MetricsChartWidget* _chartWidget = nullptr;
    QPushButton* _refreshButton = nullptr;
    QLabel* _statusLabel = nullptr;
};
```

**변경 사유**: `QTimer`/`MetricsChartWidget` 전방 선언 추가, 멤버 2개(`_refreshTimer`, `_chartWidget`) 추가. `QTimer`는 포인터 멤버라 전방 선언으로 충분(3~4단계에서 확립한 관례 그대로 — 값 멤버만 완전한 정의 필요).

### 제안 — MainWindow.cpp (수정, 발췌 — 바뀌는 부분만)

**수정 전** (include + 생성자 전문):
```cpp
#include "MainWindow.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>
#include <QWidget>

#include "AlertsTableModel.h"
#include "MetricsTableModel.h"
#include "MetricsWorker.h"

namespace
{
// APM_Console의 appsettings.json Apm:ConnectionString과 같은 파일을 가리켜야 한다.
// 이 체크아웃 기준 절대경로를 하드코딩 - Console 쪽도 지금 절대경로 하드코딩
// 상태라 이식성 문제가 새로 생기는 건 아니다. 배포판을 만들 때(§6 8단계)
// 커맨드라인 인자나 설정 파일로 뺄 것.
const QString kConsoleDbPath = "/home/shkim/dev/APM/APM_Console/webserver_apm.db";
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("APM Qt Dashboard - step 4 (Model/View)");

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    _refreshButton = new QPushButton("새로고침", central);
    _statusLabel = new QLabel("초기화 중...", central);

    _metricsModel = new MetricsTableModel(this);
    _metricsView = new QTableView(central);
    _metricsView->setModel(_metricsModel);
    _metricsView->horizontalHeader()->setStretchLastSection(true);
    _metricsView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    _alertsModel = new AlertsTableModel(this);
    _alertsView = new QTableView(central);
    _alertsView->setModel(_alertsModel);
    _alertsView->horizontalHeader()->setStretchLastSection(true);
    _alertsView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    layout->addWidget(_refreshButton);
    layout->addWidget(_statusLabel);
    layout->addWidget(_metricsView);
    layout->addWidget(_alertsView);
    setCentralWidget(central);

    connect(_refreshButton, &QPushButton::clicked, this, &MainWindow::OnRefreshClicked);

    // 워커를 만들고 워커 스레드로 옮긴다. 이 시점 이후 워커의 슬롯은 워커 스레드에서 실행된다.
    _worker = new MetricsWorker(kConsoleDbPath);
    _worker->moveToThread(&_workerThread);

    // 스레드가 끝나면 워커를 그 스레드에서 안전하게 삭제한다.
    connect(&_workerThread, &QThread::finished, _worker, &QObject::deleteLater);

    // UI → 워커 (스레드가 다르므로 자동으로 Queued Connection)
    connect(this, &MainWindow::RefreshRequested, _worker, &MetricsWorker::Refresh);

    // 워커 → UI (역시 Queued. 슬롯 본문은 UI 스레드에서 실행됨)
    connect(_worker, &MetricsWorker::Initialized, this, &MainWindow::OnWorkerInitialized);
    connect(_worker, &MetricsWorker::MetricsReady, this, &MainWindow::PopulateMetricsTable);
    connect(_worker, &MetricsWorker::AlertsReady, this, &MainWindow::PopulateAlertTable);
    connect(_worker, &MetricsWorker::RefreshFailed, this, &MainWindow::OnRefreshFailed);

    _workerThread.start();

    // 워커가 워커 스레드로 옮겨진 뒤 Initialize()가 그 스레드에서 실행되도록 큐에 넣는다.
    QMetaObject::invokeMethod(_worker, "Initialize", Qt::QueuedConnection);
}
```

**수정 후**:
```cpp
#include "MainWindow.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "AlertsTableModel.h"
#include "MetricsChartWidget.h"
#include "MetricsTableModel.h"
#include "MetricsWorker.h"

namespace
{
// APM_Console의 appsettings.json Apm:ConnectionString과 같은 파일을 가리켜야 한다.
// 이 체크아웃 기준 절대경로를 하드코딩 - Console 쪽도 지금 절대경로 하드코딩
// 상태라 이식성 문제가 새로 생기는 건 아니다. 배포판을 만들 때(§6 8단계)
// 커맨드라인 인자나 설정 파일로 뺄 것.
const QString kConsoleDbPath = "/home/shkim/dev/APM/APM_Console/webserver_apm.db";

// Agent의 수집 주기(APM_Agent/Agent/main.cpp의 MetricScheduler)와 맞춘다 -
// 더 뜸하면 새 데이터 반영이 늦고, 더 잦으면 같은 데이터를 헛되이 반복 조회한다.
constexpr int kRefreshIntervalMs = 5000;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("APM Qt Dashboard - step 5 (QtCharts)");

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    _refreshButton = new QPushButton("새로고침", central);
    _statusLabel = new QLabel("초기화 중...", central);

    _chartWidget = new MetricsChartWidget(central);

    _metricsModel = new MetricsTableModel(this);
    _metricsView = new QTableView(central);
    _metricsView->setModel(_metricsModel);
    _metricsView->horizontalHeader()->setStretchLastSection(true);
    _metricsView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    _alertsModel = new AlertsTableModel(this);
    _alertsView = new QTableView(central);
    _alertsView->setModel(_alertsModel);
    _alertsView->horizontalHeader()->setStretchLastSection(true);
    _alertsView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    layout->addWidget(_refreshButton);
    layout->addWidget(_statusLabel);
    layout->addWidget(_chartWidget);
    layout->addWidget(_metricsView);
    layout->addWidget(_alertsView);
    setCentralWidget(central);

    connect(_refreshButton, &QPushButton::clicked, this, &MainWindow::OnRefreshClicked);

    // 워커를 만들고 워커 스레드로 옮긴다. 이 시점 이후 워커의 슬롯은 워커 스레드에서 실행된다.
    _worker = new MetricsWorker(kConsoleDbPath);
    _worker->moveToThread(&_workerThread);

    // 스레드가 끝나면 워커를 그 스레드에서 안전하게 삭제한다.
    connect(&_workerThread, &QThread::finished, _worker, &QObject::deleteLater);

    // UI → 워커 (스레드가 다르므로 자동으로 Queued Connection)
    connect(this, &MainWindow::RefreshRequested, _worker, &MetricsWorker::Refresh);

    // 워커 → UI (역시 Queued. 슬롯 본문은 UI 스레드에서 실행됨)
    connect(_worker, &MetricsWorker::Initialized, this, &MainWindow::OnWorkerInitialized);
    connect(_worker, &MetricsWorker::MetricsReady, this, &MainWindow::PopulateMetricsTable);
    // 시그널 팬아웃 - 같은 MetricsReady를 차트도 받아서, 최신 값 1개만 누적한다.
    connect(_worker, &MetricsWorker::MetricsReady, _chartWidget, &MetricsChartWidget::AppendLatest);
    connect(_worker, &MetricsWorker::AlertsReady, this, &MainWindow::PopulateAlertTable);
    connect(_worker, &MetricsWorker::RefreshFailed, this, &MainWindow::OnRefreshFailed);

    _refreshTimer = new QTimer(this);
    _refreshTimer->setInterval(kRefreshIntervalMs);
    connect(_refreshTimer, &QTimer::timeout, this, &MainWindow::OnRefreshClicked);

    _workerThread.start();

    // 워커가 워커 스레드로 옮겨진 뒤 Initialize()가 그 스레드에서 실행되도록 큐에 넣는다.
    QMetaObject::invokeMethod(_worker, "Initialize", Qt::QueuedConnection);
}
```

**변경 사유**: `<QTimer>`, `"MetricsChartWidget.h"` include 추가. `kRefreshIntervalMs`(5000) 상수 추가 — 설계 결정 1 근거를 주석으로 남김. `_chartWidget` 생성 + 레이아웃에 추가(테이블 위, 상태 라벨 아래 — 가장 먼저 보이게). `MetricsReady` 시그널에 `connect()`를 한 번 더 호출해 `_chartWidget->AppendLatest`를 추가 연결(설계 결정 4). `_refreshTimer`는 `QTimer::timeout`을 **새 슬롯을 만들지 않고 기존 `OnRefreshClicked()`에 연결** — 버튼과 타이머가 같은 진입점을 공유하므로 "수동 새로고침"과 "자동 새로고침"의 동작이 100% 동일함이 보장된다(코드 중복 없음). 타이머는 `_workerThread.start()` 이전에 생성만 해두고 `start()`는 호출하지 않음 — DB가 아직 안 열렸는데 타이머가 먼저 돌면 낭비이므로, 아래 `OnWorkerInitialized`에서 최초 연결 성공 시점에 `start()`.

**수정 전** (`OnWorkerInitialized` 함수 전문):
```cpp
void MainWindow::OnWorkerInitialized(bool ok)
{
    if (!ok)
    {
        SetStatus("DB 열기 실패 - stderr 확인");
        return;
    }
    SetStatus("연결됨");
    emit RefreshRequested();
}
```

**수정 후**:
```cpp
void MainWindow::OnWorkerInitialized(bool ok)
{
    if (!ok)
    {
        SetStatus("DB 열기 실패 - stderr 확인");
        return;
    }
    SetStatus("연결됨");
    emit RefreshRequested();
    _refreshTimer->start();
}
```

**변경 사유**: DB 연결이 실제로 성공한 시점에만 자동 갱신 타이머를 돌린다 — 연결 실패 상태에서 5초마다 계속 실패하는 조회를 반복하지 않도록.

### 제안 — CMakeLists.txt (수정)

**수정 전**:
```cmake
find_package(Qt6 REQUIRED COMPONENTS Widgets Sql)

add_executable(APM_QtDashboard
    main.cpp
    MainWindow.cpp
    MainWindow.h
    MetricsRepository.cpp
    MetricsRepository.h
    MetricsWorker.cpp
    MetricsWorker.h
    MetricsTableModel.cpp
    MetricsTableModel.h
    AlertsTableModel.cpp
    AlertsTableModel.h
)

target_link_libraries(APM_QtDashboard PRIVATE Qt6::Widgets Qt6::Sql)
```

**수정 후**:
```cmake
find_package(Qt6 REQUIRED COMPONENTS Widgets Sql Charts)

add_executable(APM_QtDashboard
    main.cpp
    MainWindow.cpp
    MainWindow.h
    MetricsRepository.cpp
    MetricsRepository.h
    MetricsWorker.cpp
    MetricsWorker.h
    MetricsTableModel.cpp
    MetricsTableModel.h
    AlertsTableModel.cpp
    AlertsTableModel.h
    MetricsChartWidget.cpp
    MetricsChartWidget.h
)

target_link_libraries(APM_QtDashboard PRIVATE Qt6::Widgets Qt6::Sql Qt6::Charts)
```

**변경 사유**: `Charts` 컴포넌트가 없으면 `Qt6::Charts` 임포트 타겟이 없어서 2단계 때 겪었던 것과 같은 "target was not found" 에러가 난다(이번엔 미리 반영). 신규 파일 2개 추가.

### 검증 (미검증 — 사용자가 직접 작성/빌드 후 확인)

1. `find_package(Qt6 REQUIRED COMPONENTS Widgets Sql Charts)` 성공 여부 — `qt6-charts-dev`가 이미 설치돼 있어(이번 세션에 확인) 성공할 것으로 예상.
2. 클린 빌드 성공(`MetricsChartWidget` moc 포함).
3. 실행 시 차트가 뜨고, 5초마다(수동 버튼 없이도) CPU/Mem 선이 오른쪽으로 한 칸씩 늘어나는지.
4. 점 개수가 30개를 넘으면 왼쪽(오래된) 점이 스크롤되어 사라지는지(X축 범위가 같이 이동하는지).
5. DB에 실제 행이 3개뿐이라(현재 `webserver_apm.db` 상태) 매 갱신마다 `samples.first()`가 항상 같은(가장 최근 커밋된) 행일 수 있음 — 이 경우 CPU/Mem 값이 안 바뀌고 X축만 늘어나는 것처럼 보이는 게 정상. 값 자체가 바뀌는 걸 보려면 `APM_Console`/`APM_Agent`를 같이 띄워 실제로 새 지표가 쌓이게 해야 함.

### 결정 사항

문서 제안만 — `APM_QtDashboard/`의 실제 소스는 사용자가 직접 작성(원칙 2, [[feedback_claude_md_rule2_scope]]). 작성 중 나오는 오타/컴파일 에러는 요청 시 수정 지원. 완료되면 `WORK_STATUS.md` 갱신 후 커밋 + push.
