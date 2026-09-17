## 2026-09-14 — Qt/MFC 트랙: 차트 갱신을 폴링→발행-구독(Collector→Qt 푸시)으로 재설계

### 배경

`MetricsChartWidget::AppendLatest`가 "새 데이터가 생겼을 때"가 아니라 "갱신 이벤트가 발생할 때"마다 한 점을 찍는다는 문제(수동 새로고침 버튼과 자동 타이머가 같은 슬롯을 타므로, 실제로 새 지표가 없어도 중복 점이 찍힐 수 있음)를 사용자와 논의 → "Collector가 각자에게 쏴줘도 된다"는 제안 → 발행-구독(pub/sub) 패턴으로 재설계하기로 확정(AskUserQuestion).

**설계 방향(사용자 확인 완료)**:
- Collector가 **발행자(Publisher)**, Qt가 **구독자(Subscriber)**. 로컬 Unix domain socket 위의 최소 pub/sub — 브로커 없음, 토픽 1개("새 지표 생김"), 구독자가 그 순간 없으면 유실(재생 없음).
- **페이로드 없음, 핑만 보낸다** — Collector가 실제 지표 값을 JSON으로 실어 보내지 않고 `{"event":"new_metric"}` 한 줄만 보낸다. Qt는 신호를 받으면 기존 SQL 조회(`MetricsRepository::FetchLatestMetrics`)를 그대로 다시 돈다. 이유: 지표 스키마를 "SQL"과 "푸시 JSON" 두 군데서 유지보수하지 않기 위해 — SQL이 여전히 유일한 데이터 원본.
- 전송 계층은 Agent 제어 채널(`AgentControlServer`)과 완전히 동일(Unix domain socket) — 다른 건 통신 패턴(요청-응답 vs 발행-구독)뿐. **ICMP 아님**(네트워크 계층 프로토콜과 무관, 애플리케이션 계층의 일반 텍스트 메시지).
- `QTimer`는 "주 트리거"에서 "안전망"으로 격하 — 푸시 연결이 끊겨도 완전히 멈추지 않도록 긴 주기(30초)로 유지.
- **이건 "Collector 코어 무수정" 원칙을 두 번째로 깨는 지점**이다(첫 번째는 Agent Phase A). Collector에 신규 파일 1개 + `main.cpp` 수정이 필요함. Console은 여전히 전혀 안 건드림. Qt가 상대하는 채널이 이제 3개(Agent 제어 IPC / Collector 지표 파일 읽기 / Collector 푸시 IPC)로 늘어남 — 의식하고 진행.

### 제안 — Collector/MetricsBroadcastServer.h (신규, `APM_Agent/Collector/`)

```cpp
#pragma once
#include "pch.h"

// 2026-09-14(§6 후속) : Collector가 새 지표를 저장할 때마다 로컬로 붙어있는 구독자(Qt)에게
// "뭔가 새로 생겼다"는 핑만 보낸다(발행-구독, 페이로드 없음 - 실제 값은 구독자가 기존
// SQL 조회로 알아서 다시 읽는다). Unix domain socket, 여러 구독자 동시 지원.
class MetricsBroadcastServer
{
public:
    explicit MetricsBroadcastServer(asio::io_context& ioContext, const String& socketPath);
    ~MetricsBroadcastServer();

    void Start();

    // 연결된 모든 구독자에게 핑 한 줄을 보낸다. 반드시 io_context 스레드에서 호출해야
    // 한다(Asio 소켓은 스레드 안전하지 않음) - 다른 스레드에서 부를 땐 asio::post로 넘길 것
    // (아래 Collector/main.cpp 변경 참고).
    void Notify();

private:
    void AcceptNext();

private:
    asio::io_context& _ioContext;
    String _socketPath;
    asio::local::stream_protocol::acceptor _acceptor;
    std::vector<std::shared_ptr<asio::local::stream_protocol::socket>> _subscribers;
    bool _running = false;
};
```

### 제안 — Collector/MetricsBroadcastServer.cpp (신규)

```cpp
#include "pch.h"
#include "MetricsBroadcastServer.h"

// TODO(Windows 지원 시): ::unlink는 POSIX 전용(Agent의 AgentControlServer.cpp와 같은 제약,
// Linux/WSL 우선 - Docs/QT_MFC_PORTFOLIO_PLAN.md §8).
#include <unistd.h>

MetricsBroadcastServer::MetricsBroadcastServer(asio::io_context& ioContext, const String& socketPath)
    : _ioContext(ioContext), _socketPath(socketPath), _acceptor(ioContext)
{
    ::unlink(_socketPath.c_str());

    asio::local::stream_protocol::endpoint endpoint(_socketPath);
    _acceptor.open(endpoint.protocol());
    _acceptor.bind(endpoint);
    _acceptor.listen();
}

MetricsBroadcastServer::~MetricsBroadcastServer()
{
    ::unlink(_socketPath.c_str());
}

void MetricsBroadcastServer::Start()
{
    _running = true;
    AcceptNext();
}

void MetricsBroadcastServer::AcceptNext()
{
    auto socket = std::make_shared<asio::local::stream_protocol::socket>(_ioContext);
    _acceptor.async_accept(*socket,
        [this, socket](const asio::error_code& ec)
        {
            if (!ec)
            {
                std::cout << "[MetricsBroadcastServer] subscriber connected (총 "
                    << (_subscribers.size() + 1) << "개)" << std::endl;
                _subscribers.push_back(socket);
            }
            if (_running)
                AcceptNext();
        });
}

void MetricsBroadcastServer::Notify()
{
    static const String kPingLine = "{\"event\":\"new_metric\"}\n";

    // 끊긴 구독자는 지워가면서 순회 - erase-remove 관용구.
    for (auto it = _subscribers.begin(); it != _subscribers.end(); )
    {
        auto socket = *it;
        asio::error_code ec;
        asio::write(*socket, asio::buffer(kPingLine), ec);

        if (ec)
            it = _subscribers.erase(it);
        else
            ++it;
    }
}
```

**변경 사유**: `Notify()`가 동기(블로킹) `asio::write`를 쓴다 — 로컬 소켓+수십 바이트짜리 페이로드라 OS 파이프 버퍼에 사실상 즉시 들어가고, 5초에 한 번(Agent 수집 주기)만 불리는 빈도라 비동기로 안 만든 의도적 단순화다. 구독자 수가 아주 많아지거나(이 프로젝트 범위 밖) 페이로드가 커지면(지금은 고정 문자열) 재검토 필요 — 지금 범위에선 과설계라고 판단.

### 제안 — Collector/main.cpp (수정)

**수정 전** (관련 부분 발췌 — 현재 디스크 상태):
```cpp
#include "Protocol/Metric.pb.h"
#include "Storage/MetricStoreFactory.h"
#include <thread>
```
```cpp
        IMetricStore* storePtr = store.get();
        WorkerQueue metricStoreQueue;
```
```cpp
        PacketHandler::Register<apm::Metric>(
            [storePtr, &metricStoreQueue, &pendingMetrics, &consoleLogQueue](const apm::Metric& pkt)
            {
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                // store->Store(pkt) 직접 호출(동기, fdatasync 블로킹 포함) 대신 워커 스레드로 위임.
                // pkt은 값 복사로 캡처 - 비동기 실행 시점까지 살아있어야 함.
                metricStoreQueue.Push([storePtr, pkt]() { storePtr->Store(pkt); });

                pendingMetrics.push_back(pkt);
```

**수정 후**:
```cpp
#include "Protocol/Metric.pb.h"
#include "Storage/MetricStoreFactory.h"
#include "MetricsBroadcastServer.h"
#include <thread>
```
```cpp
        IMetricStore* storePtr = store.get();
        WorkerQueue metricStoreQueue;

        // 2026-09-14(§6 후속) : 지표를 저장할 때마다 로컬 구독자(Qt)에게 핑을 쏜다.
        constexpr const char* METRICS_PUBSUB_SOCKET_PATH = "/tmp/apm_collector.sock";
        MetricsBroadcastServer broadcastServer(ioContext, METRICS_PUBSUB_SOCKET_PATH);
        broadcastServer.Start();
```
```cpp
        PacketHandler::Register<apm::Metric>(
            [storePtr, &metricStoreQueue, &pendingMetrics, &consoleLogQueue, &ioContext, &broadcastServer](const apm::Metric& pkt)
            {
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                // store->Store(pkt) 직접 호출(동기, fdatasync 블로킹 포함) 대신 워커 스레드로 위임.
                // pkt은 값 복사로 캡처 - 비동기 실행 시점까지 살아있어야 함.
                // 저장이 "실제로 끝난 뒤" 구독자에게 알려야 하므로, 알림도 같은 워커 람다 안에서
                // Store() 다음에 건다. 단, broadcastServer.Notify()는 Asio 소켓을 건드리므로
                // io_context 스레드에서만 호출 가능 - asio::post로 워커 스레드에서 io_context
                // 스레드로 다시 넘긴다(이 파일의 cliThread -> flushToWebServer와 같은 패턴).
                metricStoreQueue.Push([storePtr, pkt, &ioContext, &broadcastServer]()
                {
                    storePtr->Store(pkt);
                    asio::post(ioContext, [&broadcastServer]() { broadcastServer.Notify(); });
                });

                pendingMetrics.push_back(pkt);
```

**주의 — 선언 순서**: 현재 `Collector/main.cpp`는 `IMetricStore* storePtr = store.get(); WorkerQueue metricStoreQueue;`(54~55번째 줄)가 `asio::io_context ioContext;`(65번째 줄) **보다 먼저** 나온다. `MetricsBroadcastServer`의 생성자는 `ioContext` 참조를 받으므로, `broadcastServer` 선언은 반드시 **`ioContext` 선언(65번째 줄) 다음**에 와야 한다 — `storePtr`/`metricStoreQueue` 블록 바로 다음이 아니라, `ioContext` 선언 직후에 넣을 것. (위 "수정 전/후" 발췌는 논리적 인접성 때문에 `storePtr`/`metricStoreQueue` 옆에 나란히 적었지만, 실제 삽입 지점은 `ioContext` 선언 이후다.)

**변경 사유**: 새 스레드/타이머를 안 만들고, 이미 지표 하나를 처리하는 그 자리에 알림을 얹었다 — `consoleLogQueue`에 로그를 위임하는 것과 같은 이유(네트워크 스레드가 직접 블로킹 작업을 하지 않게). `asio::post`로 스레드를 다시 넘기는 이유는 Asio 소켓이 스레드 안전하지 않아서 — 이 파일에 이미 있는 "cliThread(별도 스레드) → `asio::post(ioContext, flushToWebServer)`" 패턴을 그대로 재사용한 것이지 새 관용구를 만든 게 아니다.

### 제안 — CMakeLists.txt 변경 (`APM_Agent/CMakeLists.txt`)

**수정 전** (`Collector` 타겟 부분):
```cmake
add_executable(Collector
    Collector/main.cpp
    Collector/CollectorConfig.cpp
    pch.cpp
)
target_include_directories(Collector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(Collector PRIVATE APM_Common APM_Storage)
```

**수정 후**:
```cmake
add_executable(Collector
    Collector/main.cpp
    Collector/CollectorConfig.cpp
    Collector/MetricsBroadcastServer.cpp
    pch.cpp
)
target_include_directories(Collector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(Collector PRIVATE APM_Common APM_Storage)
```

**변경 사유**: 신규 파일 추가뿐 — 새 외부 의존성 없음(Asio는 이미 `APM_Common` 경유로 링크돼 있음).

---

### 제안 — Qt: MetricsPushClient.h (신규, `APM_QtDashboard/`)

```cpp
#pragma once
#include <QObject>
#include <QString>

class QLocalSocket;
class QTimer;

// 2026-09-14(§6 후속) : Collector의 MetricsBroadcastServer에 구독자로 붙는다. 페이로드는
// 안 보고 "뭔가 왔다"는 사실만으로 NewMetricAvailable()을 낸다 - 실제 값은 기존 SQL
// 경로(MetricsWorker::Refresh)로 다시 읽는다. 연결이 끊기면 재시도한다(Agent의
// ResilientSender와 같은 발상이지만 훨씬 단순 - 큐잉/재전송이 필요 없는 단방향 알림이라).
class MetricsPushClient : public QObject
{
    Q_OBJECT
public:
    explicit MetricsPushClient(const QString& socketPath, QObject* parent = nullptr);

public slots:
    void Start();

signals:
    void NewMetricAvailable();

private slots:
    void OnReadyRead();
    void OnDisconnected();
    void TryConnect();

private:
    QString _socketPath;
    QLocalSocket* _socket = nullptr;
    QTimer* _reconnectTimer = nullptr;
};
```

### 제안 — Qt: MetricsPushClient.cpp (신규)

```cpp
#include "MetricsPushClient.h"

#include <QLocalSocket>
#include <QTimer>

namespace
{
constexpr int kReconnectIntervalMs = 3000;
}

MetricsPushClient::MetricsPushClient(const QString& socketPath, QObject* parent)
    : QObject(parent), _socketPath(socketPath)
{
    _socket = new QLocalSocket(this);
    connect(_socket, &QLocalSocket::readyRead, this, &MetricsPushClient::OnReadyRead);
    connect(_socket, &QLocalSocket::disconnected, this, &MetricsPushClient::OnDisconnected);

    _reconnectTimer = new QTimer(this);
    _reconnectTimer->setInterval(kReconnectIntervalMs);
    connect(_reconnectTimer, &QTimer::timeout, this, &MetricsPushClient::TryConnect);
}

void MetricsPushClient::Start()
{
    TryConnect();
    _reconnectTimer->start();
}

void MetricsPushClient::TryConnect()
{
    if (_socket->state() == QLocalSocket::ConnectedState)
        return;

    _socket->connectToServer(_socketPath);
}

void MetricsPushClient::OnReadyRead()
{
    // 줄 단위로 읽되 내용은 실제로 안 본다 - "뭔가 왔다"는 사실 자체가 신호다.
    while (_socket->canReadLine())
    {
        _socket->readLine();
        emit NewMetricAvailable();
    }
}

void MetricsPushClient::OnDisconnected()
{
    // 별도 처리 없음 - _reconnectTimer가 주기적으로 TryConnect()를 계속 시도한다.
}
```

**변경 사유**: `QLocalSocket::connectToServer(fullPath)`는 경로에 `/`가 포함되면 Qt가 그 경로를 그대로 OS의 Unix domain socket 경로로 써서 `connect()`한다 — Collector의 Asio 소켓(같은 OS 레벨 메커니즘)에 라이브러리 무관하게 붙을 수 있다(이전 세션에서 Python `socket.AF_UNIX`로 Asio 소켓에 직접 접속해본 것과 같은 원리). `_reconnectTimer`(3초 간격)는 `ResilientSender`의 재연결 타이머(5초)보다 짧게 잡았다 — 이쪽은 큐잉된 데이터를 잃을 걱정이 없는 단방향 신호라 더 공격적으로 재시도해도 부담이 적음.

### 제안 — MetricsChartWidget.h / .cpp (수정) — 중복 점 방지

**수정 전** (`.h` 멤버 목록 부분):
```cpp
private:
    QChartView* _chartView;
    QLineSeries* _cpuSeries;
    QLineSeries* _memSeries;
    QValueAxis* _axisX;
    QValueAxis* _axisY;
    qint64 _nextX = 0;
};
```

**수정 후**:
```cpp
private:
    QChartView* _chartView;
    QLineSeries* _cpuSeries;
    QLineSeries* _memSeries;
    QValueAxis* _axisX;
    QValueAxis* _axisY;
    qint64 _nextX = 0;
    qlonglong _lastSeenId = -1;   // 2026-09-14 : 중복 점 방지(아래 .cpp 참고)
};
```

**수정 전** (`.cpp`의 `AppendLatest` 함수 전문):
```cpp
void MetricsChartWidget::AppendLatest(const QVector<MetricsSample>& samples)
{
    if (samples.isEmpty())
        return;

    // samples[0]이 최신값(FetchLatestMetrics가 "ORDER BY rowid DESC"로 가져오므로).
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

**수정 후**:
```cpp
void MetricsChartWidget::AppendLatest(const QVector<MetricsSample>& samples)
{
    if (samples.isEmpty())
        return;

    // samples[0]이 최신값(FetchLatestMetrics가 "ORDER BY rowid DESC"로 가져오므로).
    const MetricsSample& latest = samples.first();

    // 2026-09-14 : 이제 갱신이 Collector 푸시로 트리거되긴 하지만, 수동 "새로고침" 버튼이
    // 여전히 있어서 "갱신 이벤트 발생"과 "실제 새 데이터"가 100% 같다는 보장은 없다(사용자가
    // 짚은 지점). 같은 행(id)을 또 받으면 조용히 무시 - 의도("실제 새 샘플 1개당 점 1개")를
    // 갱신 트리거 방식과 무관하게 항상 지킨다.
    if (latest.id == _lastSeenId)
        return;
    _lastSeenId = latest.id;

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

**변경 사유**: 푸시로 바꿔도 수동 버튼이 여전히 같은 트리거 경로(`RefreshRequested`)를 타므로 "이벤트 발생 = 새 데이터"가 100% 보장되진 않는다 — 방어 코드를 남겨두는 게 맞다고 판단(지난 대화에서 사용자에게 설명한 그대로).

### 제안 — MainWindow.h / .cpp (수정)

**수정 전** (`.h` 전방 선언 + 멤버 부분, 2026-09-14 6′ 제안 기준):
```cpp
class QLabel;
class QPushButton;
class QTableView;
class QTimer;
class MetricsWorker;
class MetricsTableModel;
class AlertsTableModel;
class MetricsChartWidget;
```
```cpp
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
    QLabel* _agentStatusLabel = nullptr;
};
```

**수정 후**:
```cpp
class QLabel;
class QPushButton;
class QTableView;
class QTimer;
class MetricsWorker;
class MetricsTableModel;
class AlertsTableModel;
class MetricsChartWidget;
class MetricsPushClient;
```
```cpp
private:
    QThread _workerThread;
    MetricsWorker* _worker = nullptr;
    QTimer* _refreshTimer = nullptr;           // 이제 "안전망"(주 트리거는 _pushClient)
    MetricsPushClient* _pushClient = nullptr;  // 2026-09-14 : Collector 푸시 구독, 주 트리거

    MetricsTableModel* _metricsModel = nullptr;
    AlertsTableModel* _alertsModel = nullptr;
    QTableView* _metricsView = nullptr;
    QTableView* _alertsView = nullptr;
    MetricsChartWidget* _chartWidget = nullptr;
    QPushButton* _refreshButton = nullptr;
    QLabel* _statusLabel = nullptr;
    QLabel* _agentStatusLabel = nullptr;
};
```

**수정 사유**: `MetricsPushClient` 전방 선언 + 멤버 추가. `_refreshTimer`는 지우지 않고 역할만 "주 트리거→안전망"으로 바뀐다(생성자에서 interval을 늘림, 아래).

**수정 전** (`.cpp`, 생성자 안 include와 워커/타이머 배선 부분, 6′ 제안 기준):
```cpp
#include "AlertsTableModel.h"
#include "MetricsChartWidget.h"
#include "MetricsTableModel.h"
#include "MetricsWorker.h"

namespace
{
const QString kCollectorDbPath = "/home/shkim/dev/APM/APM_Agent/apm_metrics.db";
const QString kAgentAlertsDbPath = "/home/shkim/dev/APM/APM_Agent/agent_alerts.db";
constexpr int kRefreshIntervalMs = 5000;
constexpr qint64 kAgentStaleSeconds = 15;
}
```
```cpp
    _refreshTimer = new QTimer(this);
    _refreshTimer->setInterval(kRefreshIntervalMs);
    connect(_refreshTimer, &QTimer::timeout, this, &MainWindow::OnRefreshClicked);

    _workerThread.start();
```

**수정 후**:
```cpp
#include "AlertsTableModel.h"
#include "MetricsChartWidget.h"
#include "MetricsPushClient.h"
#include "MetricsTableModel.h"
#include "MetricsWorker.h"

namespace
{
const QString kCollectorDbPath = "/home/shkim/dev/APM/APM_Agent/apm_metrics.db";
const QString kAgentAlertsDbPath = "/home/shkim/dev/APM/APM_Agent/agent_alerts.db";

// 2026-09-14 : Collector의 MetricsBroadcastServer 소켓 경로(Collector/main.cpp의
// METRICS_PUBSUB_SOCKET_PATH와 반드시 같아야 함).
const QString kCollectorPushSocketPath = "/tmp/apm_collector.sock";

// 2026-09-14 : 이제 "주 트리거"가 아니라 "안전망" - 푸시 연결이 끊겨 있어도 이 주기마다는
// 갱신되게 한다. 너무 짧으면 안전망의 존재 의미가 없고(푸시랑 다를 바 없어짐), 너무 길면
// 푸시가 끊긴 동안 화면이 오래 정체된다 - 30초(수집 주기의 6배)로 절충.
constexpr int kFallbackRefreshIntervalMs = 30000;
constexpr qint64 kAgentStaleSeconds = 15;
}
```
```cpp
    _refreshTimer = new QTimer(this);
    _refreshTimer->setInterval(kFallbackRefreshIntervalMs);
    connect(_refreshTimer, &QTimer::timeout, this, &MainWindow::OnRefreshClicked);
    _refreshTimer->start();

    // 2026-09-14 : Collector 푸시 구독 - 새 지표 알림을 받으면 기존 트리거(OnRefreshClicked)를
    // 그대로 재사용한다(새 갱신 로직을 안 만듦, 트리거 경로만 하나 더 생기는 것).
    _pushClient = new MetricsPushClient(kCollectorPushSocketPath, this);
    connect(_pushClient, &MetricsPushClient::NewMetricAvailable, this, &MainWindow::OnRefreshClicked);
    _pushClient->Start();

    _workerThread.start();
```

**변경 사유**: `_refreshTimer`는 이제 `OnWorkerInitialized`가 아니라 **생성자에서 바로 `start()`** — 안전망이라는 성격상 Worker 초기화 성공 여부와 무관하게 항상 돌아야 한다(초기화가 실패해도 30초마다 재시도하는 셈이 되어 오히려 자연스러운 복구 경로가 됨). `_pushClient`도 마찬가지로 생성자에서 바로 `Start()` — 연결 자체가 실패해도 내부 재연결 타이머가 알아서 계속 시도하므로 특별한 실패 처리가 필요 없다. `OnWorkerInitialized`의 `emit RefreshRequested()`(최초 1회 로드)는 그대로 유지(아래는 변경 없음, 참고로만 표시):
```cpp
void MainWindow::OnWorkerInitialized(bool metricsOk)
{
    if (!metricsOk)
    {
        SetStatus("DB 열기 실패 - stderr 확인");
        return;
    }
    SetStatus("연결됨");
    emit RefreshRequested();
    // _refreshTimer->start() 호출은 위로 이동(생성자에서 즉시 시작) - 이 함수에서는 제거.
}
```

### 제안 — CMakeLists.txt 변경 (`APM_QtDashboard/CMakeLists.txt`)

**수정 전**:
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
    AgentAlertRepository.cpp
    AgentAlertRepository.h
)

target_link_libraries(APM_QtDashboard PRIVATE Qt6::Widgets Qt6::Sql Qt6::Charts)
```

**수정 후**:
```cmake
find_package(Qt6 REQUIRED COMPONENTS Widgets Sql Charts Network)

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
    AgentAlertRepository.cpp
    AgentAlertRepository.h
    MetricsPushClient.cpp
    MetricsPushClient.h
)

target_link_libraries(APM_QtDashboard PRIVATE Qt6::Widgets Qt6::Sql Qt6::Charts Qt6::Network)
```

**변경 사유**: 신규 파일 2개 추가. `QLocalSocket`은 `Qt6::Widgets`만으로는 못 쓴다 — 직접 최소 재현 프로젝트로 확인함(`#include <QLocalSocket>`만 있는 파일을 `Widgets`만 링크해서 빌드 → `fatal error: QLocalSocket: No such file or directory`, `Network` 컴포넌트 추가 후 정상 빌드). `Qt6::Network`를 `find_package`/`target_link_libraries` 양쪽에 추가해야 한다 — 2단계 때 `Sql` 컴포넌트를 빠뜨렸던 것과 같은 종류의 실수를 이번엔 제안 단계에서 미리 검증해서 방지.

### 검증 (미검증 — 사용자가 직접 작성/빌드 후 확인)

1. 클린 빌드 성공(Collector에 `MetricsBroadcastServer` 추가, Qt에 `MetricsPushClient` 추가 — `Qt6::Network` 링크는 이미 별도로 검증 완료).
2. Collector+Agent+Qt를 순서대로 띄우고, Qt 로그/상태에서 푸시 소켓 연결이 되는지(가장 쉬운 확인: Collector 콘솔에 "subscriber connected" 로그가 뜨는지).
3. 5초마다 Agent가 지표를 보낼 때, 수동 새로고침 버튼을 **누르지 않아도** 차트/테이블이 자동으로 갱신되는지(폴링 없이 푸시만으로 동작 확인).
4. 새로고침 버튼을 푸시 갱신 직후 연타 — 차트에 중복 점이 **안** 찍히는지(`_lastSeenId` 방어 확인).
5. Collector를 죽였다 다시 띄우기 — Qt의 `MetricsPushClient`가 재연결 타이머(3초)로 자동 재구독하는지, Collector 콘솔에 "subscriber connected"가 다시 뜨는지.
6. 푸시 소켓 자체를 막아둔 상태(예: Collector가 구버전이라 `MetricsBroadcastServer`가 없는 경우를 흉내) — `_refreshTimer`(30초 안전망)만으로도 결국 갱신되는지.

### 결정 사항

문서 제안만 — `APM_Agent/`/`APM_QtDashboard/`의 실제 소스는 사용자가 직접 작성(원칙 2, [[feedback_claude_md_rule2_scope]]). 작성 중 나오는 오타/컴파일 에러는 요청 시 수정 지원. 완료되면 `WORK_STATUS.md`/`Docs/QT_MFC_PORTFOLIO_PLAN.md` 갱신 후 커밋+push.
