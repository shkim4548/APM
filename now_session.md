## 2026-09-17 — 6′ 설계 재검토(발행-구독 반영 후) — 확정 및 전체 코드 제안

### 배경

2026-09-14에 작성해둔 6′(알림+연결상태) 설계는 그 시점의 `MainWindow`(폴링 기반, `_refreshTimer`가 주 트리거) 위에서 짜여 있었는데, 그 사이 발행-구독 재설계(`MetricsPushClient`가 주 트리거, `_refreshTimer`는 30초 안전망)가 먼저 적용되면서 그 "수정 전" 기준 자체가 낡았다. 사용자 요청으로 현재 실제 코드(`4f93d3b` 커밋) 기준으로 다시 맞춤.

**이번에 새로 결정한 것 — 알림/상태 갱신을 어느 트리거에 얹을지**:

Agent는 Collector에 보내기 **전에** 이미 로컬로 알림을 판단하므로, Collector가 저장→푸시하는 시점엔 Agent의 알림/상태가 이미 최신인 경우가 대부분이다. 그래서 **알림/상태 전용 푸시 채널을 새로 만들지 않고, 기존 `MetricsReady` 트리거(푸시 우선 + 30초 안전망)에 그대로 얹는다** — `MetricsWorker`가 지표 조회와 같은 `Refresh()` 호출 안에서 알림/상태도 같이 조회.

**감수하는 트레이드오프(사용자 확인 완료)**: Agent↔Collector 연결이 끊긴 동안은 Collector가 푸시를 못 하므로, 그 사이엔 알림/상태 화면도 30초 안전망 주기로만 갱신된다 — Phase A가 지향한 "즉시성"이 알림에 한해 "최대 30초 지연"으로 완화됨. 지금 규모에선 감수 가능하다고 판단(새 소켓을 하나 더 늘리는 비용이 더 크다는 근거).

핵심 설계(스키마/`AgentAlertRepository`/상태 판정 기준)는 2026-09-14 원안과 동일 — 아래는 현재 실제 코드(`4f93d3b`) 기준으로 다시 맞춘 전체 diff다.

### 제안 — AgentAlertRepository.h (신규, `APM_QtDashboard/`) — 2026-09-14 원안과 동일

```cpp
#pragma once
#include <QMetaType>
#include <QString>
#include <QVector>
#include <qglobal.h>

#include "MetricsRepository.h"   // AlertSample 재사용

// step 6' : Agent의 agent_alerts.db(local_alerts + agent_status)를 읽는다.
// MetricsRepository(Collector의 apm_metrics.db)와는 완전히 다른 파일/커넥션 -
// 두 리포지토리를 하나로 합치지 않는다(프로세스 경계가 다르므로 파일도 다름).
struct AgentStatus
{
    bool connected = false;
    qlonglong updatedAt = 0;   // epoch seconds
    bool valid = false;        // agent_status에 행이 아직 없으면 false(Agent가 한 번도 안 씀)
};

Q_DECLARE_METATYPE(AgentStatus)

class AgentAlertRepository
{
public:
    explicit AgentAlertRepository(const QString& dbPath);
    ~AgentAlertRepository();

    bool Open();
    bool IsOpen() const;

    QVector<AlertSample> FetchOpenAlerts(int limit) const;
    AgentStatus FetchStatus() const;

private:
    QString _dbPath;
    QString _connectionName;
};
```

### 제안 — AgentAlertRepository.cpp (신규) — 2026-09-14 원안과 동일

```cpp
#include "AgentAlertRepository.h"

#include <QDateTime>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>

namespace
{
    constexpr const char* kConnectionName = "apm_agent_alerts_ro";
}

AgentAlertRepository::AgentAlertRepository(const QString& dbPath)
    : _dbPath(dbPath), _connectionName(kConnectionName)
{
}

AgentAlertRepository::~AgentAlertRepository()
{
    if (QSqlDatabase::contains(_connectionName))
    {
        QSqlDatabase::removeDatabase(_connectionName);
    }
}

bool AgentAlertRepository::Open()
{
    auto db = QSqlDatabase::addDatabase("QSQLITE", _connectionName);
    db.setDatabaseName(_dbPath);
    db.setConnectOptions("QSQLITE_OPEN_READONLY");

    if (!db.open())
    {
        qWarning() << "AgentAlertRepository : failed to Open" << _dbPath << db.lastError().text();
        return false;
    }

    return true;
}

bool AgentAlertRepository::IsOpen() const
{
    return QSqlDatabase::database(_connectionName, false).isOpen();
}

QVector<AlertSample> AgentAlertRepository::FetchOpenAlerts(int limit) const
{
    QVector<AlertSample> result;

    QSqlQuery query(QSqlDatabase::database(_connectionName));
    query.prepare(
        "SELECT id, metric_type, threshold_value, trigger_value, opened_at "
        "FROM local_alerts WHERE closed_at IS NULL ORDER BY id DESC LIMIT ?");
    query.addBindValue(limit);

    if (!query.exec())
    {
        qWarning() << "AgentAlertRepository::FetchOpenAlerts failed : " << query.lastError().text();
        return result;
    }

    while (query.next())
    {
        AlertSample sample;
        sample.id = query.value(0).toLongLong();
        sample.metricType = query.value(1).toInt();
        sample.thresholdValue = query.value(2).toDouble();
        sample.triggerValue = query.value(3).toDouble();
        qint64 epochSeconds = query.value(4).toLongLong();
        sample.openedAt = QDateTime::fromSecsSinceEpoch(epochSeconds).toString("yyyy-MM-dd HH:mm:ss");
        result.push_back(sample);
    }
    return result;
}

AgentStatus AgentAlertRepository::FetchStatus() const
{
    AgentStatus status;

    QSqlQuery query(QSqlDatabase::database(_connectionName));
    query.prepare("SELECT connected, updated_at FROM agent_status WHERE id = 1");

    if (!query.exec())
    {
        qWarning() << "AgentAlertRepository::FetchStatus failed : " << query.lastError().text();
        return status;
    }

    if (query.next())
    {
        status.connected = query.value(0).toInt() != 0;
        status.updatedAt = query.value(1).toLongLong();
        status.valid = true;
    }
    // 행이 없으면(Agent가 아직 한 번도 UpdateStatus를 안 부름) valid=false인 채로 반환.

    return status;
}
```

### 제안 — MetricsWorker.h (수정) — 수정 전은 현재 디스크(`4f93d3b`) 그대로

**수정 전** (파일 전문):
```cpp
#pragma once
#include <QObject>
#include <QString>
#include <QVector>

#include "MetricsRepository.h"

// Sqlite3 조회를 UI 스레드에서 분리한다
class MetricsWorker : public QObject
{
    Q_OBJECT
public :
    explicit MetricsWorker(const QString& dbPath, QObject* parent = nullptr);

public slots:
    // move to thread 이후 워커쓰레드에서 실행한다.
    void Initialize();
    // 최신 지표를 조회해서 결과를 시그널로 남긴다.
    // 2026-09-14 : 알림 조회(AlertsReady)는 뺐다 - Collector의 apm_metrics.db엔 알림
    // 테이블이 없음(알림 판단은 이제 Agent가 로컬로 함, agent_alerts.db). §6 단계에서
    // 그 DB를 보는 별도 워커/리포지토리로 다시 연결할 예정.
    void Refresh();

signals:
    void Initialized(bool ok);
    void MetricsReady(const QVector<MetricsSample>& samples);
    void RefreshFailed(const QString& reason);

private:
    MetricsRepository _repository;
    bool _ready = false;  
};
```

**수정 후**:
```cpp
#pragma once
#include <QObject>
#include <QString>
#include <QVector>

#include "AgentAlertRepository.h"
#include "MetricsRepository.h"

// Sqlite3 조회를 UI 스레드에서 분리한다
class MetricsWorker : public QObject
{
    Q_OBJECT
public :
    explicit MetricsWorker(const QString& metricsDbPath, const QString& alertsDbPath, QObject* parent = nullptr);

public slots:
    // move to thread 이후 워커쓰레드에서 실행한다.
    void Initialize();
    // 최신 지표 + 열린 알림 + Agent 상태를 조회해서 결과를 시그널로 남긴다.
    // 2026-09-17(§6') : 알림/상태 전용 푸시 채널을 새로 안 만들고, 기존 지표 갱신
    // 트리거(Collector 푸시 우선 + 30초 안전망)에 그대로 얹는다 - Agent가 Collector에
    // 보내기 전에 이미 로컬로 알림을 판단해두므로 대부분 시점상 최신이고, 소켓을 더
    // 늘리지 않는 쪽을 택함(단, Agent<->Collector 연결이 끊긴 동안은 알림/상태도 지표와
    // 같이 30초 안전망 주기로만 갱신됨 - 감수한 트레이드오프).
    void Refresh();

signals:
    void Initialized(bool metricsOk);
    void MetricsReady(const QVector<MetricsSample>& samples);
    void AlertsReady(const QVector<AlertSample>& alerts);
    void StatusReady(const AgentStatus& status);
    void RefreshFailed(const QString& reason);

private:
    MetricsRepository _repository;
    AgentAlertRepository _alertRepository;
    bool _ready = false;
};
```

**변경 사유**: 생성자가 경로 2개를 받도록 바뀜(호출부 `MainWindow`도 같이 바뀜, 아래). `Initialized(bool metricsOk)`로 파라미터 이름을 명확히 함(지표 리포지토리 open 성공 여부만 의미 - 알림 DB open 실패는 별개로 비치명적 처리, `.cpp` 참고).

### 제안 — MetricsWorker.cpp (수정, 전문)

**수정 전** (전문):
```cpp
#include "MetricsWorker.h"

namespace
{
    constexpr int kFetchLimit = 20;
}

MetricsWorker::MetricsWorker(const QString& dbPath, QObject* parent)
    : QObject(parent), _repository(dbPath)
{
}

void MetricsWorker::Initialize()
{
    // QSqlDatabase 연결은 만든 쓰레드에서만 쓸 수 있으므로 Open()을 이 슬롯에서 호출한다
    _ready = _repository.Open();
    emit Initialized(_ready);
}

void MetricsWorker::Refresh()
{
    if(!_ready)
    {
        emit RefreshFailed("Repository not open");
        return;
    }

    emit MetricsReady(_repository.FetchLatestMetrics(kFetchLimit));
}
```

**수정 후**:
```cpp
#include "MetricsWorker.h"

#include <QDebug>

namespace
{
    constexpr int kFetchLimit = 20;
}

MetricsWorker::MetricsWorker(const QString& metricsDbPath, const QString& alertsDbPath, QObject* parent)
    : QObject(parent), _repository(metricsDbPath), _alertRepository(alertsDbPath)
{
}

void MetricsWorker::Initialize()
{
    // QSqlDatabase 연결은 만든 쓰레드에서만 쓸 수 있으므로 Open()을 이 슬롯에서 호출한다
    _ready = _repository.Open();

    // 알림 DB는 지표 DB와 별개 파일이라 실패해도 지표 표시 자체는 계속 동작해야 한다
    // (Agent가 아직 안 떠 있어도 Collector 지표는 볼 수 있어야 함) - 그래서 실패해도
    // Initialized(false)로 전체를 막지 않고 경고만 남긴다.
    if (!_alertRepository.Open())
        qWarning() << "MetricsWorker: alert repository open failed (지표는 계속 조회됨)";

    emit Initialized(_ready);
}

void MetricsWorker::Refresh()
{
    if(!_ready)
    {
        emit RefreshFailed("Repository not open");
        return;
    }

    emit MetricsReady(_repository.FetchLatestMetrics(kFetchLimit));

    if (_alertRepository.IsOpen())
    {
        emit AlertsReady(_alertRepository.FetchOpenAlerts(kFetchLimit));
        emit StatusReady(_alertRepository.FetchStatus());
    }
}
```

**변경 사유**: `Refresh()`가 트리거 종류(수동 버튼/푸시/안전망 타이머 중 무엇이 불렀는지)를 구분하지 않고 항상 지표+알림+상태를 한 번에 같이 조회 — "배경"에서 정한 대로 알림/상태 전용 트리거를 안 만들었기 때문에 이게 자연스럽다. `_alertRepository.IsOpen()`으로 지키는 이유는 Agent가 아직 안 떠서 `agent_alerts.db` 자체가 없는 상태에서도 지표 조회(`FetchLatestMetrics`)는 계속 정상 동작해야 하기 때문.

### 제안 — MainWindow.h (수정) — 수정 전은 현재 디스크(`4f93d3b`) 그대로

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
class QTimer;
class MetricsWorker;
class MetricsTableModel;
class MetricsChartWidget;
class MetricsPushClient;

// step 2~5 (2026-09-14 재작업) : 대상 DB를 APM_Console(중앙)에서 Collector(그 장비
// 로컬)로 교체 + QTimer 자동 갱신 + 실시간 차트(MetricsChartWidget) 추가.
// 알림/Agent 제어(§6~7)는 별도로 검토 중이라 이번엔 손대지 않음.
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

private:
    void SetStatus(const QString& text);

private:
    QThread _workerThread;
    MetricsWorker* _worker = nullptr;
    QTimer* _refreshTimer = nullptr;           // 이제 "안전망"(주 트리거는 _pushClient)
    MetricsPushClient* _pushClient = nullptr;  // 2026-09-14 : Collector 푸시 구독, 주 트리거

    MetricsTableModel* _metricsModel = nullptr;
    QTableView* _metricsView = nullptr;
    MetricsChartWidget* _chartWidget = nullptr;
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

#include "AgentAlertRepository.h"
#include "MetricsRepository.h"

class QLabel;
class QPushButton;
class QTableView;
class QTimer;
class MetricsWorker;
class MetricsTableModel;
class AlertsTableModel;
class MetricsChartWidget;
class MetricsPushClient;

// step 2~6' (2026-09-17 재작업) : Collector 로컬 DB(지표) + Agent 로컬 DB(알림/상태)
// 둘 다 읽는다. 알림/상태도 기존 지표 갱신 트리거(푸시+안전망)를 그대로 재사용 -
// 별도 푸시 채널 없음(WORK_STATUS.md/SESSION_LOG.md 2026-09-17 참고). Agent 제어(§7)는
// 아직 손대지 않음.
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
    void OnWorkerInitialized(bool metricsOk);
    void OnRefreshFailed(const QString& reason);
    void PopulateMetricsTable(const QVector<MetricsSample>& samples);
    void PopulateAlertTable(const QVector<AlertSample>& alerts);
    void UpdateAgentStatus(const AgentStatus& status);

private:
    void SetStatus(const QString& text);

private:
    QThread _workerThread;
    MetricsWorker* _worker = nullptr;
    QTimer* _refreshTimer = nullptr;           // 안전망(주 트리거는 _pushClient)
    MetricsPushClient* _pushClient = nullptr;  // Collector 푸시 구독, 주 트리거

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

**변경 사유**: `AlertsTableModel`/`MetricsPushClient`(이미 있었음, 그대로 유지) 전방 선언 + `_alertsModel`/`_alertsView`/`_agentStatusLabel` 추가. `AgentAlertRepository.h`를 include하는 이유는 `AgentStatus`를 시그널/슬롯 시그니처에 직접 쓰기 때문(전방 선언으로는 부족 - `MetricsRepository` include와 같은 이유).

### 제안 — MainWindow.cpp (수정, 전문)

**수정 전**: 현재 디스크(`4f93d3b`) 상태 그대로(이전 답변에서 이미 전문을 보여드림 — 생략, 아래 "수정 후"와 대조하면 바뀐 지점이 뚜렷함).

**수정 후** (전문):
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
#include "MetricsPushClient.h"
#include "MetricsTableModel.h"
#include "MetricsWorker.h"

namespace
{
// 2026-09-14 : APM_Console(중앙) 대신 Collector(그 장비 로컬)가 만드는 apm_metrics.db를
// 본다 - Qt는 이제 Console을 전혀 모른다(WORK_STATUS.md/QT_MFC_PORTFOLIO_PLAN.md §3-2
// 아키텍처 대전환 참고). Collector가 상대경로("apm_metrics.db")로 파일을 만들기 때문에
// 실제 위치는 Collector를 어느 디렉터리에서 실행했는지에 달려있다 - 이 체크아웃에서는
// APM_Agent/ 안에서 실행하는 관례(APM_Viewer의 기존 하드코딩과 동일)를 그대로 따름.
const QString kCollectorDbPath = "/home/shkim/dev/APM/APM_Agent/apm_metrics.db";

// step 6' : Agent가 만드는 agent_alerts.db(알림 + 연결 상태). Agent도 같은 디렉터리
// 관례로 실행한다고 가정.
const QString kAgentAlertsDbPath = "/home/shkim/dev/APM/APM_Agent/agent_alerts.db";

// 2026-09-14 : Collector의 MetricsBroadcastServer 소켓 경로(Collector/main.cpp의
// METRICS_PUBSUB_SOCKET_PATH와 반드시 같아야 함).
const QString kCollectorPushSocketPath = "/tmp/apm_collector.sock";

// 2026-09-14 : "주 트리거"가 아니라 "안전망" - 푸시 연결이 끊겨 있어도(또는 6' 트레이드오프대로
// Agent<->Collector가 끊겨 Collector가 아예 푸시를 못 하는 동안도) 이 주기마다는 갱신되게 한다.
constexpr int kFallbackRefreshIntervalMs = 30000;

// step 6' : agent_status.updated_at이 이보다 오래되면 "Agent 응답 없음"으로 본다.
// Agent 수집 주기(5초)의 3배 - 한두 번 갱신을 놓쳐도 바로 "죽음"으로 오판정하지 않기 위한 여유.
constexpr qint64 kAgentStaleSeconds = 15;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("APM Qt Dashboard - step 2~6' (Collector+Agent 로컬 DB, 발행-구독 갱신)");

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    _refreshButton = new QPushButton("새로고침", central);
    _statusLabel = new QLabel("초기화 중...", central);
    _agentStatusLabel = new QLabel("Agent 상태: 확인 중...", central);

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
    layout->addWidget(_agentStatusLabel);
    layout->addWidget(_chartWidget);
    layout->addWidget(_metricsView);
    layout->addWidget(_alertsView);
    setCentralWidget(central);

    connect(_refreshButton, &QPushButton::clicked, this, &MainWindow::OnRefreshClicked);

    // 워커를 만들고 워커 스레드로 옮긴다. 이 시점 이후 워커의 슬롯은 워커 스레드에서 실행된다.
    _worker = new MetricsWorker(kCollectorDbPath, kAgentAlertsDbPath);
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
    connect(_worker, &MetricsWorker::StatusReady, this, &MainWindow::UpdateAgentStatus);
    connect(_worker, &MetricsWorker::RefreshFailed, this, &MainWindow::OnRefreshFailed);

    _refreshTimer = new QTimer(this);
    _refreshTimer->setInterval(kFallbackRefreshIntervalMs);
    connect(_refreshTimer, &QTimer::timeout, this, &MainWindow::OnRefreshClicked);
    _refreshTimer->start();

    // Collector 푸시 구독 - 새 지표 알림을 받으면 기존 트리거(OnRefreshClicked)를 그대로
    // 재사용한다(알림/상태도 이 트리거 한 번에 같이 조회됨 - MetricsWorker::Refresh() 참고).
    _pushClient = new MetricsPushClient(kCollectorPushSocketPath, this);
    connect(_pushClient, &MetricsPushClient::NewMetricAvailable, this, &MainWindow::OnRefreshClicked);
    _pushClient->Start();

    _workerThread.start();

    // 워커가 워커 스레드로 옮겨진 뒤 Initialize()가 그 스레드에서 실행되도록 큐에 넣는다.
    QMetaObject::invokeMethod(_worker, "Initialize", Qt::QueuedConnection);
}

MainWindow::~MainWindow()
{
    // 워커 스레드의 이벤트 루프를 멈추고, 실제로 끝날 때까지 기다린다.
    // 이걸 빼면 프로세스 종료 시 "QThread: Destroyed while thread is still running" 경고/크래시.
    _workerThread.quit();
    _workerThread.wait();
}

void MainWindow::OnRefreshClicked()
{
    SetStatus("불러오는 중...");
    emit RefreshRequested();
}

void MainWindow::OnWorkerInitialized(bool metricsOk)
{
    if (!metricsOk)
    {
        SetStatus("DB 열기 실패 - stderr 확인");
        return;
    }
    SetStatus("연결됨");
    emit RefreshRequested();
}

void MainWindow::OnRefreshFailed(const QString& reason)
{
    SetStatus("조회 실패: " + reason);
}

void MainWindow::SetStatus(const QString& text)
{
    _statusLabel->setText(text);
}

void MainWindow::PopulateMetricsTable(const QVector<MetricsSample>& samples)
{
    _metricsModel->SetSamples(samples);
    SetStatus(QString("갱신 완료 %1").arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
}

void MainWindow::PopulateAlertTable(const QVector<AlertSample>& alerts)
{
    _alertsModel->SetAlerts(alerts);
}

void MainWindow::UpdateAgentStatus(const AgentStatus& status)
{
    if (!status.valid)
    {
        _agentStatusLabel->setText("Agent 상태: 알 수 없음(agent_alerts.db에 상태 없음)");
        return;
    }

    qint64 ageSeconds = QDateTime::currentSecsSinceEpoch() - status.updatedAt;

    if (ageSeconds > kAgentStaleSeconds)
        _agentStatusLabel->setText(QString("Agent 상태: 응답 없음 (마지막 갱신 %1초 전)").arg(ageSeconds));
    else if (status.connected)
        _agentStatusLabel->setText("Agent 상태: Collector에 연결됨");
    else
        _agentStatusLabel->setText("Agent 상태: Collector 연결 끊김(재시도 중)");
}
```

**변경 사유**: `_worker` 생성자 호출에 `kAgentAlertsDbPath` 인자 추가. `_alertsModel`/`_alertsView`/`_agentStatusLabel` 신규 배선(레이아웃엔 상태 라벨 바로 아래, 차트/지표 테이블보다 위). `AlertsReady`/`StatusReady` 연결 추가. `_pushClient`/`_refreshTimer`는 그대로(새 트리거 없음 - "배경"에서 정한 대로). 창 제목을 "step 2~6'"로 갱신.

### 제안 — CMakeLists.txt (수정, `APM_QtDashboard/`)

**수정 전** (현재 디스크):
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
    MetricsPushClient.cpp
    MetricsPushClient.h
)

target_link_libraries(APM_QtDashboard PRIVATE Qt6::Widgets Qt6::Sql Qt6::Charts Qt6::Network)
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
    MetricsPushClient.cpp
    MetricsPushClient.h
    AgentAlertRepository.cpp
    AgentAlertRepository.h
)

target_link_libraries(APM_QtDashboard PRIVATE Qt6::Widgets Qt6::Sql Qt6::Charts Qt6::Network)
```

**변경 사유**: 신규 파일 2개만 추가 — `find_package`/링크는 변경 없음(Qt SQL 모듈 이미 링크돼 있고, `AgentAlertRepository`도 같은 `QSqlDatabase` API만 씀).

### 제안 — main.cpp (수정)

**수정 전**(현재 디스크 상태 추정 - `AlertSample` 메타타입은 이미 등록돼 있을 가능성이 높음, 작성 시 실제 파일 확인 후 없는 것만 추가할 것):
```cpp
qRegisterMetaType<QVector<MetricsSample>>("QVector<MetricsSample>");
qRegisterMetaType<QVector<AlertSample>>("QVector<AlertSample>");
```

**수정 후** (`AgentStatus` 등록 추가):
```cpp
qRegisterMetaType<QVector<MetricsSample>>("QVector<MetricsSample>");
qRegisterMetaType<QVector<AlertSample>>("QVector<AlertSample>");
qRegisterMetaType<AgentStatus>("AgentStatus");
```
`#include "AgentAlertRepository.h"`도 상단에 추가(`AgentStatus` 정의 위치).

**변경 사유**: `StatusReady(const AgentStatus&)`도 워커→UI 큐 연결로 건너오는 시그널이라 메타타입 등록 필요.

### 검증 (미검증 — 작성/빌드 후 확인)

1. 클린 빌드 성공(`AgentAlertRepository` 신규 컴파일 포함).
2. Agent 실행 후 `agent_alerts.db`에 `agent_status` 1행이 유지되는지(Phase A 때 이미 검증된 부분 재확인 수준).
3. Collector+Agent+Qt 다 띄운 상태 — 상태 라벨이 "확인 중..." → "Collector에 연결됨"으로, 알림 테이블이 정상 표시되는지.
4. **Collector만 죽이기** — Qt가 더 이상 푸시를 못 받으므로, 지표/알림/상태 화면이 전부 **30초 안전망 주기로만** 갱신되는지(이번에 확인한 트레이드오프가 실제로 그렇게 동작하는지 확인하는 게 핵심).
5. **Agent까지 죽이기** — 15초 후 "Agent 응답 없음"으로 바뀌는지(Collector가 살아있어도 지표는 계속 갱신되지만 Agent 상태만 별도로 죽었다고 표시돼야 함).
6. Agent를 아예 한 번도 안 띄운 상태(`agent_alerts.db` 자체가 없음)에서 Qt 실행 — 지표는 정상 표시되고 Agent 상태만 "알 수 없음"으로 뜨는지.

### 결정 사항

문서 제안만 — 실제 소스는 사용자가 직접 작성(원칙 2, [[feedback_claude_md_rule2_scope]]). 완료되면 `WORK_STATUS.md`/`Docs/QT_MFC_PORTFOLIO_PLAN.md` 갱신 후 커밋+push. 이어서 §7(Agent 제어 UI)로 넘어감.
