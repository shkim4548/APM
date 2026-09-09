# SESSION_LOG.md

> 설계 제안·코드 전문을 시간순으로 기록하는 로그.
> VSCode 확장 채팅 패널의 코드 블록 렌더링 품질 문제를 우회하기 위해 이 파일을 별도 탭으로 열어 참고하는 용도
> (원 모노레포 `gw2-cross/SESSION_LOG.md`와 동일한 목적으로 이 저장소에도 재도입, 2026-07-26).
> 최신 항목이 아래에 계속 추가(append)됨 — 위에서 아래로 시간순.

---

## 2026-07-26 — 저장소 이관 + 1순위(부하/스케일 테스트 툴) 설계 제안

### 배경

`gw2-cross` 모노레포에서 포트폴리오 공개용으로 분리한 새 저장소(`APM`, private)에서 후속 기능 개발을 시작. 클론/SSH 인증 확인(이상 없음), README 3종 + `PROJECT_TECHNICAL_REVIEW.md` 전반부를 읽고 현재 구현 상태 파악 완료. 로드맵 우선순위 확정(`WORK_STATUS.md` 참고), 첫 착수 항목은 1순위(부하/스케일 테스트 툴).

### 기존 코드 구조 파악

`APM_Agent/Agent/main.cpp`: `ResilientSender` + `MetricScheduler`로 5초 주기 `apm::Metric` 큐잉 전송.

`APM_Agent/Collector/main.cpp`: `PacketHandler::Register<apm::Metric>`로 저장 콜백 등록, `acceptor.async_accept`로 연결 수락, `ioContext.run()`을 **메인 스레드에서 단 한 번만 호출**(149번째 줄) — 별도 스레드 풀 없음. `GW2_CrossPlatformCore/Network/IoCore.cpp`도 동일 패턴(`_ioContext.run()` 단일 호출).

**핵심 발견**: Collector는 처음부터 "epoll 이벤트 루프 하나가 전부 처리"하는 구조다. 접속 수가 늘어도 소켓 자체의 다중화는 epoll이 효율적으로 처리하지만, 패킷 파싱·복호화(`AesGcmPayload::Open`)·SQLite 저장(`IMetricStore::Store`) 같은 CPU 바운드 작업은 전부 그 한 스레드에서 순차 처리된다. 이게 스케일의 실제 병목이 될 가능성이 높다 — 부하 테스트 결과가 이 가설을 검증해줄 1차 대상이다.

`ApmSession::Send()`의 `SendCallback(bool success)`(`ApmSession.h:55`)는 TCP 쓰기 완료 시점에 호출된다 — "enqueue → 전송 완료" 지연은 잴 수 있지만, "Collector가 실제로 처리를 끝냈는지"는 이 콜백만으로 알 수 없다(프로토콜에 애플리케이션 레벨 ACK가 없음 — 2순위 원격 명령 기능에서 양방향 통신을 만들 때 자연스럽게 열릴 여지).

`ResilientSender`(`ResilientSender.h:29`)의 `maxQueueSize` 기본값 100은 단일 Agent 기준 설계값(`FlushNext()`가 큐 맨 앞부터 순서대로 전송, 가득 차면 `pop_front()`로 오래된 것부터 버림 — `EnqueueRaw()`, `ResilientSender.cpp:77`). 부하 테스트에서 이 기본값을 그대로 쓰면 "Collector의 처리 한계"가 아니라 "Agent 큐 설계의 한계"를 측정하는 착시가 생길 수 있다 — LoadTester는 이 값을 크게 오버라이드해야 한다.

`Metric.proto`(`apm::Metric`): `cpu_usage_percent`(double), `mem_used_bytes`/`mem_total_bytes`/`disk_used_bytes`/`disk_total_bytes`/`net_rx_bytes_per_sec`/`net_tx_bytes_per_sec`(uint64), `tcp_rtt_us`/`tcp_rtt_var_us`/`tcp_retransmits`/`tcp_total_retrans`/`tcp_snd_cwnd`(uint32) 12필드. 부하 테스트에서 이 값들은 고정값/랜덤값으로 채워도 충분 — 측정 대상은 파이프라인 처리 능력이지 지표의 사실성이 아니다.

### LoadTester 설계 제안

**목적**: 동시 Agent 수를 늘려가며 Collector가 몇 개까지 버티는지, 처리량(msg/sec)·지연·에러율(큐 드롭, 재연결)이 어떻게 변하는지 측정 → README의 단일 에이전트 실측치(syscall 1,921회/19.4ms, RSS 13.8MB, CPU 0.017% — `APM_Agent/README.md` "성능/권한 설계" 절)를 N-agent 스케일 기준으로 확장.

**아키텍처(권장)**: `APM_Agent/LoadTester/`에 별도 실행파일을 신설하고, 실제 Agent가 쓰는 `ResilientSender`+`AesGcmPayload`+`apm::Metric`을 그대로 재사용해서 **하나의 io_context 안에 N개의 시뮬레이션 연결**을 생성한다.

- **왜 프로세스 N개 fork 대신 이 방식인가**: asio는 io_context 하나로 스레드 하나에서도 수천 개의 비동기 소켓을 다중화할 수 있다. 실제 Agent 바이너리를 N개 띄우면 OS 프로세스 오버헤드(컨텍스트 스위칭, 각자의 TLS/OpenSSL 초기화 비용)가 측정하고 싶은 "Collector의 처리 능력"과 무관한 잡음으로 섞인다. 기존 프로토콜 코드(`ResilientSender`/`ApmSession`)를 그대로 재사용하므로 Collector 입장에서는 진짜 Agent와 구분 불가능한 트래픽이다.

**파라미터(커맨드라인 인자)**:
- `--agents N`: 동시 시뮬레이션 Agent 수
- `--interval-ms`: 각 시뮬레이션 Agent의 전송 주기(실제 Agent 기본 5000ms보다 훨씬 짧게, 예: 100ms — 스트레스를 걸려면)
- `--duration-sec`: 테스트 지속 시간
- `--ramp-up-ms`: 0이면 전체 동시 connect(thundering herd 테스트), >0이면 이 시간 동안 스태거링 connect
- `--queue-size`: `ResilientSender`의 `maxQueueSize`를 크게 잡아 "Agent 큐 한계"가 "Collector 한계"로 오인되지 않게 함
- `--collector-host`/`--collector-port`: 기본 127.0.0.1:9000

**측정 지표(두 관점)**:
1. **LoadTester 자체 기록**: 연결 성공/실패/재연결 횟수, 초당 전송 성공 건수, enqueue→`Send` 완료 콜백까지의 지연시간 분포(p50/p95/p99), 큐 드롭 횟수
2. **Collector 프로세스 외부 관측**(기존 README 방법론과 동일): `strace -c`로 syscall 프로파일, `/proc/[pid]/status`·`/proc/[pid]/stat`으로 RSS/CPU — 별도 셸 스크립트로 Collector를 감싸서 LoadTester 실행 전/중/후 샘플링

**출력**: 1초마다 진행 상황(연결 수/누적 전송/최근 처리량) 콘솔 출력, 종료 시 요약 통계를 콘솔 표 + CSV 파일로 저장 — 이 수치를 이후 README/`PROJECT_TECHNICAL_REVIEW.md`에 "N-agent 스케일 실측"으로 반영하는 게 최종 목표.

### 결정 사항

- 상태 추적 파일 형식: 원 모노레포 관행(`WORK_STATUS.md` 요약 + `SESSION_LOG.md` 코드/설계 전문 로그) 그대로 재사용하기로 확정(2026-07-26). 파일명도 원본과 동일하게 유지.
- 이 로그 항목까지는 **설계 제안 단계** — 실제 코드 스켈레톤/파일 작성은 다음 단계에서 사용자 확인 후 진행.

---

## 2026-07-26 — `CLAUDE.md` 이관 + 1-3(LoadTester 코드 스켈레톤) 제시

### 배경

`gw2-cross/CLAUDE.md`의 작업 규칙(코드 직접 수정 금지, 수정전/후 스니펫+사유, 정리 섹션, `SESSION_LOG.md` 코드 기록 등)을 이 저장소(`APM/CLAUDE.md`)로 이관. `GW2_ServerCore` 원본 비교 규칙과 `ANALYSIS_STATUS.md` 분석 트랙 규칙은 이 저장소에 해당 파일이 없어 제외.

이어서 로드맵 1순위 1-3(LoadTester 코드 스켈레톤 제시) 착수. `ResilientSender`/`ApmSession`/`PacketHandler`/`Metric.proto`/`Agent/main.cpp`/`CMakeLists.txt`를 다시 읽고 확인한 뒤 설계.

### 발견한 제약 — `ResilientSender::Enqueue`는 완료 콜백이 없음

SESSION_LOG 2026-07-26(1차 항목) 설계안의 측정 지표 중 "enqueue → Send 완료 콜백까지의 지연시간 분포(p50/p95/p99)"를 재려면, 패킷 단위로 전송 완료 시점을 알아야 한다. 그런데 현재 `ResilientSender::Enqueue<PacketType>()`/`EnqueueRaw()`는 완료 콜백을 받지 않는다 — 내부적으로 `FlushNext()`가 `_session->Send()`에 넘기는 콜백은 큐 관리(재시도/pop) 전용이고 호출자에게는 전혀 노출되지 않는다.

**대안 비교**:
1. `ResilientSender`에 선택적 완료 콜백 파라미터를 추가(기본값 `nullptr` — 기존 `Agent/main.cpp` 호출부는 수정 없이 그대로 동작). **채택.**
2. `SimulatedAgent`가 `ResilientSender`를 우회하고 `ApmSession::SendPacket()`을 직접 호출 — 콜백은 되지만 재연결/큐잉 복원력을 잃어서 "진짜 Agent와 구분 불가능한 트래픽"이라는 설계 원칙(1차 로그, "왜 프로세스 N개 fork 대신 이 방식인가")이 깨짐. **기각.**
3. 지연시간 대신 처리량(초당 성공 전송 건수)만 측정 — 측정 항목이 빈약해짐. **기각.**

### 제안 1 — `ResilientSender` 수정 (수정 전 / 수정 후)

**변경 사유**: 패킷별 enqueue→전송완료 지연시간을 `LoadTestStats`가 측정할 수 있게 선택적 콜백을 추가. 기본값 `nullptr`이라 기존 호출부(`Agent/main.cpp`)는 동작 변화 없음.

**`ResilientSender.h` — `QueuedPacket` 구조체**

수정 전:
```cpp
struct QueuedPacket
{
	uint16 id;
	String payload;
};
```

수정 후:
```cpp
struct QueuedPacket
{
	uint16 id;
	String payload;
	std::function<void(bool success)> onComplete;   // nullptr 허용 - 콜백 없는 기존 EnqueueRaw 호출과 호환
};
```

**`ResilientSender.h` — 클래스 선언(멤버 변수 선언 전체는 변경 없음, `public`/`private` 함수 시그니처만 아래처럼 변경)**

수정 전:
```cpp
class ResilientSender
{
public:
	using SealerFactory = std::function<std::unique_ptr<IPayloadSealer>()>;

	ResilientSender(asio::io_context& ioContext, asio::ssl::context& sslContext,
		String host, unsigned short port,
		SealerFactory sealerFactory, size_t maxQueueSize = 100);

	template<typename PacketType>
	void Enqueue(const PacketType& pkt)
	{
		String payload;
		if (!pkt.SerializeToString(&payload))
		{
			std::cerr << "[ResilientSender] serialize failed" << std::endl;
			return;
		}

		uint16 id = static_cast<uint16>(PacketType::descriptor()->index());
		EnqueueRaw(id, std::move(payload));
	}

	TcpConnectionInfo GetConnectionInfo() const;

private:
	void EnqueueRaw(uint16 id, String payload);
	void Connect();
	void ScheduleReconnect();
	void FlushNext();
	void OnSessionDisconnected();

private:
	asio::io_context& _ioContext;
	asio::ssl::context& _sslContext;
	String _host;
	unsigned short _port;
	SealerFactory _sealerFactory;
	size_t _maxQueueSize;

	std::deque<QueuedPacket> _queue;
	std::shared_ptr<ApmSession> _session;
	asio::steady_timer _reconnectTimer;
	bool _connected = false;
	bool _sending = false;
};
```

수정 후:
```cpp
class ResilientSender
{
public:
	using SealerFactory = std::function<std::unique_ptr<IPayloadSealer>()>;
	using SendCallback = std::function<void(bool success)>;

	ResilientSender(asio::io_context& ioContext, asio::ssl::context& sslContext,
		String host, unsigned short port,
		SealerFactory sealerFactory, size_t maxQueueSize = 100);

	// onComplete: 이 패킷이 실제로 전송 완료(성공)됐을 때 1회 호출. 기본값 nullptr -
	// 기존 호출부(Agent/main.cpp)는 수정 없이 그대로 동작(콜백 없이 fire-and-forget).
	// LoadTester가 enqueue -> 전송완료 지연시간을 재려고 추가(2026-07-26).
	template<typename PacketType>
	void Enqueue(const PacketType& pkt, SendCallback onComplete = nullptr)
	{
		String payload;
		if (!pkt.SerializeToString(&payload))
		{
			std::cerr << "[ResilientSender] serialize failed" << std::endl;
			return;
		}

		uint16 id = static_cast<uint16>(PacketType::descriptor()->index());
		EnqueueRaw(id, std::move(payload), std::move(onComplete));
	}

	TcpConnectionInfo GetConnectionInfo() const;

private:
	void EnqueueRaw(uint16 id, String payload, SendCallback onComplete = nullptr);
	void Connect();
	void ScheduleReconnect();
	void FlushNext();
	void OnSessionDisconnected();

private:
	asio::io_context& _ioContext;
	asio::ssl::context& _sslContext;
	String _host;
	unsigned short _port;
	SealerFactory _sealerFactory;
	size_t _maxQueueSize;

	std::deque<QueuedPacket> _queue;
	std::shared_ptr<ApmSession> _session;
	asio::steady_timer _reconnectTimer;
	bool _connected = false;
	bool _sending = false;
};
```

**`ResilientSender.cpp` — `EnqueueRaw()`**

수정 전:
```cpp
void ResilientSender::EnqueueRaw(uint16 id, String payload)
{
	if (_queue.size() >= _maxQueueSize)
	{
		std::cerr << "[ResilientSender] queue full, dropping oldest buffered item" << std::endl;
		_queue.pop_front();
	}

	_queue.push_back(QueuedPacket{ id, std::move(payload) });
	std::cout << "[ResilientSender] queued (size=" << _queue.size() << ")" << std::endl;

	if (_connected && !_sending)
		FlushNext();
}
```

수정 후:
```cpp
void ResilientSender::EnqueueRaw(uint16 id, String payload, SendCallback onComplete)
{
	if (_queue.size() >= _maxQueueSize)
	{
		std::cerr << "[ResilientSender] queue full, dropping oldest buffered item" << std::endl;
		// 버려지는 항목의 콜백도 실패로 통지 - 콜백이 아예 안 불리면 LoadTester가
		// "드롭"과 "아직 응답 대기 중"을 구분할 수 없음.
		if (_queue.front().onComplete)
			_queue.front().onComplete(false);
		_queue.pop_front();
	}

	_queue.push_back(QueuedPacket{ id, std::move(payload), std::move(onComplete) });
	std::cout << "[ResilientSender] queued (size=" << _queue.size() << ")" << std::endl;

	if (_connected && !_sending)
		FlushNext();
}
```

**`ResilientSender.cpp` — `FlushNext()`**

수정 전:
```cpp
void ResilientSender::FlushNext()
{
	if (_queue.empty() || !_connected || _sending || !_session)
		return;

	_sending = true;
	QueuedPacket packet = _queue.front();

	_session->Send(packet.id, packet.payload,
		[this](bool success)
		{
			_sending = false;

			if (success)
			{
				if (!_queue.empty())
					_queue.pop_front();
				FlushNext();
			}
			// 실패하면 큐에 그대로 남겨둠 - OnSessionDisconnected가 별도로 호출되어
			// 재연결을 예약하고, 재연결 성공 시 FlushNext()가 이 항목부터 다시 재시도함.
		});
}
```

수정 후:
```cpp
void ResilientSender::FlushNext()
{
	if (_queue.empty() || !_connected || _sending || !_session)
		return;

	_sending = true;
	QueuedPacket packet = _queue.front();

	_session->Send(packet.id, packet.payload,
		[this, onComplete = packet.onComplete](bool success)
		{
			_sending = false;

			if (success)
			{
				if (onComplete)
					onComplete(true);
				if (!_queue.empty())
					_queue.pop_front();
				FlushNext();
			}
			// 실패하면 큐에 그대로 남겨둠 - OnSessionDisconnected가 별도로 호출되어
			// 재연결을 예약하고, 재연결 성공 시 FlushNext()가 이 항목부터 다시 재시도함.
			// (여기서는 onComplete(false)를 호출하지 않음 - 아직 "최종 실패"가 아니라
			// 재시도 대기 상태이므로, LoadTester에는 재연결 후 실제 성공/드롭 시점에만 통지)
		});
}
```

### 제안 2 — `APM_Agent/LoadTester/` 신설 (코드 스켈레톤)

**디렉토리 구성**: `LoadTester/{main.cpp, LoadTesterConfig.h/.cpp, LoadTestStats.h/.cpp, SimulatedAgent.h/.cpp, LoadTester.h/.cpp}` — 기존 `Agent/`, `Collector/`와 동일한 패턴(디렉토리당 실행 파일 하나).

**`LoadTesterConfig.h`**
```cpp
#pragma once
#include "pch.h"

/*-------------------
    LoadTesterConfig
---------------------*/
// 커맨드라인 인자를 파싱해서 LoadTester 실행에 필요한 파라미터로 변환.
// SESSION_LOG.md 2026-07-26(1차 항목) 설계안의 파라미터 목록을 그대로 반영.

struct LoadTesterConfig
{
    int agentCount = 10;                                // --agents
    std::chrono::milliseconds sendInterval{ 100 };      // --interval-ms
    std::chrono::seconds duration{ 60 };                 // --duration-sec
    std::chrono::milliseconds rampUp{ 0 };               // --ramp-up-ms (0=전체 동시 connect)
    size_t queueSize = 10000;                            // --queue-size (ResilientSender maxQueueSize 오버라이드)
    String collectorHost = "127.0.0.1";                  // --collector-host
    unsigned short collectorPort = 9000;                 // --collector-port
    String csvOutputPath = "load_test_result.csv";       // --csv-out

    // argv 파싱. 실패(알 수 없는 인자/잘못된 값) 시 std::runtime_error -
    // main()에서 사용법 출력 후 종료하는 용도.
    static LoadTesterConfig Parse(int argc, char** argv);
};
```

**`LoadTestStats.h`**
```cpp
#pragma once
#include "pch.h"

/*-------------------
    LoadTestStats
---------------------*/
// io_context 이벤트 루프(단일 스레드)에서만 접근된다는 전제 - 별도 스레드가 없으므로
// 락 없이 단순 카운터/vector로 구현.

class LoadTestStats
{
public:
    void OnConnectSuccess();
    void OnConnectFail();
    void OnReconnect();
    void OnQueueDrop();
    void OnPacketSent(std::chrono::milliseconds latency);   // enqueue -> Send 완료까지 지연

    // 마지막 호출 이후의 델타만 계산해 콘솔 한 줄 출력(최근 처리량 파악용) 후 델타 리셋.
    void PrintProgressAndReset(std::chrono::seconds elapsed);

    // 종료 시 누적 통계 + 지연시간 분포(p50/p95/p99)를 CSV로 저장.
    void DumpCsv(const String& path) const;

private:
    static std::chrono::milliseconds Percentile(std::vector<std::chrono::milliseconds> sorted, double p);

private:
    uint64_t _connectSuccessCount = 0;
    uint64_t _connectFailCount = 0;
    uint64_t _reconnectCount = 0;
    uint64_t _queueDropCount = 0;
    uint64_t _sentCount = 0;
    uint64_t _sentSinceLastReport = 0;
    std::vector<std::chrono::milliseconds> _latencySamples;   // 주의: 무제한 누적 - 아래 "확인 필요" 참고
};
```

**`SimulatedAgent.h`**
```cpp
#pragma once
#include "pch.h"
#include "ResilientSender.h"
#include "LoadTesterConfig.h"
#include "LoadTestStats.h"

/*-------------------
    SimulatedAgent
---------------------*/
// 실제 Agent 한 대에 대응하는 시뮬레이션 단위. ResilientSender를 그대로 재사용해서
// Collector 입장에서는 진짜 Agent와 구분 불가능한 트래픽을 만든다.
// MetricScheduler(ResourceCollector 의존)는 재사용하지 않음 - 부하 테스트는 실측 리소스가
// 아니라 고정/랜덤값으로 충분(1차 로그 설계안 참고), 불필요한 결합을 만들지 않기 위함.

class SimulatedAgent
{
public:
    SimulatedAgent(asio::io_context& ioContext, asio::ssl::context& sslContext,
        const LoadTesterConfig& config, LoadTestStats& stats, int agentIndex);

    void Start();
    void Stop();

private:
    void ScheduleNextSend();
    apm::Metric BuildFakeMetric() const;

private:
    asio::io_context& _ioContext;
    const LoadTesterConfig& _config;
    LoadTestStats& _stats;
    int _agentIndex;
    ResilientSender _sender;
    asio::steady_timer _sendTimer;
    bool _running = false;
};
```

**`LoadTester.h`**
```cpp
#pragma once
#include "pch.h"
#include "LoadTesterConfig.h"
#include "LoadTestStats.h"
#include "SimulatedAgent.h"

/*-------------------
    LoadTester
---------------------*/
// 오케스트레이터. io_context 하나에 N개의 SimulatedAgent를 램프업 스케줄에 맞춰 생성하고,
// 지정된 duration 동안 실행한 뒤 정리하고 CSV를 남긴다.

class LoadTester
{
public:
    explicit LoadTester(LoadTesterConfig config);

    // 블로킹 - ioContext.run()을 내부에서 호출하고 duration 경과 시 스스로 정지.
    void Run();

private:
    void SpawnAgents();          // ramp-up 스케줄에 맞춰 SimulatedAgent 생성(동시 또는 스태거링)
    void ScheduleProgressReport();
    void ScheduleShutdown();
    void Shutdown();

private:
    LoadTesterConfig _config;
    asio::io_context _ioContext;
    asio::ssl::context _sslContext;
    AesGcmCipher::Key _agentCollectorKey;
    LoadTestStats _stats;
    std::vector<std::unique_ptr<SimulatedAgent>> _agents;
    asio::steady_timer _spawnTimer;
    asio::steady_timer _reportTimer;
    asio::steady_timer _shutdownTimer;
    std::chrono::steady_clock::time_point _startTime;
};
```

**`main.cpp`**
```cpp
#include "pch.h"
#include "LoadTesterConfig.h"
#include "LoadTester.h"

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        LoadTesterConfig config = LoadTesterConfig::Parse(argc, argv);
        LoadTester tester(std::move(config));
        tester.Run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[LoadTester] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

**`CMakeLists.txt`(APM_Agent 루트) 추가분**
```cmake
add_executable(LoadTester
    LoadTester/main.cpp
    LoadTester/LoadTesterConfig.cpp
    LoadTester/LoadTestStats.cpp
    LoadTester/SimulatedAgent.cpp
    LoadTester/LoadTester.cpp
    pch.cpp
)
target_link_libraries(LoadTester PRIVATE APM_Common)
```

### 확인 필요(1-4 착수 전 사용자 결정 사항)

1. `ResilientSender::Enqueue`/`EnqueueRaw`에 선택적 콜백 파라미터를 추가하는 위 변경에 동의하는지 — 기존 `Agent/main.cpp` 동작에는 영향 없음(기본값 `nullptr`).
2. `LoadTestStats::_latencySamples`가 실행 시간 내내 무제한 누적됨 — `--agents 100 --interval-ms 100 --duration-sec 600`이면 샘플 60만 개(약 5MB, `milliseconds` 8바이트 기준)라 이 정도는 문제없지만, 더 긴 시간/더 많은 에이전트로 돌릴 계획이 있다면 온라인 percentile(예: t-digest)이나 주기적 다운샘플링으로 바꿀지 미리 정해두는 게 나음.
3. `SimulatedAgent`가 `MetricScheduler`/`ResourceCollector`를 쓰지 않고 고정/랜덤 `apm::Metric` 값을 직접 만드는 방향 재확인.
4. Collector 쪽에는 세션 레지스트리가 없어(`WORK_STATUS.md` 아키텍처 제약 3번) "몇 개 연결이 살아있는지"를 Collector 쪽에서 볼 방법이 없음 — 지금은 `LoadTester` 자체 기록(connect success/fail 카운트)만으로 충분하다고 보고 Collector는 건드리지 않는 방향으로 설계함. 2순위(원격 명령) 착수 때 세션 레지스트리가 생기면 그때 Collector 쪽 관측치도 추가 가능.

### 결정 사항

- 이 항목까지도 **설계 제안 단계** — 실제 파일 생성/`CMakeLists.txt` 수정은 사용자가 위 4개 확인 사항에 답하고 "적용해줘"라고 명시한 뒤(1-4) 진행.

---

## 2026-07-26 — 1-4(LoadTester 구현) 완성 제안서

### 배경

앞 항목의 "확인 필요 4건"에 전부 추천안대로 확정 답변을 받음(콜백 추가 동의 / 현재 규모면 지연샘플 무제한 누적 유지 / 고정·랜덤 Metric 값 확정 / Collector 세션 레지스트리 미보강 확정). 다만 `CLAUDE.md` 규칙 2에 따라 코드 직접 수정은 사용자가 명시적으로 요청했을 때만 하는 것이 원칙이고, 이번엔 "제안서 갱신 방식으로 안내하고 적용은 직접 하겠다"는 요청을 받아 **실제 파일은 만들지 않고** 바로 적용 가능한 완성된 전문(全文)을 이 로그에 남긴다. 실제 디스크 파일(`ResilientSender.h/.cpp`, `APM_Agent/CMakeLists.txt`, `Agent/main.cpp` 등)을 다시 읽어 확인한 뒤 아래 전문을 작성함.

### 설계 중 추가로 발견한 것 — 연결 상태 콜백 필요

이전 항목에서 승인받은 변경은 "패킷 단위 완료 콜백"(`SendCallback`) 하나뿐이었다. 그런데 `LoadTestStats`의 승인된 스켈레톤에는 `OnConnectSuccess()`/`OnConnectFail()`/`OnReconnect()`가 이미 있고, 이 값들을 채우려면 `ResilientSender`가 "연결 성공/실패/끊김" 시점도 바깥으로 알려줘야 한다. 그런데 현재 `ResilientSender::Connect()`/`OnSessionDisconnected()`는 전부 `private`이고 바깥에 연결 상태를 알리는 통로가 없다.

**대안 비교**:
1. `ResilientSender` 생성자에 선택적 `ConnectionStateCallback`(연결 성공 시 `true`, resolve/connect 실패나 끊김 감지 시 `false`) 파라미터를 추가. 기본값 `nullptr`이라 기존 `Agent/main.cpp` 호출부는 영향 없음. **채택.**
2. `SimulatedAgent`가 `GetConnectionInfo()`를 폴링해서 값이 0이 아니면 "연결됨"으로 간주 — 연결은 됐지만 아직 지표가 0인 순간과 구분이 안 되는 데다, resolve/connect 단계 실패는 애초에 감지 불가. **기각.**
3. 연결 상태 추적 자체를 포기하고 `LoadTestStats`에서 `OnConnectSuccess`/`OnConnectFail`/`OnReconnect`를 빼거나 항상 0으로 둠 — 이미 확정된 스켈레톤(1-3 항목)을 축소하게 됨. **기각.**

`SimulatedAgent`는 이 콜백에서 `true`가 처음 오면 `OnConnectSuccess()`, 이후 다시 `true`가 오면(한 번이라도 끊긴 뒤 재연결) `OnReconnect()`, 한 번도 연결된 적 없는 상태에서 `false`가 오면 `OnConnectFail()`을 호출하는 식으로 구분한다(`_everConnected` 플래그 사용).

### 제안 1 — `ResilientSender` 수정 (수정 전 / 수정 후)

**변경 사유**: (a) 패킷별 enqueue→전송완료 지연시간을 잴 수 있게 선택적 완료 콜백 추가(1-3 항목에서 이미 승인), (b) 연결 성공/실패/재연결을 바깥에서 관측할 수 있게 선택적 연결상태 콜백 추가(이번 항목에서 새로 필요성 발견, 위 참고). 둘 다 기본값 `nullptr`이라 `Agent/main.cpp`는 코드 변경 없이 그대로 동작.

**`ResilientSender.h` — 전체 파일**

수정 전(현재 디스크 상태, `APM_Agent/Common/ResilientSender.h`):
```cpp
#pragma once
#include "pch.h"
#include "ApmSession.h"

/*-------------------
	ResilientSender
---------------------*/
// 상대(Collector 또는 WebServer)에 대한 연결을 유지/재연결하고, 연결이 끊긴 동안
// 보낼 데이터를 로컬 큐(메모리)에 쌓아뒀다가 재연결되면 순서대로 재전송한다.
// 큐가 가득 차면 가장 오래된 항목부터 버린다(최신 데이터가 더 중요하다는 가정).
// 주의: 메모리 큐라 프로세스 자체가 죽으면 큐 내용도 사라짐 (디스크 영속화는 범위 밖).

struct QueuedPacket
{
	uint16 id;
	String payload;
};

class ResilientSender
{
public:
	// sealerFactory: 재연결마다 새 ApmSession에 넘길 IPayloadSealer를 새로 만들어주는 함수.
	// (SecurePayload/AesGcmCipher 둘 다 키만 들고 있는 상태 없는 객체라 매번 새로 만들어도
	// 비용이 거의 없음 - 재연결마다 깨끗한 상태로 시작한다는 걸 보장하는 쪽을 택함.)
	// Agent->Collector는 [] { return std::make_unique<SecurePayload>(encKey, macKey); } 형태로,
	// Collector->WebServer는 [] { return std::make_unique<AesGcmPayload>(key); } 형태로 넘김.
	using SealerFactory = std::function<std::unique_ptr<IPayloadSealer>()>;

	ResilientSender(asio::io_context& ioContext, asio::ssl::context& sslContext,
		String host, unsigned short port,
		SealerFactory sealerFactory, size_t maxQueueSize = 100);

	// Protobuf 메시지를 직접 받아서 직렬화 후 큐에 넣음 - 타입 안전 진입점.
	template<typename PacketType>
	void Enqueue(const PacketType& pkt)
	{
		String payload;
		if (!pkt.SerializeToString(&payload))
		{
			std::cerr << "[ResilientSender] serialize failed" << std::endl;
			return;
		}

		uint16 id = static_cast<uint16>(PacketType::descriptor()->index());
		EnqueueRaw(id, std::move(payload));
	}

	// 현재 연결의 TCP 품질 조회. 연결 안 된 상태면 전부 0인 기본값.
	TcpConnectionInfo GetConnectionInfo() const;

private:
	void EnqueueRaw(uint16 id, String payload);
	void Connect();
	void ScheduleReconnect();
	void FlushNext();
	void OnSessionDisconnected();

private:
	asio::io_context& _ioContext;
	asio::ssl::context& _sslContext;
	String _host;
	unsigned short _port;
	SealerFactory _sealerFactory;
	size_t _maxQueueSize;

	std::deque<QueuedPacket> _queue;
	std::shared_ptr<ApmSession> _session;
	asio::steady_timer _reconnectTimer;
	bool _connected = false;
	bool _sending = false;
};
```

수정 후(전체 교체):
```cpp
#pragma once
#include "pch.h"
#include "ApmSession.h"

/*-------------------
	ResilientSender
---------------------*/
// 상대(Collector 또는 WebServer)에 대한 연결을 유지/재연결하고, 연결이 끊긴 동안
// 보낼 데이터를 로컬 큐(메모리)에 쌓아뒀다가 재연결되면 순서대로 재전송한다.
// 큐가 가득 차면 가장 오래된 항목부터 버린다(최신 데이터가 더 중요하다는 가정).
// 주의: 메모리 큐라 프로세스 자체가 죽으면 큐 내용도 사라짐 (디스크 영속화는 범위 밖).

struct QueuedPacket
{
	uint16 id;
	String payload;
	std::function<void(bool success)> onComplete;   // nullptr 허용 - 콜백 없는 기존 EnqueueRaw 호출과 호환
};

class ResilientSender
{
public:
	// sealerFactory: 재연결마다 새 ApmSession에 넘길 IPayloadSealer를 새로 만들어주는 함수.
	// (SecurePayload/AesGcmCipher 둘 다 키만 들고 있는 상태 없는 객체라 매번 새로 만들어도
	// 비용이 거의 없음 - 재연결마다 깨끗한 상태로 시작한다는 걸 보장하는 쪽을 택함.)
	// Agent->Collector는 [] { return std::make_unique<SecurePayload>(encKey, macKey); } 형태로,
	// Collector->WebServer는 [] { return std::make_unique<AesGcmPayload>(key); } 형태로 넘김.
	using SealerFactory = std::function<std::unique_ptr<IPayloadSealer>()>;
	using SendCallback = std::function<void(bool success)>;
	// connected: TLS 핸드셰이크 완료 시 true, resolve/connect 실패 또는 연결 끊김 감지 시
	// false. 재연결마다 다시 호출됨 - LoadTester가 connect success/fail/reconnect 횟수를
	// 셀 수 있게 추가(2026-07-26). 기본값 nullptr - 기존 호출부(Agent/main.cpp)는 동작 변화 없음.
	using ConnectionStateCallback = std::function<void(bool connected)>;

	ResilientSender(asio::io_context& ioContext, asio::ssl::context& sslContext,
		String host, unsigned short port,
		SealerFactory sealerFactory, size_t maxQueueSize = 100,
		ConnectionStateCallback onConnectionStateChanged = nullptr);

	// Protobuf 메시지를 직접 받아서 직렬화 후 큐에 넣음 - 타입 안전 진입점.
	// onComplete: 이 패킷이 실제로 전송 완료(성공)됐을 때 1회 호출. 기본값 nullptr -
	// 기존 호출부(Agent/main.cpp)는 수정 없이 그대로 동작(콜백 없이 fire-and-forget).
	// LoadTester가 enqueue -> 전송완료 지연시간을 재려고 추가(2026-07-26).
	template<typename PacketType>
	void Enqueue(const PacketType& pkt, SendCallback onComplete = nullptr)
	{
		String payload;
		if (!pkt.SerializeToString(&payload))
		{
			std::cerr << "[ResilientSender] serialize failed" << std::endl;
			return;
		}

		uint16 id = static_cast<uint16>(PacketType::descriptor()->index());
		EnqueueRaw(id, std::move(payload), std::move(onComplete));
	}

	// 현재 연결의 TCP 품질 조회. 연결 안 된 상태면 전부 0인 기본값.
	TcpConnectionInfo GetConnectionInfo() const;

private:
	void EnqueueRaw(uint16 id, String payload, SendCallback onComplete = nullptr);
	void Connect();
	void ScheduleReconnect();
	void FlushNext();
	void OnSessionDisconnected();

private:
	asio::io_context& _ioContext;
	asio::ssl::context& _sslContext;
	String _host;
	unsigned short _port;
	SealerFactory _sealerFactory;
	size_t _maxQueueSize;
	ConnectionStateCallback _onConnectionStateChanged;

	std::deque<QueuedPacket> _queue;
	std::shared_ptr<ApmSession> _session;
	asio::steady_timer _reconnectTimer;
	bool _connected = false;
	bool _sending = false;
};
```

**`ResilientSender.cpp` — 전체 파일**

수정 전(현재 디스크 상태, `APM_Agent/Common/ResilientSender.cpp`):
```cpp
#include "pch.h"
#include "ResilientSender.h"
#include "PacketHandler.h"

ResilientSender::ResilientSender(asio::io_context& ioContext, asio::ssl::context& sslContext,
	String host, unsigned short port,
	SealerFactory sealerFactory, size_t maxQueueSize)
	: _ioContext(ioContext), _sslContext(sslContext), _host(std::move(host)), _port(port)
	, _sealerFactory(std::move(sealerFactory)), _maxQueueSize(maxQueueSize), _reconnectTimer(ioContext)
{
	Connect();
}

void ResilientSender::Connect()
{
	auto resolver = std::make_shared<asio::ip::tcp::resolver>(_ioContext);
	resolver->async_resolve(_host, std::to_string(_port),
		[this, resolver](const asio::error_code& ec, asio::ip::tcp::resolver::results_type endpoints)
		{
			if (ec)
			{
				std::cerr << "[ResilientSender] resolve failed: " << ec.message() << std::endl;
				ScheduleReconnect();
				return;
			}

			auto socket = std::make_shared<asio::ip::tcp::socket>(_ioContext);
			asio::async_connect(*socket, endpoints,
				[this, socket](const asio::error_code& ec, const asio::ip::tcp::endpoint&)
				{
					if (ec)
					{
						std::cerr << "[ResilientSender] connect failed: " << ec.message() << std::endl;
						ScheduleReconnect();
						return;
					}

					std::cout << "[ResilientSender] connected" << std::endl;

					_session = std::make_shared<ApmSession>(std::move(*socket), _sslContext, SessionMode::Client, _sealerFactory());
					_session->Start(
						[this]()
						{
							std::cout << "[ResilientSender] TLS handshake complete" << std::endl;
							_connected = true;
							FlushNext();
						},
						[this]()
						{
							OnSessionDisconnected();
						},
						&PacketHandler::Dispatch);
				});
		});
}

void ResilientSender::ScheduleReconnect()
{
	_reconnectTimer.expires_after(std::chrono::seconds(5));
	_reconnectTimer.async_wait(
		[this](const asio::error_code& ec)
		{
			if (!ec)
				Connect();
		});
}

void ResilientSender::OnSessionDisconnected()
{
	std::cerr << "[ResilientSender] disconnected, will retry" << std::endl;
	_connected = false;
	_sending = false;
	_session = nullptr;
	ScheduleReconnect();
}

void ResilientSender::EnqueueRaw(uint16 id, String payload)
{
	if (_queue.size() >= _maxQueueSize)
	{
		std::cerr << "[ResilientSender] queue full, dropping oldest buffered item" << std::endl;
		_queue.pop_front();
	}

	_queue.push_back(QueuedPacket{ id, std::move(payload) });
	std::cout << "[ResilientSender] queued (size=" << _queue.size() << ")" << std::endl;

	if (_connected && !_sending)
		FlushNext();
}

void ResilientSender::FlushNext()
{
	if (_queue.empty() || !_connected || _sending || !_session)
		return;

	_sending = true;
	QueuedPacket packet = _queue.front();

	_session->Send(packet.id, packet.payload,
		[this](bool success)
		{
			_sending = false;

			if (success)
			{
				if (!_queue.empty())
					_queue.pop_front();
				FlushNext();
			}
			// 실패하면 큐에 그대로 남겨둠 - OnSessionDisconnected가 별도로 호출되어
			// 재연결을 예약하고, 재연결 성공 시 FlushNext()가 이 항목부터 다시 재시도함.
		});
}

TcpConnectionInfo ResilientSender::GetConnectionInfo() const
{
	if (_connected && _session)
		return _session->GetConnectionInfo();

	return TcpConnectionInfo{};
}
```

수정 후(전체 교체):
```cpp
#include "pch.h"
#include "ResilientSender.h"
#include "PacketHandler.h"

ResilientSender::ResilientSender(asio::io_context& ioContext, asio::ssl::context& sslContext,
	String host, unsigned short port,
	SealerFactory sealerFactory, size_t maxQueueSize,
	ConnectionStateCallback onConnectionStateChanged)
	: _ioContext(ioContext), _sslContext(sslContext), _host(std::move(host)), _port(port)
	, _sealerFactory(std::move(sealerFactory)), _maxQueueSize(maxQueueSize)
	, _onConnectionStateChanged(std::move(onConnectionStateChanged)), _reconnectTimer(ioContext)
{
	Connect();
}

void ResilientSender::Connect()
{
	auto resolver = std::make_shared<asio::ip::tcp::resolver>(_ioContext);
	resolver->async_resolve(_host, std::to_string(_port),
		[this, resolver](const asio::error_code& ec, asio::ip::tcp::resolver::results_type endpoints)
		{
			if (ec)
			{
				std::cerr << "[ResilientSender] resolve failed: " << ec.message() << std::endl;
				if (_onConnectionStateChanged)
					_onConnectionStateChanged(false);
				ScheduleReconnect();
				return;
			}

			auto socket = std::make_shared<asio::ip::tcp::socket>(_ioContext);
			asio::async_connect(*socket, endpoints,
				[this, socket](const asio::error_code& ec, const asio::ip::tcp::endpoint&)
				{
					if (ec)
					{
						std::cerr << "[ResilientSender] connect failed: " << ec.message() << std::endl;
						if (_onConnectionStateChanged)
							_onConnectionStateChanged(false);
						ScheduleReconnect();
						return;
					}

					std::cout << "[ResilientSender] connected" << std::endl;

					_session = std::make_shared<ApmSession>(std::move(*socket), _sslContext, SessionMode::Client, _sealerFactory());
					_session->Start(
						[this]()
						{
							std::cout << "[ResilientSender] TLS handshake complete" << std::endl;
							_connected = true;
							if (_onConnectionStateChanged)
								_onConnectionStateChanged(true);
							FlushNext();
						},
						[this]()
						{
							OnSessionDisconnected();
						},
						&PacketHandler::Dispatch);
				});
		});
}

void ResilientSender::ScheduleReconnect()
{
	_reconnectTimer.expires_after(std::chrono::seconds(5));
	_reconnectTimer.async_wait(
		[this](const asio::error_code& ec)
		{
			if (!ec)
				Connect();
		});
}

void ResilientSender::OnSessionDisconnected()
{
	std::cerr << "[ResilientSender] disconnected, will retry" << std::endl;
	_connected = false;
	_sending = false;
	_session = nullptr;
	if (_onConnectionStateChanged)
		_onConnectionStateChanged(false);
	ScheduleReconnect();
}

void ResilientSender::EnqueueRaw(uint16 id, String payload, SendCallback onComplete)
{
	if (_queue.size() >= _maxQueueSize)
	{
		std::cerr << "[ResilientSender] queue full, dropping oldest buffered item" << std::endl;
		// 버려지는 항목의 콜백도 실패로 통지 - 콜백이 아예 안 불리면 LoadTester가
		// "드롭"과 "아직 응답 대기 중"을 구분할 수 없음.
		if (_queue.front().onComplete)
			_queue.front().onComplete(false);
		_queue.pop_front();
	}

	_queue.push_back(QueuedPacket{ id, std::move(payload), std::move(onComplete) });
	std::cout << "[ResilientSender] queued (size=" << _queue.size() << ")" << std::endl;

	if (_connected && !_sending)
		FlushNext();
}

void ResilientSender::FlushNext()
{
	if (_queue.empty() || !_connected || _sending || !_session)
		return;

	_sending = true;
	QueuedPacket packet = _queue.front();

	_session->Send(packet.id, packet.payload,
		[this, onComplete = packet.onComplete](bool success)
		{
			_sending = false;

			if (success)
			{
				if (onComplete)
					onComplete(true);
				if (!_queue.empty())
					_queue.pop_front();
				FlushNext();
			}
			// 실패하면 큐에 그대로 남겨둠 - OnSessionDisconnected가 별도로 호출되어
			// 재연결을 예약하고, 재연결 성공 시 FlushNext()가 이 항목부터 다시 재시도함.
			// (여기서는 onComplete(false)를 호출하지 않음 - 아직 "최종 실패"가 아니라
			// 재시도 대기 상태이므로, LoadTester에는 재연결 후 실제 성공/드롭 시점에만 통지)
		});
}

TcpConnectionInfo ResilientSender::GetConnectionInfo() const
{
	if (_connected && _session)
		return _session->GetConnectionInfo();

	return TcpConnectionInfo{};
}
```

### 제안 2 — `APM_Agent/LoadTester/` 신설 (전체 파일, 적용 가능한 완성본)

새 디렉토리라 "수정 전/후" 대신 최종본 전문만 제시. `APM_Agent/pch.h`, `ApmSession.h`, `AesGcmCipher.h`, `KeyLoader.h`, `AesGcmPayload.h`, `Metric.proto`, `Agent/main.cpp`(실제 Agent가 쓰는 키 파일 경로 `certs/agent_collector_aes.key` 확인)를 다시 읽고 실제 시그니처에 맞춰 작성함.

**`LoadTester/LoadTesterConfig.h`**
```cpp
#pragma once
#include "pch.h"

/*-------------------
    LoadTesterConfig
---------------------*/
// 커맨드라인 인자를 파싱해서 LoadTester 실행에 필요한 파라미터로 변환.

struct LoadTesterConfig
{
    int agentCount = 10;                                  // --agents
    std::chrono::milliseconds sendInterval{ 100 };        // --interval-ms
    std::chrono::seconds duration{ 60 };                   // --duration-sec
    std::chrono::milliseconds rampUp{ 0 };                 // --ramp-up-ms (0=전체 동시 connect)
    size_t queueSize = 10000;                              // --queue-size (ResilientSender maxQueueSize 오버라이드)
    String collectorHost = "127.0.0.1";                    // --collector-host
    unsigned short collectorPort = 9000;                   // --collector-port
    String csvOutputPath = "load_test_result.csv";         // --csv-out
    String keyFilePath = "certs/agent_collector_aes.key";  // --key-file (Agent/main.cpp와 동일 키 - 다르면 Collector가 복호화 실패)

    // argv 파싱. 실패(알 수 없는 인자/잘못된 값) 시 std::runtime_error -
    // main()에서 사용법 출력 후 종료하는 용도.
    static LoadTesterConfig Parse(int argc, char** argv);
};
```

**`LoadTester/LoadTesterConfig.cpp`**
```cpp
#include "pch.h"
#include "LoadTesterConfig.h"

namespace
{
    // "--flag value" 형태에서 다음 토큰을 값으로 소비. 다음 토큰이 없으면 예외.
    String NextValue(int argc, char** argv, int& i, const String& flag)
    {
        if (i + 1 >= argc)
            throw std::runtime_error("LoadTesterConfig: " + flag + " requires a value");
        return String(argv[++i]);
    }
}

LoadTesterConfig LoadTesterConfig::Parse(int argc, char** argv)
{
    LoadTesterConfig config;

    for (int i = 1; i < argc; ++i)
    {
        String arg = argv[i];

        if (arg == "--agents")
            config.agentCount = std::stoi(NextValue(argc, argv, i, arg));
        else if (arg == "--interval-ms")
            config.sendInterval = std::chrono::milliseconds(std::stoll(NextValue(argc, argv, i, arg)));
        else if (arg == "--duration-sec")
            config.duration = std::chrono::seconds(std::stoll(NextValue(argc, argv, i, arg)));
        else if (arg == "--ramp-up-ms")
            config.rampUp = std::chrono::milliseconds(std::stoll(NextValue(argc, argv, i, arg)));
        else if (arg == "--queue-size")
            config.queueSize = static_cast<size_t>(std::stoull(NextValue(argc, argv, i, arg)));
        else if (arg == "--collector-host")
            config.collectorHost = NextValue(argc, argv, i, arg);
        else if (arg == "--collector-port")
            config.collectorPort = static_cast<unsigned short>(std::stoi(NextValue(argc, argv, i, arg)));
        else if (arg == "--csv-out")
            config.csvOutputPath = NextValue(argc, argv, i, arg);
        else if (arg == "--key-file")
            config.keyFilePath = NextValue(argc, argv, i, arg);
        else
            throw std::runtime_error("LoadTesterConfig: unknown argument '" + arg + "'");
    }

    if (config.agentCount <= 0)
        throw std::runtime_error("LoadTesterConfig: --agents must be > 0");

    return config;
}
```

**`LoadTester/LoadTestStats.h`**
```cpp
#pragma once
#include "pch.h"

/*-------------------
    LoadTestStats
---------------------*/
// io_context 이벤트 루프(단일 스레드)에서만 접근된다는 전제 - 별도 스레드가 없으므로
// 락 없이 단순 카운터/vector로 구현.

class LoadTestStats
{
public:
    void OnConnectSuccess();
    void OnConnectFail();
    void OnReconnect();
    void OnQueueDrop();
    void OnPacketSent(std::chrono::milliseconds latency);   // enqueue -> Send 완료까지 지연

    // 마지막 호출 이후의 델타만 계산해 콘솔 한 줄 출력(최근 처리량 파악용) 후 델타 리셋.
    void PrintProgressAndReset(std::chrono::seconds elapsed);

    // 종료 시 누적 통계 + 지연시간 분포(p50/p95/p99)를 CSV로 저장.
    void DumpCsv(const String& path) const;

private:
    static std::chrono::milliseconds Percentile(std::vector<std::chrono::milliseconds> sorted, double p);

private:
    uint64_t _connectSuccessCount = 0;
    uint64_t _connectFailCount = 0;
    uint64_t _reconnectCount = 0;
    uint64_t _queueDropCount = 0;
    uint64_t _sentCount = 0;
    uint64_t _sentSinceLastReport = 0;
    std::vector<std::chrono::milliseconds> _latencySamples;   // 확인 필요 2번 답변대로 무제한 누적 유지(현재 계획 규모 기준)
};
```

**`LoadTester/LoadTestStats.cpp`**
```cpp
#include "pch.h"
#include "LoadTestStats.h"
#include <algorithm>
#include <fstream>

void LoadTestStats::OnConnectSuccess() { ++_connectSuccessCount; }
void LoadTestStats::OnConnectFail() { ++_connectFailCount; }
void LoadTestStats::OnReconnect() { ++_reconnectCount; }
void LoadTestStats::OnQueueDrop() { ++_queueDropCount; }

void LoadTestStats::OnPacketSent(std::chrono::milliseconds latency)
{
    ++_sentCount;
    ++_sentSinceLastReport;
    _latencySamples.push_back(latency);
}

void LoadTestStats::PrintProgressAndReset(std::chrono::seconds elapsed)
{
    std::cout << "[LoadTester] t=" << elapsed.count() << "s"
        << " sent(total)=" << _sentCount
        << " sent(recent)=" << _sentSinceLastReport
        << " connected=" << _connectSuccessCount
        << " reconnects=" << _reconnectCount
        << " connectFails=" << _connectFailCount
        << " queueDrops=" << _queueDropCount
        << std::endl;

    _sentSinceLastReport = 0;
}

std::chrono::milliseconds LoadTestStats::Percentile(std::vector<std::chrono::milliseconds> sorted, double p)
{
    if (sorted.empty())
        return std::chrono::milliseconds(0);

    std::sort(sorted.begin(), sorted.end());
    size_t index = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

void LoadTestStats::DumpCsv(const String& path) const
{
    std::ofstream out(path);
    if (!out)
    {
        std::cerr << "[LoadTestStats] failed to open csv output: " << path << std::endl;
        return;
    }

    std::chrono::milliseconds p50 = Percentile(_latencySamples, 0.50);
    std::chrono::milliseconds p95 = Percentile(_latencySamples, 0.95);
    std::chrono::milliseconds p99 = Percentile(_latencySamples, 0.99);

    out << "metric,value\n";
    out << "sent_total," << _sentCount << "\n";
    out << "connect_success," << _connectSuccessCount << "\n";
    out << "connect_fail," << _connectFailCount << "\n";
    out << "reconnect," << _reconnectCount << "\n";
    out << "queue_drop," << _queueDropCount << "\n";
    out << "latency_p50_ms," << p50.count() << "\n";
    out << "latency_p95_ms," << p95.count() << "\n";
    out << "latency_p99_ms," << p99.count() << "\n";

    std::cout << "[LoadTester] result written to " << path << std::endl;
}
```

**`LoadTester/SimulatedAgent.h`**
```cpp
#pragma once
#include "pch.h"
#include "ResilientSender.h"
#include "AesGcmCipher.h"
#include "LoadTesterConfig.h"
#include "LoadTestStats.h"

namespace apm { class Metric; }

/*-------------------
    SimulatedAgent
---------------------*/
// 실제 Agent 한 대에 대응하는 시뮬레이션 단위. ResilientSender를 그대로 재사용해서
// Collector 입장에서는 진짜 Agent와 구분 불가능한 트래픽을 만든다.
// MetricScheduler(ResourceCollector 의존)는 재사용하지 않음 - 부하 테스트는 실측 리소스가
// 아니라 고정/랜덤값으로 충분(확인 필요 3번 답변대로 확정), 불필요한 결합을 만들지 않기 위함.

class SimulatedAgent
{
public:
    SimulatedAgent(asio::io_context& ioContext, asio::ssl::context& sslContext,
        const LoadTesterConfig& config, const AesGcmCipher::Key& agentCollectorKey,
        LoadTestStats& stats, int agentIndex);

    void Start();
    void Stop();

private:
    void ScheduleNextSend();
    apm::Metric BuildFakeMetric() const;

private:
    asio::io_context& _ioContext;
    const LoadTesterConfig& _config;
    LoadTestStats& _stats;
    int _agentIndex;
    ResilientSender _sender;
    asio::steady_timer _sendTimer;
    bool _running = false;
    bool _everConnected = false;   // ConnectionStateCallback에서 최초 연결/재연결 구분용
};
```

**`LoadTester/SimulatedAgent.cpp`**
```cpp
#include "pch.h"
#include "SimulatedAgent.h"
#include "AesGcmPayload.h"
#include "Protocol/Metric.pb.h"
#include <random>

namespace
{
    std::mt19937& RandomEngine()
    {
        static thread_local std::mt19937 engine{ std::random_device{}() };
        return engine;
    }

    double RandomPercent()
    {
        std::uniform_real_distribution<double> dist(0.0, 100.0);
        return dist(RandomEngine());
    }

    uint64 RandomBytes(uint64 maxValue)
    {
        std::uniform_int_distribution<uint64> dist(0, maxValue);
        return dist(RandomEngine());
    }
}

SimulatedAgent::SimulatedAgent(asio::io_context& ioContext, asio::ssl::context& sslContext,
    const LoadTesterConfig& config, const AesGcmCipher::Key& agentCollectorKey,
    LoadTestStats& stats, int agentIndex)
    : _ioContext(ioContext)
    , _config(config)
    , _stats(stats)
    , _agentIndex(agentIndex)
    , _sender(ioContext, sslContext, config.collectorHost, config.collectorPort,
        [agentCollectorKey]() { return std::make_unique<AesGcmPayload>(agentCollectorKey); },
        config.queueSize,
        [this](bool connected)
        {
            if (connected)
            {
                if (_everConnected)
                    _stats.OnReconnect();
                else
                    _stats.OnConnectSuccess();
                _everConnected = true;
            }
            else if (!_everConnected)
            {
                _stats.OnConnectFail();
            }
        })
    , _sendTimer(ioContext)
{
}

void SimulatedAgent::Start()
{
    _running = true;
    ScheduleNextSend();
}

void SimulatedAgent::Stop()
{
    _running = false;
    _sendTimer.cancel();
}

void SimulatedAgent::ScheduleNextSend()
{
    if (!_running)
        return;

    _sendTimer.expires_after(_config.sendInterval);
    _sendTimer.async_wait(
        [this](const asio::error_code& ec)
        {
            if (ec || !_running)
                return;

            auto sentAt = std::chrono::steady_clock::now();
            apm::Metric metric = BuildFakeMetric();

            _sender.Enqueue(metric,
                [this, sentAt](bool success)
                {
                    if (success)
                    {
                        auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - sentAt);
                        _stats.OnPacketSent(latency);
                    }
                    else
                    {
                        _stats.OnQueueDrop();
                    }
                });

            ScheduleNextSend();
        });
}

apm::Metric SimulatedAgent::BuildFakeMetric() const
{
    apm::Metric pkt;
    pkt.set_cpu_usage_percent(RandomPercent());
    pkt.set_mem_used_bytes(RandomBytes(8ULL * 1024 * 1024 * 1024));
    pkt.set_mem_total_bytes(16ULL * 1024 * 1024 * 1024);
    pkt.set_disk_used_bytes(RandomBytes(100ULL * 1024 * 1024 * 1024));
    pkt.set_disk_total_bytes(500ULL * 1024 * 1024 * 1024);
    pkt.set_net_rx_bytes_per_sec(RandomBytes(1024ULL * 1024));
    pkt.set_net_tx_bytes_per_sec(RandomBytes(1024ULL * 1024));
    pkt.set_tcp_rtt_us(static_cast<uint32>(RandomBytes(50000)));
    pkt.set_tcp_rtt_var_us(static_cast<uint32>(RandomBytes(5000)));
    pkt.set_tcp_retransmits(0);
    pkt.set_tcp_total_retrans(0);
    pkt.set_tcp_snd_cwnd(static_cast<uint32>(RandomBytes(100)));
    return pkt;
}
```

**`LoadTester/LoadTester.h`**
```cpp
#pragma once
#include "pch.h"
#include "LoadTesterConfig.h"
#include "LoadTestStats.h"
#include "SimulatedAgent.h"
#include "AesGcmCipher.h"

/*-------------------
    LoadTester
---------------------*/
// 오케스트레이터. io_context 하나에 N개의 SimulatedAgent를 램프업 스케줄에 맞춰 생성하고,
// 지정된 duration 동안 실행한 뒤 정리하고 CSV를 남긴다.

class LoadTester
{
public:
    explicit LoadTester(LoadTesterConfig config);

    // 블로킹 - ioContext.run()을 내부에서 호출하고 duration 경과 시 스스로 정지.
    void Run();

private:
    void SpawnAgents();          // ramp-up 스케줄에 맞춰 SimulatedAgent 생성(동시 또는 스태거링)
    void ScheduleProgressReport();
    void ScheduleShutdown();
    void Shutdown();

private:
    LoadTesterConfig _config;
    asio::io_context _ioContext;
    asio::ssl::context _sslContext;
    AesGcmCipher::Key _agentCollectorKey;
    LoadTestStats _stats;
    std::vector<std::unique_ptr<SimulatedAgent>> _agents;
    asio::steady_timer _spawnTimer;
    asio::steady_timer _reportTimer;
    asio::steady_timer _shutdownTimer;
    std::chrono::steady_clock::time_point _startTime;
};
```

**`LoadTester/LoadTester.cpp`**
```cpp
#include "pch.h"
#include "LoadTester.h"
#include "KeyLoader.h"

LoadTester::LoadTester(LoadTesterConfig config)
    : _config(std::move(config))
    , _sslContext(asio::ssl::context::tls_client)
    , _agentCollectorKey(LoadKeyFromHexFile(_config.keyFilePath))
    , _spawnTimer(_ioContext)
    , _reportTimer(_ioContext)
    , _shutdownTimer(_ioContext)
{
    // 테스트용 자체 서명 인증서라 CA 검증 생략 - 실제 Agent(Agent/main.cpp)와 동일한 이유.
    _sslContext.set_verify_mode(asio::ssl::verify_none);
    _agents.reserve(_config.agentCount);
}

void LoadTester::Run()
{
    _startTime = std::chrono::steady_clock::now();

    std::cout << "[LoadTester] starting: agents=" << _config.agentCount
        << " interval-ms=" << _config.sendInterval.count()
        << " duration-sec=" << _config.duration.count()
        << " ramp-up-ms=" << _config.rampUp.count()
        << " collector=" << _config.collectorHost << ":" << _config.collectorPort
        << std::endl;

    SpawnAgents();
    ScheduleProgressReport();
    ScheduleShutdown();

    _ioContext.run();

    _stats.DumpCsv(_config.csvOutputPath);
}

void LoadTester::SpawnAgents()
{
    // rampUp==0: 전체 동시 connect(thundering herd 테스트) - 한 번에 다 생성.
    if (_config.rampUp.count() == 0)
    {
        for (int i = 0; i < _config.agentCount; ++i)
        {
            _agents.push_back(std::make_unique<SimulatedAgent>(
                _ioContext, _sslContext, _config, _agentCollectorKey, _stats, i));
            _agents.back()->Start();
        }
        return;
    }

    // rampUp>0: agentCount개를 rampUp 기간에 걸쳐 균등 간격으로 스태거링 connect.
    auto staggerInterval = _config.rampUp / _config.agentCount;

    auto spawnNext = std::make_shared<std::function<void(int)>>();
    *spawnNext = [this, staggerInterval, spawnNext](int index)
    {
        if (index >= _config.agentCount)
            return;

        _agents.push_back(std::make_unique<SimulatedAgent>(
            _ioContext, _sslContext, _config, _agentCollectorKey, _stats, index));
        _agents.back()->Start();

        _spawnTimer.expires_after(staggerInterval);
        _spawnTimer.async_wait(
            [this, spawnNext, index](const asio::error_code& ec)
            {
                if (!ec)
                    (*spawnNext)(index + 1);
            });
    };

    (*spawnNext)(0);
}

void LoadTester::ScheduleProgressReport()
{
    _reportTimer.expires_after(std::chrono::seconds(1));
    _reportTimer.async_wait(
        [this](const asio::error_code& ec)
        {
            if (ec)
                return;

            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - _startTime);
            _stats.PrintProgressAndReset(elapsed);
            ScheduleProgressReport();
        });
}

void LoadTester::ScheduleShutdown()
{
    _shutdownTimer.expires_after(_config.duration);
    _shutdownTimer.async_wait(
        [this](const asio::error_code& ec)
        {
            if (!ec)
                Shutdown();
        });
}

void LoadTester::Shutdown()
{
    std::cout << "[LoadTester] duration elapsed, shutting down" << std::endl;

    _spawnTimer.cancel();
    _reportTimer.cancel();

    for (auto& agent : _agents)
        agent->Stop();

    // 마지막 진행 상황 한 번 더 출력하고 io_context를 정지 - ResilientSender의
    // 재연결 타이머 등 아직 남아있는 비동기 작업이 있어도 run()이 즉시 반환하게 함.
    // (소켓을 정중히 닫지는 않음 - 프로세스가 곧 종료되는 부하 테스트 도구라 범위 밖으로 판단.
    // 필요해지면 SimulatedAgent/ResilientSender에 Close() 계열 메서드 추가 검토.)
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - _startTime);
    _stats.PrintProgressAndReset(elapsed);

    _ioContext.stop();
}
```

**`LoadTester/main.cpp`**
```cpp
#include "pch.h"
#include "LoadTesterConfig.h"
#include "LoadTester.h"

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        LoadTesterConfig config = LoadTesterConfig::Parse(argc, argv);
        LoadTester tester(std::move(config));
        tester.Run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[LoadTester] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

### 제안 3 — `APM_Agent/CMakeLists.txt` 수정 (추가분, 기존 내용 변경 없음)

**변경 사유**: `Collector`/`Agent`와 같은 패턴으로 `LoadTester` 실행 파일 타겟 추가.

`add_executable(Agent ...)` 블록(현재 40번째 줄 부근) 바로 뒤에 아래를 추가:

```cmake
add_executable(LoadTester
    LoadTester/main.cpp
    LoadTester/LoadTesterConfig.cpp
    LoadTester/LoadTestStats.cpp
    LoadTester/SimulatedAgent.cpp
    LoadTester/LoadTester.cpp
    pch.cpp
)
target_link_libraries(LoadTester PRIVATE APM_Common)
```

### 미해결 사항 (직접 적용 시 참고)

- `LoadTester::Shutdown()`은 소켓을 정중히 닫지 않고 `_ioContext.stop()`으로 즉시 정지시킨다. 프로세스가 곧 종료되는 일회성 도구라 범위 밖으로 판단했는데, Collector 쪽 로그에 비정상 종료로 잡히는 게 거슬리면 `SimulatedAgent`/`ResilientSender`에 `Close()`를 추가하는 걸 별도로 검토할 것.
- CMake의 `add_executable`은 새 `.cpp` 파일이 실제로 존재해야 한다 — 위 6개 파일(`LoadTesterConfig.h/.cpp`, `LoadTestStats.h/.cpp`, `SimulatedAgent.h/.cpp`, `LoadTester.h/.cpp`, `main.cpp`)을 먼저 `APM_Agent/LoadTester/`에 만든 뒤 `CMakeLists.txt`를 갱신해야 빌드된다.
- 이 환경(Windows MSYS bash)에는 `cmake`/`g++`/`protoc`가 없어 이 제안서 자체를 빌드 검증하지 못했다 — 적용 후 WSL 또는 기존 빌드 환경에서 직접 빌드 확인 필요.

### 결정 사항

- 코드 직접 수정 없이 이 로그에 적용 가능한 완성 전문만 남김(사용자가 직접 적용하기로 함, 2026-07-26).
- 이후 사용자가 "그냥 바로 적용해줘"로 명시 지시(같은 날) — 위 제안 그대로 `ResilientSender.h/.cpp` 수정 + `LoadTester/` 6개 파일 신규 작성 + `CMakeLists.txt` 반영을 실제로 적용함. `Agent/main.cpp`는 콜백 파라미터 기본값이 `nullptr`이라 수정 불필요(그대로 둠).
- 이 환경엔 cmake/g++/protoc 툴체인이 없어 빌드 미검증 상태였으나, 사용자가 WSL(Ubuntu, GCC 13.3.0, Protobuf 3.21.12, OpenSSL 3.0.13, SQLite3 3.45.1)에서 직접 `cmake -B build -S . -DAPM_STORAGE_BACKEND=SQLite` → `cmake --build build -j$(nproc) --target LoadTester` 실행, **빌드 성공 확인**(2026-07-26). `GW2_CrossPlatformCore`의 기존 `ASIO_STANDALONE` 재정의 경고만 있고 이번 변경분(`ResilientSender`, `LoadTester/`)에서 발생한 경고/에러는 없음.
- 1-4 완료. 1-5(N-agent 스케일 실측)로 이어감.

---

## 2026-07-26 — 1-5(N-agent 스케일 실측) 방법론 + 실행 스크립트 제안

### 배경

1-4 빌드 검증 통과 확인 후 바로 이어서 착수. 기존 단일 Agent 기준 실측(README `7-2`/`7-3`절: `strace -c`로 Agent를 5분간 감싸 syscall 프로파일, `/proc/[pid]/status`·`/proc/[pid]/stat`을 15초 간격 20회 샘플링해 RSS/CPU)이 **커밋된 스크립트 없이 수작업으로 진행됐음**을 확인(`APM_Agent/run_collector.sh`/`run_agent.sh`는 단순 실행 파일 찾기 래퍼일 뿐, strace/샘플링 로직 없음). 이번엔 N-agent 스케일로 반복 측정해야 하니 스크립트로 자동화하는 게 맞다고 판단.

### 측정 대상 변경 — 이번엔 Collector를 감싼다

단일 Agent 실측(README 7절)은 **Agent** 프로세스를 대상으로 했다(Agent가 가벼운지 증명). 이번 1-5는 반대로 **Collector**가 접속 수가 늘어도 버티는지가 목적이므로(1순위 착수 당시 첫 로그의 핵심 가설 - "epoll 이벤트 루프 하나가 파싱/복호화/SQLite 저장까지 전부 처리") `strace -c`/RSS·CPU 샘플링 대상을 Collector로 바꿔서 같은 방법론을 재사용한다.

### 제안 — `APM_Agent/run_load_test.sh` (전체 파일, 적용 가능한 완성본)

`run_collector.sh`/`run_agent.sh`와 동일한 스타일(`set -euo pipefail`, `cd "$(dirname "$0")"`로 호출 위치 무관하게 동작)을 따름. `strace` 기반이라 **Linux/WSL 전용**(Windows 측정은 범위 밖 - 기존 단일 Agent 실측도 Linux 기준이었음).

```bash
#!/usr/bin/env bash
# APM_Agent/Collector를 strace -c + RSS/CPU 샘플링으로 감싼 채 LoadTester로 부하를 걸고,
# 결과를 loadtest_results/ 아래에 남긴다. Linux(strace) 전용 - Windows에서는 미지원.
# 어느 위치에서 호출해도 스크립트 자신의 디렉토리(APM_Agent/) 기준으로 동작.
set -euo pipefail
cd "$(dirname "$0")"

AGENTS="${1:?사용법: run_load_test.sh <agent_count> [duration_sec=60] [interval_ms=100] [ramp_up_ms=0]}"
DURATION="${2:-60}"
INTERVAL_MS="${3:-100}"
RAMP_UP_MS="${4:-0}"

if [ ! -x build/Collector ] || [ ! -x build/LoadTester ]; then
    echo "[run_load_test] build/Collector 또는 build/LoadTester가 없음 - 먼저 빌드하세요:" >&2
    echo "  cmake --build build -j\$(nproc)" >&2
    exit 1
fi

RESULT_DIR="loadtest_results/agents_${AGENTS}_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULT_DIR"
echo "[run_load_test] agents=$AGENTS duration=${DURATION}s interval=${INTERVAL_MS}ms ramp-up=${RAMP_UP_MS}ms"
echo "[run_load_test] results -> $RESULT_DIR"

rm -f apm_metrics.db   # 이전 실행의 누적 데이터와 섞이지 않게 Collector를 매번 새 DB로 시작

# 1) Collector를 strace -c로 감싸서 백그라운드 실행(syscall 프로파일, README 7-2절과 동일 방식).
strace -c -o "$RESULT_DIR/collector_strace.txt" ./build/Collector > "$RESULT_DIR/collector_stdout.log" 2>&1 &
STRACE_PID=$!

sleep 1
# strace는 자식 프로세스를 새로 fork+exec하므로 $STRACE_PID는 strace 자신의 pid다 -
# 실제 Collector(트레이싱 대상)의 pid는 그 직속 자식에서 찾는다(pgrep -f로 커맨드라인
# 문자열을 매칭하면 strace 자신의 인자에도 "Collector"가 들어있어 오탐할 수 있어 피함).
COLLECTOR_PID=$(pgrep -P "$STRACE_PID" | head -n1)
if [ -z "$COLLECTOR_PID" ]; then
    echo "[run_load_test] Collector pid를 찾지 못함 - 종료" >&2
    kill "$STRACE_PID" 2>/dev/null || true
    exit 1
fi
echo "[run_load_test] Collector pid=$COLLECTOR_PID (strace pid=$STRACE_PID)"

# 2) RSS/CPU를 1초 간격으로 백그라운드 샘플링(README 7-3절과 동일 방식 - /proc/[pid]/status,
#    /proc/[pid]/stat 기준. 원본은 15초 간격/5분이었지만 부하 테스트는 훨씬 짧게 도니 1초로 촘촘히).
SAMPLE_CSV="$RESULT_DIR/collector_resource_samples.csv"
echo "elapsed_sec,rss_kb,cpu_ticks_total" > "$SAMPLE_CSV"
(
    START=$(date +%s)
    while kill -0 "$COLLECTOR_PID" 2>/dev/null; do
        ELAPSED=$(( $(date +%s) - START ))
        RSS=$(awk '/VmRSS/ {print $2}' "/proc/$COLLECTOR_PID/status" 2>/dev/null || echo 0)
        read -r UTIME STIME < <(awk '{print $14, $15}' "/proc/$COLLECTOR_PID/stat" 2>/dev/null || echo "0 0")
        echo "${ELAPSED},${RSS:-0},$(( UTIME + STIME ))" >> "$SAMPLE_CSV"
        sleep 1
    done
) &
SAMPLER_PID=$!

# 3) Collector가 accept 가능해질 시간을 준 뒤 LoadTester 실행 - 이게 실제 부하 발생원.
sleep 1
./build/LoadTester \
    --agents "$AGENTS" \
    --interval-ms "$INTERVAL_MS" \
    --duration-sec "$DURATION" \
    --ramp-up-ms "$RAMP_UP_MS" \
    --csv-out "$RESULT_DIR/loadtester_result.csv" \
    2>&1 | tee "$RESULT_DIR/loadtester_stdout.log"

# 4) LoadTester가 끝나면 Collector를 정지 -> strace -c 요약이 이 시점에 파일로 flush됨.
kill -TERM "$COLLECTOR_PID" 2>/dev/null || true
wait "$STRACE_PID" 2>/dev/null || true
wait "$SAMPLER_PID" 2>/dev/null || true

echo "[run_load_test] 완료. 결과: $RESULT_DIR/{collector_strace.txt, collector_resource_samples.csv, loadtester_result.csv}"
```

**사용 전 준비**: `cmake --build build -j$(nproc)`(Collector/LoadTester 둘 다 최신인지 확인) + `certs/`가 이미 생성돼 있어야 함(HOW_TO_RUN.md 2절, 이미 완료된 상태로 보임) + `strace`(`sudo apt install strace`, 대개 기본 설치됨).

### 실행 계획(테스트 매트릭스) — 제안

| 실행 | agents | duration-sec | interval-ms | ramp-up-ms | 목적 |
|---|---|---|---|---|---|
| 1 | 1 | 60 | 100 | 0 | LoadTester 자체 검증(단일 에이전트로 정상 동작하는지, 기존 Agent 실측과 비교 기준점) |
| 2 | 10 | 60 | 100 | 0 | 저부하 |
| 3 | 50 | 60 | 100 | 0 | 중부하 |
| 4 | 100 | 60 | 100 | 0 | 고부하(thundering herd - 동시 접속) |
| 5 | 100 | 60 | 100 | 5000 | 4와 동일 규모, 스태거링 접속 - thundering herd 유무 차이 비교 |
| 6 | 300~500 (환경이 버티는 선까지) | 60 | 100 | 5000 | 한계점 탐색 |

`interval-ms 100`은 README 설계 노트(1차 로그)의 "실제 Agent 기본 5000ms보다 훨씬 짧게 - 스트레스를 걸려면" 방침 그대로.

### 각 결과 파일에서 볼 것

- `collector_strace.txt`: `epoll_wait`/`read`/`accept`/`openat`/`close` 등 호출 횟수가 에이전트 수에 **비례**하는지, 아니면 어느 지점부터 초선형으로 튀는지(→ 병목 신호).
- `collector_resource_samples.csv`: RSS가 에이전트 수에 따라 늘어나는 폭으로 "세션 하나당 고정 메모리 오버헤드"를 역산 가능. `cpu_ticks_total`의 tier 간 증가율로 CPU 부담 확인.
- `loadtester_result.csv`: `connect_fail`/`queue_drop`이 0을 벗어나기 시작하는 지점 = Collector가 못 버티기 시작하는 임계점. `latency_p95_ms`/`latency_p99_ms`가 튀는 지점도 같이 봄.

### 결정 사항

- 사용자가 "스크립트 만들어서 진행"을 명시적으로 선택(2026-07-26) — `APM_Agent/run_load_test.sh` 실제 생성 완료. 실행 권한(`chmod +x`)은 아직 안 걸려있을 수 있음 - WSL에서 `bash run_load_test.sh ...`로 실행하거나 `chmod +x run_load_test.sh` 먼저 실행.
- 다음 단계: 위 6단계 매트릭스대로 사용자가 WSL에서 순차 실행 → 결과(`loadtest_results/agents_*/` 안의 `collector_strace.txt`/`collector_resource_samples.csv`/`loadtester_result.csv`) 공유 → 분석 후 1-6(README/PROJECT_TECHNICAL_REVIEW 반영)으로 이어감.

### 버그 발견 — 실행 6회 전부 수초 만에 조기 종료 (2026-07-26)

사용자가 매트릭스 6개(`agents=1,10,50,100,100(ramp-up 5s),300`)를 연달아 실행했는데, 전부 60초는커녕 3~9초 만에 다음 명령 프롬프트로 돌아옴 — `Collector pid=`/`완료` 로그가 전혀 안 찍힘.

**원인**: `COLLECTOR_PID=$(pgrep -P "$STRACE_PID" | head -n1)` 줄. `set -euo pipefail` 상태에서 `pgrep`이 자식을 못 찾으면(exit 1) `pipefail`이 파이프라인 전체를 실패로 처리하고, `set -e`가 그 즉시 스크립트를 종료시킨다 — 바로 다음 줄의 `if [ -z "$COLLECTOR_PID" ]` 진단 메시지조차 못 찍고 죽어서 원인이 안 보였다. `pgrep`이 자식을 못 찾는 실제 이유(Collector가 그 사이 죽음)는 아직 미확인 — 흔한 후보는 이전 실행이 이 버그 때문에 정리(`kill -TERM`) 없이 죽으면서 백그라운드에 Collector가 남아 9000번 포트를 계속 잡고 있어, 다음 실행의 Collector가 `bind` 실패로 즉시 죽는 연쇄. 사용자에게 `collector_stdout.log` 내용과 `pgrep -af Collector` 결과 확인 요청함(확인 후 이 항목에 원인 추가 예정).

**수정 전 → 수정 후** (`run_load_test.sh`, `set -e`/`pipefail`에 안 죽게 방어 + 진단 메시지 보강):

수정 전:
```bash
sleep 1
# strace는 자식 프로세스를 새로 fork+exec하므로 $STRACE_PID는 strace 자신의 pid다 -
# 실제 Collector(트레이싱 대상)의 pid는 그 직속 자식에서 찾는다(pgrep -f로 커맨드라인
# 문자열을 매칭하면 strace 자신의 인자에도 "Collector"가 들어있어 오탐할 수 있어 피함).
COLLECTOR_PID=$(pgrep -P "$STRACE_PID" | head -n1)
if [ -z "$COLLECTOR_PID" ]; then
    echo "[run_load_test] Collector pid를 찾지 못함 - 종료" >&2
    kill "$STRACE_PID" 2>/dev/null || true
    exit 1
fi
echo "[run_load_test] Collector pid=$COLLECTOR_PID (strace pid=$STRACE_PID)"
```

수정 후(실제 적용 완료):
```bash
sleep 1
# strace는 자식 프로세스를 새로 fork+exec하므로 $STRACE_PID는 strace 자신의 pid다 -
# 실제 Collector(트레이싱 대상)의 pid는 그 직속 자식에서 찾는다(pgrep -f로 커맨드라인
# 문자열을 매칭하면 strace 자신의 인자에도 "Collector"가 들어있어 오탐할 수 있어 피함).
# "|| true"로 pgrep 실패(자식을 못 찾음)를 무시 - set -e/pipefail 상태에서 그대로 두면
# 아래 진단 메시지도 못 찍고 스크립트가 조용히 죽어버림(실제로 이렇게 죽는 걸 확인함, 2026-07-26).
COLLECTOR_PID=$(pgrep -P "$STRACE_PID" | head -n1 || true)
if [ -z "$COLLECTOR_PID" ]; then
    echo "[run_load_test] Collector pid를 찾지 못함 - Collector가 바로 죽었을 가능성이 큼." >&2
    echo "[run_load_test] 로그 확인: $RESULT_DIR/collector_stdout.log" >&2
    echo "[run_load_test] (흔한 원인: 9000번 포트를 이미 다른 Collector가 쓰고 있음 - 'pgrep -af Collector'로 확인)" >&2
    cat "$RESULT_DIR/collector_stdout.log" >&2 2>/dev/null || true
    kill "$STRACE_PID" 2>/dev/null || true
    exit 1
fi
echo "[run_load_test] Collector pid=$COLLECTOR_PID (strace pid=$STRACE_PID)"
```

**결정 사항**: 위 수정은 이미 `run_load_test.sh`에 직접 적용함(같은 세션 흐름상 재확인 없이 바로 반영 — 진단 메시지를 보이게 하는 방어 코드라 리스크 낮다고 판단).

**근본 원인 확정(2026-07-26)**: 사용자가 `collector_stdout.log`/`pgrep -af Collector` 확인 — `strace: command not found`. 포트 충돌이 아니라 **`strace` 자체가 이 WSL 환경에 설치돼 있지 않아서** `Collector`가 애초에 exec조차 안 됐던 것(스크립트의 "사용 전 준비" 절에 `sudo apt install strace` 필요하다고 이미 적어뒀지만 실제로 설치가 안 된 상태였음). `pgrep -af Collector`가 빈 결과인 것도 이와 일치(떠 있는 Collector가 아예 없었음 - 포트 점유 문제 아님).

**다음 할 일**: 사용자가 `sudo apt install strace` 실행 후 6단계 매트릭스 재실행.

---

## 2026-07-26 — 2순위(알림) 설계 제안

### 배경

1순위(1-5) 실측은 사용자 판단으로 보류(위 항목 참고), 2순위(원격 명령 실행 제외, 알림만)로 전환. 착수 전 `APM_Console` 쪽 구조(`ApmModule`, `MetricsReceiverService`, `MetricsHub`, `ApmDbContext`, `MetricRecord`, `DashboardController`, `Dashboard/Index.cshtml`, 테스트 컨벤션 `ReadExactAsyncTests`/`DecryptAndParseTests`, `site.css`)를 다시 읽고 확인.

**확인 필요 4건 → 사용자 결정**:
1. 임계치 대상 지표: CPU/메모리/디스크 사용률(%) + **TCP RTT까지 포함**(4개)
2. 임계치 저장 위치: **DB에 저장 + UI로 편집**(appsettings.json 정적값 아님)
3. 알림 트리거 시점: **상태 전이 시만**(OK→Alert 최초 1회, Alert→Resolved 복구 1회) — Zabbix/Nagios/Alertmanager와 동일한 업계 표준 방식
4. 알림 영속화: **DB에 `AlertRecord` 테이블로 저장**(이력 조회 가능)

### 설계

**아키텍처**: `MetricsReceiverService.StoreAndBroadcastAsync()`가 메트릭을 저장하고 `"NewMetric"`을 브로드캐스트하는 지점 바로 뒤에 알림 평가를 추가. 상태 전이 판단(순수 로직)은 `AlertEvaluator`(internal static, `DecryptAndParse`와 동일한 패턴 — 테스트 대상)로 분리하고, DB I/O(현재 열린 알림 조회/기록)는 서비스가 담당.

- `AlertThreshold`(DB, 4개 행 시드): 지표별 임계치 값 + 활성화 여부. `/apm/alerts`에서 편집.
- `AlertRecord`(DB): 알림 "사건" 하나당 한 행. `ClosedAt == null`이면 현재 진행 중. 임계치 값은 발생 시점 스냅샷으로 저장(나중에 임계치를 바꿔도 과거 이력은 안 바뀜).
- 상태(현재 열려 있는지)는 메모리가 아니라 **매번 DB 조회로 판단**(`AlertRecords`에서 `ClosedAt == null`인 최신 행 존재 여부) — Console 프로세스가 재시작돼도 상태가 유실되지 않음(메모리 캐시였다면 재시작 시 진행 중이던 알림을 "새로 시작"으로 오인했을 것).
- SignalR: 기존 `MetricsHub`(`/apm/hub/metrics`)를 그대로 재사용해 `"AlertOpened"`/`"AlertResolved"` 이벤트 추가.
- 새 페이지 `/apm/alerts`: 임계치 편집 폼 + 현재 활성 알림 목록(실시간) + 최근 이력 20건. 기존 대시보드처럼 공유 레이아웃이 없는 구조(`_Layout.cshtml` 없음)라 페이지 상단에 서로 링크만 추가.

**TCP RTT 기본 임계치**: 200,000us(200ms) — 로컬 테스트 환경에서는 사실상 안 울리는 보수적인 기본값(운영 환경 값이 아니라 "동작 확인용" 시드). CPU/메모리/디스크는 90%.

### 제안 — 전체 파일

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Infrastructure/Persistence/AlertThreshold.cs`** (신규)
```csharp
namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

// 알림을 걸 지표 종류. int로 DB에 저장됨(EF Core enum 기본 매핑) - 값 순서를 바꾸면
// 기존 DB의 저장된 정수와 어긋나므로, 항목을 추가할 땐 반드시 끝에만 추가할 것.
public enum AlertMetricType
{
    CpuPercent,
    MemoryPercent,
    DiskPercent,
    TcpRttUs,
}

// 지표별 임계치 설정 - /apm/alerts에서 편집 가능(DB에 저장, appsettings.json 아님 -
// 재시작 없이 바꿀 수 있어야 한다는 요구사항 때문, 2026-07-26 결정).
public class AlertThreshold
{
    public int Id { get; set; }
    public AlertMetricType MetricType { get; set; }
    public double Value { get; set; }
    public bool Enabled { get; set; } = true;
}
```

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Infrastructure/Persistence/AlertRecord.cs`** (신규)
```csharp
namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

// 알림 "사건"(episode) 하나당 한 행. ClosedAt이 null이면 현재도 진행 중인 알림 -
// 상태 전이 판단(AlertEvaluator)이 "이 지표에 열린 행이 있는가"를 매번 DB에서 조회해서 판단하므로
// 이 테이블 자체가 곧 상태 저장소다(별도 메모리 캐시 없음, 2026-07-26 결정 - 재시작 안전성).
public class AlertRecord
{
    public int Id { get; set; }
    public AlertMetricType MetricType { get; set; }
    public double ThresholdValue { get; set; }   // 발생 시점의 임계치 스냅샷 - 나중에 임계치가 바뀌어도 이력은 그대로 보존
    public double TriggerValue { get; set; }     // 임계치를 넘은 순간의 측정값
    public DateTimeOffset OpenedAt { get; set; }
    public double? ResolvedValue { get; set; }    // 복구 시점의 측정값 - 아직 진행 중이면 null
    public DateTimeOffset? ClosedAt { get; set; } // null이면 현재도 진행 중
}
```

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Infrastructure/AlertEvaluator.cs`** (신규)
```csharp
namespace ApmConsole.Domain.Apm.Infrastructure;

public enum AlertTransition
{
    None,
    Opened,
    Resolved,
}

/*----------------
    AlertEvaluator
------------------*/
// 순수 로직만 담당(DB I/O 없음) - MetricsReceiverService.DecryptAndParse와 같은 이유로
// internal이 아니라 그냥 public으로 둠(다른 internal 순수 로직과 달리 컨트롤러/뷰 쪽에서도
// AlertTransition을 참조할 여지가 있어 접근 제한을 걸 이유가 약함). 테스트 대상.
public static class AlertEvaluator
{
    // currentlyOpen: 이 지표에 대해 현재 열려 있는(ClosedAt == null) AlertRecord가 있는지.
    // 상태 전이 시에만 알리기 위한 판단 - 매 수치마다 반복 알림을 피함
    // (Zabbix/Nagios/Alertmanager와 동일한 방식, 2026-07-26 결정).
    public static AlertTransition Evaluate(double currentValue, double threshold, bool currentlyOpen)
    {
        bool isBreaching = currentValue >= threshold;

        if (isBreaching && !currentlyOpen)
            return AlertTransition.Opened;

        if (!isBreaching && currentlyOpen)
            return AlertTransition.Resolved;

        return AlertTransition.None;
    }
}
```

**`APM_Console/tests/ApmConsole.Domain.Apm.Tests/Infrastructure/AlertEvaluatorTests.cs`** (신규, 기존 테스트 컨벤션 그대로 따름)
```csharp
using ApmConsole.Domain.Apm.Infrastructure;

namespace ApmConsole.Domain.Apm.Tests.Infrastructure;

public class AlertEvaluatorTests
{
    [Fact]
    public void 임계치를_처음_넘으면_Opened를_반환한다()
    {
        var result = AlertEvaluator.Evaluate(currentValue: 95, threshold: 90, currentlyOpen: false);

        Assert.Equal(AlertTransition.Opened, result);
    }

    [Fact]
    public void 이미_열려있는_상태에서_계속_넘으면_None을_반환한다()
    {
        // 스팸 방지 핵심 - 매 수치마다 다시 Opened가 나오면 안 됨.
        var result = AlertEvaluator.Evaluate(currentValue: 95, threshold: 90, currentlyOpen: true);

        Assert.Equal(AlertTransition.None, result);
    }

    [Fact]
    public void 열려있던_상태에서_임계치_아래로_내려가면_Resolved를_반환한다()
    {
        var result = AlertEvaluator.Evaluate(currentValue: 80, threshold: 90, currentlyOpen: true);

        Assert.Equal(AlertTransition.Resolved, result);
    }

    [Fact]
    public void 원래도_정상이었고_계속_정상이면_None을_반환한다()
    {
        var result = AlertEvaluator.Evaluate(currentValue: 50, threshold: 90, currentlyOpen: false);

        Assert.Equal(AlertTransition.None, result);
    }

    [Fact]
    public void 임계치와_정확히_같으면_초과로_간주한다()
    {
        // ">=" 채택 - 경계값이 "아직 안전"으로 새는 쪽보다 "이미 위험"으로 잡는 쪽이 알림 목적에 맞음.
        var result = AlertEvaluator.Evaluate(currentValue: 90, threshold: 90, currentlyOpen: false);

        Assert.Equal(AlertTransition.Opened, result);
    }
}
```

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Models/AlertsViewModel.cs`** (신규)
```csharp
using ApmConsole.Domain.Apm.Infrastructure.Persistence;

namespace ApmConsole.Domain.Apm.Models;

public record AlertsViewModel(
    List<AlertThreshold> Thresholds,
    List<AlertRecord> ActiveAlerts,
    List<AlertRecord> RecentHistory);
```

### 제안 — 기존 파일 수정 (수정 전 / 수정 후)

**변경 사유**: 메트릭 저장 직후 알림 상태 전이를 평가해서, 전이가 있을 때만 DB에 기록 + SignalR로 브로드캐스트.

**`ApmDbContext.cs`**

수정 전(현재 디스크 상태):
```csharp
using Microsoft.EntityFrameworkCore;

namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

public class ApmDbContext : DbContext
{
    public ApmDbContext(DbContextOptions<ApmDbContext> options) : base(options) { }

    public DbSet<MetricRecord> Metrics => Set<MetricRecord>();
}
```

수정 후(전체 교체):
```csharp
using Microsoft.EntityFrameworkCore;

namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

public class ApmDbContext : DbContext
{
    public ApmDbContext(DbContextOptions<ApmDbContext> options) : base(options) { }

    public DbSet<MetricRecord> Metrics => Set<MetricRecord>();
    public DbSet<AlertThreshold> AlertThresholds => Set<AlertThreshold>();
    public DbSet<AlertRecord> AlertRecords => Set<AlertRecord>();

    // EnsureCreated()가 스키마를 처음 만들 때 이 시드 데이터도 같이 넣어줌(마이그레이션 없이도
    // 동작 - MetricsReceiverService가 EnsureCreated()를 쓰는 기존 방식 그대로 유지).
    // CPU/메모리/디스크는 90%, RTT는 200ms(200,000us) - 로컬 테스트 환경 기준 보수적인 기본값.
    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        modelBuilder.Entity<AlertThreshold>().HasData(
            new AlertThreshold { Id = 1, MetricType = AlertMetricType.CpuPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 2, MetricType = AlertMetricType.MemoryPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 3, MetricType = AlertMetricType.DiskPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 4, MetricType = AlertMetricType.TcpRttUs, Value = 200_000, Enabled = true }
        );
    }
}
```

**`MetricsReceiverService.cs`** — 상단 `using` 1줄 추가:

수정 전:
```csharp
using System.Buffers.Binary;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Apm;
using ApmConsole.Domain.Apm.Hubs;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using Microsoft.AspNetCore.SignalR;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
```

수정 후:
```csharp
using System.Buffers.Binary;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Apm;
using ApmConsole.Domain.Apm.Hubs;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using Microsoft.AspNetCore.SignalR;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
```

**`MetricsReceiverService.cs` — `StoreAndBroadcastAsync()`**

수정 전:
```csharp
    private async Task StoreAndBroadcastAsync(Metric metric)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        var record = new MetricRecord
        {
            Ts = DateTimeOffset.UtcNow,
            CpuUsagePercent = metric.CpuUsagePercent,
            MemUsedBytes = (long)metric.MemUsedBytes,
            MemTotalBytes = (long)metric.MemTotalBytes,
            DiskUsedBytes = (long)metric.DiskUsedBytes,
            DiskTotalBytes = (long)metric.DiskTotalBytes,
            NetRxBytesPerSec = (long)metric.NetRxBytesPerSec,
            NetTxBytesPerSec = (long)metric.NetTxBytesPerSec,
            TcpRttUs = (int)metric.TcpRttUs,
            TcpRttVarUs = (int)metric.TcpRttVarUs,
            TcpRetransmits = (int)metric.TcpRetransmits,
            TcpTotalRetrans = (int)metric.TcpTotalRetrans,
            TcpSndCwnd = (int)metric.TcpSndCwnd,
        };

        db.Metrics.Add(record);
        await db.SaveChangesAsync();

        Console.WriteLine($"[MetricsReceiverService] 저장 완료: cpu={record.CpuUsagePercent}%");

        await _hubContext.Clients.All.SendAsync("NewMetric", record);
    }
```

수정 후:
```csharp
    private async Task StoreAndBroadcastAsync(Metric metric)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        var record = new MetricRecord
        {
            Ts = DateTimeOffset.UtcNow,
            CpuUsagePercent = metric.CpuUsagePercent,
            MemUsedBytes = (long)metric.MemUsedBytes,
            MemTotalBytes = (long)metric.MemTotalBytes,
            DiskUsedBytes = (long)metric.DiskUsedBytes,
            DiskTotalBytes = (long)metric.DiskTotalBytes,
            NetRxBytesPerSec = (long)metric.NetRxBytesPerSec,
            NetTxBytesPerSec = (long)metric.NetTxBytesPerSec,
            TcpRttUs = (int)metric.TcpRttUs,
            TcpRttVarUs = (int)metric.TcpRttVarUs,
            TcpRetransmits = (int)metric.TcpRetransmits,
            TcpTotalRetrans = (int)metric.TcpTotalRetrans,
            TcpSndCwnd = (int)metric.TcpSndCwnd,
        };

        db.Metrics.Add(record);
        await db.SaveChangesAsync();

        Console.WriteLine($"[MetricsReceiverService] 저장 완료: cpu={record.CpuUsagePercent}%");

        await _hubContext.Clients.All.SendAsync("NewMetric", record);

        await EvaluateAlertsAsync(db, record);
    }

    // 저장된 메트릭 1건에 대해 활성화된 임계치를 전부 평가 - 상태 전이(Opened/Resolved)가
    // 있을 때만 AlertRecord를 기록하고 SignalR로 알림(2026-07-26, WORK_STATUS.md 2순위).
    private async Task EvaluateAlertsAsync(ApmDbContext db, MetricRecord record)
    {
        var memPercent = record.MemTotalBytes > 0 ? record.MemUsedBytes * 100.0 / record.MemTotalBytes : 0;
        var diskPercent = record.DiskTotalBytes > 0 ? record.DiskUsedBytes * 100.0 / record.DiskTotalBytes : 0;

        var currentValues = new Dictionary<AlertMetricType, double>
        {
            [AlertMetricType.CpuPercent] = record.CpuUsagePercent,
            [AlertMetricType.MemoryPercent] = memPercent,
            [AlertMetricType.DiskPercent] = diskPercent,
            [AlertMetricType.TcpRttUs] = record.TcpRttUs,
        };

        var thresholds = await db.AlertThresholds.AsNoTracking().Where(t => t.Enabled).ToListAsync();

        foreach (var threshold in thresholds)
        {
            var currentValue = currentValues[threshold.MetricType];

            var openAlert = await db.AlertRecords
                .Where(a => a.MetricType == threshold.MetricType && a.ClosedAt == null)
                .OrderByDescending(a => a.Id)
                .FirstOrDefaultAsync();

            var transition = AlertEvaluator.Evaluate(currentValue, threshold.Value, openAlert != null);

            if (transition == AlertTransition.Opened)
            {
                var opened = new AlertRecord
                {
                    MetricType = threshold.MetricType,
                    ThresholdValue = threshold.Value,
                    TriggerValue = currentValue,
                    OpenedAt = DateTimeOffset.UtcNow,
                };
                db.AlertRecords.Add(opened);
                await db.SaveChangesAsync();

                Console.WriteLine($"[MetricsReceiverService] 알림 발생: {threshold.MetricType}={currentValue:F1} (임계치 {threshold.Value})");
                await _hubContext.Clients.All.SendAsync("AlertOpened", opened);
            }
            else if (transition == AlertTransition.Resolved && openAlert != null)
            {
                openAlert.ResolvedValue = currentValue;
                openAlert.ClosedAt = DateTimeOffset.UtcNow;
                await db.SaveChangesAsync();

                Console.WriteLine($"[MetricsReceiverService] 알림 해제: {threshold.MetricType}={currentValue:F1}");
                await _hubContext.Clients.All.SendAsync("AlertResolved", openAlert);
            }
        }
    }
```

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Controllers/DashboardController.cs`** — 알림 페이지로 가는 링크 하나만 뷰에 추가할 거라 컨트롤러 자체는 변경 없음(아래 뷰 수정 참고).

### 제안 계속 — 나머지 파일(컨트롤러/뷰/모듈 등록/스타일)

**`ApmModule.cs`** — `AlertRecord`/`AlertThreshold`의 `MetricType`(enum)이 SignalR로 나갈 때 기본은 정수로 직렬화됨. JS 쪽에서 enum 순서(0,1,2,3)에 의존한 매핑 대신 이름 문자열로 매핑할 수 있게 `JsonStringEnumConverter` 추가.

수정 전:
```csharp
using ApmConsole.Contracts;
using ApmConsole.Domain.Apm.Hubs;
using ApmConsole.Domain.Apm.Infrastructure;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Routing;
using Microsoft.AspNetCore.SignalR;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;

namespace ApmConsole.Domain.Apm;

public class ApmModule : IDomainModule
{
	public string DomainName => "Apm";

	public void RegisterServices(IServiceCollection services, IConfiguration configuration)
	{
		var backend = configuration["Apm:StorageBackend"] ?? "SQLite";
		var connectionString = configuration["Apm:ConnectionString"]
			?? throw new InvalidOperationException("appsettings.json에 Apm:ConnectionString 설정이 필요합니다.");

		services.AddDbContext<ApmDbContext>(options =>
		{
			if (backend == "TimescaleDB")
				options.UseNpgsql(connectionString);
			else
				options.UseSqlite(connectionString);
		});

		services.AddSignalR();
		services.AddHostedService<MetricsReceiverService>();
	}

	public void MapEndpoints(IEndpointRouteBuilder endpoints)
	{
		endpoints.MapHub<MetricsHub>("/apm/hub/metrics");
	}
}
```

수정 후(전체 교체):
```csharp
using ApmConsole.Contracts;
using ApmConsole.Domain.Apm.Hubs;
using ApmConsole.Domain.Apm.Infrastructure;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Routing;
using Microsoft.AspNetCore.SignalR;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using System.Text.Json.Serialization;

namespace ApmConsole.Domain.Apm;

public class ApmModule : IDomainModule
{
	public string DomainName => "Apm";

	public void RegisterServices(IServiceCollection services, IConfiguration configuration)
	{
		var backend = configuration["Apm:StorageBackend"] ?? "SQLite";
		var connectionString = configuration["Apm:ConnectionString"]
			?? throw new InvalidOperationException("appsettings.json에 Apm:ConnectionString 설정이 필요합니다.");

		services.AddDbContext<ApmDbContext>(options =>
		{
			if (backend == "TimescaleDB")
				options.UseNpgsql(connectionString);
			else
				options.UseSqlite(connectionString);
		});

		// AlertRecord/AlertThreshold의 MetricType enum을 JSON에서 정수가 아니라 이름
		// 문자열로 내려줌 - JS 쪽에서 지표별 한글 라벨을 매핑할 때 enum 값 순서(0,1,2,3)에
		// 의존하지 않고 이름으로 매핑할 수 있게(2026-07-26, 알림 기능 추가).
		services.AddSignalR()
			.AddJsonProtocol(options =>
				options.PayloadSerializerOptions.Converters.Add(new JsonStringEnumConverter()));
		services.AddHostedService<MetricsReceiverService>();
	}

	public void MapEndpoints(IEndpointRouteBuilder endpoints)
	{
		endpoints.MapHub<MetricsHub>("/apm/hub/metrics");
	}
}
```

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Controllers/AlertsController.cs`** (신규)
```csharp
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using ApmConsole.Domain.Apm.Models;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace ApmConsole.Domain.Apm.Controllers;

[Area("Apm")]
[Route("apm/alerts")]
public class AlertsController : Controller
{
    private readonly ApmDbContext _db;

    public AlertsController(ApmDbContext db)
    {
        _db = db;
    }

    [HttpGet]
    public async Task<IActionResult> Index()
    {
        var thresholds = await _db.AlertThresholds.AsNoTracking()
            .OrderBy(t => t.MetricType)
            .ToListAsync();

        var active = await _db.AlertRecords.AsNoTracking()
            .Where(a => a.ClosedAt == null)
            .OrderByDescending(a => a.Id)
            .ToListAsync();

        var history = await _db.AlertRecords.AsNoTracking()
            .Where(a => a.ClosedAt != null)
            .OrderByDescending(a => a.Id)
            .Take(20)
            .ToListAsync();

        return View(new AlertsViewModel(thresholds, active, history));
    }

    // 폼 필드명 value[{Id}]/enabled[{Id}]에 대응(Alerts/Index.cshtml 참고) - 체크 안 된
    // 체크박스는 폼에 아예 안 실려서 enabled 딕셔너리에 그 Id가 없으면 "꺼짐"으로 처리.
    [HttpPost("thresholds")]
    public async Task<IActionResult> UpdateThresholds([FromForm] Dictionary<int, double> value, [FromForm] Dictionary<int, bool> enabled)
    {
        var thresholds = await _db.AlertThresholds.ToListAsync();

        foreach (var threshold in thresholds)
        {
            if (value.TryGetValue(threshold.Id, out var newValue))
                threshold.Value = newValue;

            threshold.Enabled = enabled.ContainsKey(threshold.Id);
        }

        await _db.SaveChangesAsync();
        return RedirectToAction(nameof(Index));
    }
}
```

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Areas/Apm/Views/Alerts/Index.cshtml`** (신규)
```html
@model ApmConsole.Domain.Apm.Models.AlertsViewModel
@functions {
	string MetricLabel(ApmConsole.Domain.Apm.Infrastructure.Persistence.AlertMetricType type) => type switch
	{
		ApmConsole.Domain.Apm.Infrastructure.Persistence.AlertMetricType.CpuPercent => "CPU 사용률",
		ApmConsole.Domain.Apm.Infrastructure.Persistence.AlertMetricType.MemoryPercent => "메모리 사용률",
		ApmConsole.Domain.Apm.Infrastructure.Persistence.AlertMetricType.DiskPercent => "디스크 사용률",
		ApmConsole.Domain.Apm.Infrastructure.Persistence.AlertMetricType.TcpRttUs => "TCP RTT",
		_ => type.ToString(),
	};
}

<link rel="stylesheet" href="~/css/site.css" />

<div class="dashboard">
	<h1>알림</h1>
	<a href="/apm/dashboard">← 대시보드로</a>
	<span id="connection-status" class="status-pill">실시간 연결 중...</span>

	<div class="card">
		<h2>임계치 설정</h2>
		<form method="post" action="/apm/alerts/thresholds">
			<table>
				<thead>
					<tr><th>지표</th><th>임계치</th><th>활성화</th></tr>
				</thead>
				<tbody>
				@foreach (var t in Model.Thresholds)
				{
					<tr>
						<td>@MetricLabel(t.MetricType)</td>
						<td><input type="number" step="0.1" name="value[@t.Id]" value="@t.Value" /></td>
						<td><input type="checkbox" name="enabled[@t.Id]" value="true" checked="@t.Enabled" /></td>
					</tr>
				}
				</tbody>
			</table>
			<button type="submit">저장</button>
		</form>
	</div>

	<div class="card">
		<h2>활성 알림</h2>
		<table id="activeTable">
			<thead><tr><th>지표</th><th>발생 시각</th><th>측정값</th><th>임계치</th></tr></thead>
			<tbody>
			@if (!Model.ActiveAlerts.Any())
			{
				<tr id="activeEmptyRow"><td colspan="4" class="empty-state">현재 활성 알림 없음</td></tr>
			}
			else
			{
				foreach (var a in Model.ActiveAlerts)
				{
					<tr class="alert-row-active" data-alert-id="@a.Id">
						<td>@MetricLabel(a.MetricType)</td>
						<td>@a.OpenedAt.ToLocalTime()</td>
						<td>@a.TriggerValue.ToString("F1")</td>
						<td>@a.ThresholdValue.ToString("F1")</td>
					</tr>
				}
			}
			</tbody>
		</table>
	</div>

	<div class="card">
		<h2>최근 이력(최대 20건)</h2>
		@if (!Model.RecentHistory.Any())
		{
			<p class="empty-state">아직 해제된 알림 이력이 없습니다.</p>
		}
		else
		{
			<table>
				<thead><tr><th>지표</th><th>발생</th><th>해제</th><th>측정값</th><th>복구값</th><th>임계치</th></tr></thead>
				<tbody>
				@foreach (var a in Model.RecentHistory)
				{
					<tr>
						<td>@MetricLabel(a.MetricType)</td>
						<td>@a.OpenedAt.ToLocalTime()</td>
						<td>@a.ClosedAt?.ToLocalTime()</td>
						<td>@a.TriggerValue.ToString("F1")</td>
						<td>@a.ResolvedValue?.ToString("F1")</td>
						<td>@a.ThresholdValue.ToString("F1")</td>
					</tr>
				}
				</tbody>
			</table>
		}
	</div>
</div>

<script src="~/lib/signalr/signalr.min.js"></script>
<script>
	const statusEl = document.getElementById("connection-status");

	// AlertMetricType enum 이름(JsonStringEnumConverter로 문자열로 내려옴, ApmModule.cs 참고) -> 한글 라벨.
	const metricLabels = {
		CpuPercent: "CPU 사용률",
		MemoryPercent: "메모리 사용률",
		DiskPercent: "디스크 사용률",
		TcpRttUs: "TCP RTT",
	};

	function upsertActiveRow(alert) {
		const tbody = document.querySelector("#activeTable tbody");
		document.getElementById("activeEmptyRow")?.remove();

		const row = document.createElement("tr");
		row.className = "alert-row-active";
		row.dataset.alertId = alert.id;
		row.innerHTML = `
			<td>${metricLabels[alert.metricType] ?? alert.metricType}</td>
			<td>${new Date(alert.openedAt).toLocaleString()}</td>
			<td>${alert.triggerValue.toFixed(1)}</td>
			<td>${alert.thresholdValue.toFixed(1)}</td>
		`;
		tbody.prepend(row);
	}

	function removeActiveRow(alertId) {
		document.querySelector(`#activeTable tr[data-alert-id="${alertId}"]`)?.remove();

		const tbody = document.querySelector("#activeTable tbody");
		if (!tbody.rows.length) {
			const row = document.createElement("tr");
			row.id = "activeEmptyRow";
			row.innerHTML = `<td colspan="4" class="empty-state">현재 활성 알림 없음</td>`;
			tbody.appendChild(row);
		}
	}

	const connection = new signalR.HubConnectionBuilder()
		.withUrl("/apm/hub/metrics")
		.withAutomaticReconnect()
		.build();

	connection.on("AlertOpened", upsertActiveRow);
	connection.on("AlertResolved", (alert) => removeActiveRow(alert.id));

	connection.start()
		.then(() => {
			statusEl.textContent = "실시간 연결됨";
			statusEl.classList.add("status-connected");
		})
		.catch(err => {
			console.error(err);
			statusEl.textContent = "실시간 연결 실패 - 새로고침으로만 갱신됩니다";
			statusEl.classList.add("status-error");
		});
</script>
```

**`Areas/Apm/Views/Dashboard/Index.cshtml`** — 알림 페이지로 가는 링크만 추가(그 외 전부 동일, 상단 일부만 발췌 — 범위가 `<h1>` 바로 아래 한 줄 삽입뿐이라 전체 재게시 대신 위치만 명시):

수정 전:
```html
<div class="dashboard">
	<h1>Apm 대시보드</h1>
	<span id="connection-status" class="status-pill">실시간 연결 중...</span>
```

수정 후:
```html
<div class="dashboard">
	<h1>Apm 대시보드</h1>
	<a href="/apm/alerts">알림 설정/이력 →</a>
	<span id="connection-status" class="status-pill">실시간 연결 중...</span>
```

**`APM_Console/src/ApmConsole.Host/wwwroot/css/site.css`** — 끝에 추가(기존 내용 변경 없음):
```css

.alert-row-active td:first-child {
    border-left: 3px solid var(--danger);
}
```

### 확인 필요 없음 — 재확인한 기존 결정 그대로 적용

이번 항목은 앞서 사용자가 확정한 4건(임계치 대상/저장 위치/트리거 시점/영속화)을 그대로 구현한 것 — 추가로 확인받을 판단 지점은 없다고 보고 전체를 한 번에 제안함.

### 결정 사항

- 아직 파일 생성 전 — `CLAUDE.md` 규칙대로 제안 단계. 사용자가 "적용해줘" 하면 위 9개 파일(신규 6 + 수정 3: `ApmDbContext.cs`/`MetricsReceiverService.cs`/`ApmModule.cs`/`Dashboard/Index.cshtml`/`site.css`)을 전부 실제로 반영 예정.
- 빌드 검증은 .NET(`dotnet build`)이 필요 - 이 환경엔 없으므로 사용자 쪽에서 확인 필요(APM_Agent/LoadTester 때와 동일한 패턴).

**2차 원인(2026-07-26)**: `strace` 설치 후 재실행하니 진단 메시지가 의도대로 출력됨(스크립트 수정 검증 완료) — `[Collector] fatal: LoadKeyFromHexFile - 파일 열기 실패: certs/agent_collector_aes.key`. `APM_Agent/certs/`에 생성 스크립트(`generate_test_cert.sh`/`generate_agent_collector_key.sh`/`generate_webserver_key.sh`)만 있고 실제 키/인증서 파일이 없음을 확인 — 이 WSL 체크아웃에서 `HOW_TO_RUN.md` 2절("최초 1회 설정")을 아직 안 한 상태(`.gitignore`로 커밋 안 되는 파일이라 새 클론/체크아웃마다 한 번씩 필요). 사용자에게 `certs/generate_test_cert.sh`·`generate_agent_collector_key.sh`·`generate_webserver_key.sh` 실행 요청.

### 1번째 실측 결과 — agents=1 (베이스라인 정합성 확인)

키 생성 후 재실행, 매트릭스 1번(`agents=1 duration=60 interval-ms=100 ramp-up=0`) 성공:

- `sent(total)=593` (60초 × 100ms 간격 ≈ 600건과 거의 일치)
- `queueDrops=0`, `reconnects=0`, `connectFails=0`, `connected=1`
- 파이프라인(LoadTester → Collector → strace/RSS 샘플링 → CSV) 전체가 의도대로 동작함을 확인.

**노이즈 이슈**: 에이전트 수가 늘면 `[ResilientSender] queued` 로그가 에이전트당 초당 ~10줄씩 찍혀(100 에이전트 × 60초 ≈ 6만 줄) 터미널에 그대로 보기 어려움. 스크립트가 이미 `loadtester_stdout.log`에 전체를 저장하므로, 남은 5개 실행은 터미널 출력을 `/dev/null`로 누르고 완료 후 `loadtester_result.csv`/`collector_strace.txt` 요약 부분만 확인하는 방식으로 안내함(사용자에게 안내한 명령은 아래 "다음 할 일" 참고).

## 2026-07-26 — 3순위(데이터 보존 정책) 설계 제안

### 배경

2순위(알림) 코드 적용 + `dotnet test` 13건 통과 확인 완료 후, 로드맵 순서대로 3순위로 전환. 착수 전 저장소 구조를 다시 확인 — 이 프로젝트엔 데이터 저장소가 **두 군데** 있음을 재확인:

1. **Collector(C++) 로컬 저장소** — `APM_Agent/Storage/{Sqlite,Timescale}MetricStore.cpp`. Collector가 Agent로부터 받은 메트릭을 자기 것으로도 하나 남겨두는 저장소(`main.cpp`의 `store->Store(pkt)`) — WebServer로의 전송(`pendingMetrics`/`webServerSender`)과는 별개 경로. 컴파일 타임에 SQLite/TimescaleDB 중 하나 선택(`APM_STORAGE_BACKEND`).
2. **Console(.NET) 중앙 저장소** — `ApmDbContext`(`Metrics`/`AlertThresholds`/`AlertRecords`). Collector가 네트워크로 보낸 걸 `MetricsReceiverService`가 받아서 쓰는, 대시보드/알림이 실제로 읽는 저장소.

**확인 필요 4건 → 사용자 결정**:
1. 적용 범위: **Console + Collector 둘 다**
2. 정책 방식: **시간 기준**(N일 지난 행 삭제)
3. 기본 보존 기간: **Metrics 30일**
4. `AlertRecord`(알림 이력): **Metrics보다 길게** — 발생량이 훨씬 적은(상태 전이 시에만 기록) 사건 이력이라 길게 남겨도 부담이 적음. 기본값 **180일**로 제안(30일의 6배 — 인시던트 회고 시 최근 몇 달을 되짚어볼 수 있는 정도, 필요시 설정값만 바꾸면 됨).

### 설계

**핵심 판단 — TimescaleDB와 SQLite는 구현 방식이 완전히 다름**:
- `TimescaleMetricStore`는 생성 시 `create_hypertable()`로 이미 하이퍼테이블(시간 범위별 청크 파티션)을 만들어 둠 → TimescaleDB 네이티브 `add_retention_policy()`를 한 번 등록해두면 백그라운드 잡이 청크째로 드롭함(행 단위 `DELETE`보다 훨씬 저렴, 인덱스 스캔조차 불필요). 그래서 `Prune()`은 **no-op**.
- `SqliteMetricStore`는 그런 파티셔닝이 없어 직접 `DELETE ... WHERE ts < ...`를 날려야 함. 단, SQLite는 `DELETE`만으로 파일 크기가 안 줄어드는 특성이 있어(빈 페이지가 파일 내부에 남아 재사용됨) `auto_vacuum = INCREMENTAL` 모드 + 매 `Prune()` 뒤 `PRAGMA incremental_vacuum`으로 점진적으로 반환(풀 `VACUUM`처럼 통째로 잠그지 않음). **주의**: 이미 만들어진 기존 `apm_metrics.db` 파일에는 이 PRAGMA가 소급 적용되지 않음(파일을 새로 만들거나 수동 `VACUUM` 한 번 필요) — 이번 설계 범위 밖으로 명시.
- Console 쪽 `ApmDbContext`는 백엔드가 SQLite/TimescaleDB 어느 쪽이어도 **EF Core가 만드는 건 그냥 평범한 테이블**(하이퍼테이블 아님 — `create_hypertable()`은 Collector의 C++ 저장소에서만 호출됨). 그래서 Console 쪽은 백엔드 분기 없이 EF Core `ExecuteDeleteAsync` 하나로 통일 가능.

**실행 방식**:
- Console: 새 `RetentionService`(`BackgroundService`) — 시작 직후 1회 + 이후 1시간 간격으로 `Metrics`/`AlertRecords`를 정리. `AlertRecords`는 `ClosedAt != null`(진행 중인 알림은 기간과 무관하게 항상 보존)인 것만 대상.
- Collector: 기존 `pushTimer`(WebServer 전송 주기 타이머)와 같은 패턴으로 `pruneTimer` 신설, 24시간 간격으로 `store->Prune(config.metricsRetentionDays)` 호출.
- 두 값 모두 설정 파일에서 읽음(Console: `appsettings.json`의 `Apm:MetricsRetentionDays`/`Apm:AlertRetentionDays`, Collector: `collector_config.json`의 `metrics_retention_days`) — 코드 재빌드 없이 조정 가능.

### 제안 — Console(.NET) 신규 파일

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Infrastructure/RetentionService.cs`** (신규)
```csharp
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;

namespace ApmConsole.Domain.Apm.Infrastructure;

/*------------------
    RetentionService
--------------------*/
// Metrics/AlertRecords가 무한정 쌓이지 않도록 오래된 행을 주기적으로 삭제(2026-07-26, 3순위).
// 시작 직후 1회 실행 + 이후 1시간 간격 반복 - 엔티티를 메모리로 로드하지 않고 EF Core의
// ExecuteDeleteAsync(단일 DELETE ... WHERE 문으로 변환됨)로 서버 사이드에서 바로 삭제.
public class RetentionService : BackgroundService
{
    private readonly int _metricsRetentionDays;
    private readonly int _alertRetentionDays;
    private readonly IServiceScopeFactory _scopeFactory;

    public RetentionService(IConfiguration configuration, IServiceScopeFactory scopeFactory)
    {
        _metricsRetentionDays = int.Parse(configuration["Apm:MetricsRetentionDays"] ?? "30");
        _alertRetentionDays = int.Parse(configuration["Apm:AlertRetentionDays"] ?? "180");
        _scopeFactory = scopeFactory;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromHours(1));

        do
        {
            await PruneAsync(stoppingToken);
        }
        while (await timer.WaitForNextTickAsync(stoppingToken));
    }

    private async Task PruneAsync(CancellationToken ct)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        var metricsCutoff = DateTimeOffset.UtcNow.AddDays(-_metricsRetentionDays);
        var metricsDeleted = await db.Metrics
            .Where(m => m.Ts < metricsCutoff)
            .ExecuteDeleteAsync(ct);

        // 진행 중인 알림(ClosedAt == null)은 기간과 무관하게 항상 보존 - 오래됐다고 지우면
        // 활성 알림 목록에서 사라지는 버그가 됨(AlertsController.Index의 active 조회 기준과 동일).
        var alertsCutoff = DateTimeOffset.UtcNow.AddDays(-_alertRetentionDays);
        var alertsDeleted = await db.AlertRecords
            .Where(a => a.ClosedAt != null && a.ClosedAt < alertsCutoff)
            .ExecuteDeleteAsync(ct);

        if (metricsDeleted > 0 || alertsDeleted > 0)
            Console.WriteLine($"[RetentionService] 정리 완료: metrics {metricsDeleted}건, alerts {alertsDeleted}건 삭제");
    }
}
```

### 제안 — Console(.NET) 기존 파일 수정 (수정 전 / 수정 후)

**변경 사유**: `RetentionService`가 매시간 `Ts`/`ClosedAt` 기준으로 `WHERE` 삭제를 돌리는데, 인덱스가 없으면 테이블이 커질수록 이 삭제 자체가 풀스캔이 되어 점점 느려짐(정작 정리해야 할 이유가 정리 작업 자체를 무겁게 만드는 역설). `AlertRecords.ClosedAt` 인덱스는 `EvaluateAlertsAsync`/`AlertsController`의 "현재 열린 알림" 조회에도 같이 도움됨. 신규 서비스도 DI 등록 필요.

**`ApmDbContext.cs`**

수정 전(2순위 적용 후 현재 디스크 상태):
```csharp
using Microsoft.EntityFrameworkCore;

namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

/*---------------
    ApmDbContext
-----------------*/
// WebServer 자체 소유의 메트릭 저장소(SQLite/TimescaleDB 선택 가능) - Collector가 네트워크로
// 보내주는 데이터를 MetricsReceiverService가 이 컨텍스트를 통해 씀. 예전에는 Collector의
// SQLite 파일을 직접 읽기만 했지만(같은 장비 전제), 이제 완전히 독립된 저장소라 스키마도
// EF Core 기본 관례를 그대로 씀(수동 컬럼명 매핑/keyless 설정 불필요).

public class ApmDbContext : DbContext
{
    public ApmDbContext(DbContextOptions<ApmDbContext> options) : base(options) { }

    public DbSet<MetricRecord> Metrics => Set<MetricRecord>();
    public DbSet<AlertThreshold> AlertThresholds => Set<AlertThreshold>();
    public DbSet<AlertRecord> AlertRecords => Set<AlertRecord>();

    // EnsureCreated()가 스키마를 처음 만들 때 이 시드 데이터도 같이 넣어줌(마이그레이션 없이도
    // 동작 - MetricsReceiverService가 EnsureCreated()를 쓰는 기존 방식 그대로 유지).
    // CPU/메모리/디스크는 90%, RTT는 200ms(200,000us) - 로컬 테스트 환경 기준 보수적인 기본값.
    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        modelBuilder.Entity<AlertThreshold>().HasData(
            new AlertThreshold { Id = 1, MetricType = AlertMetricType.CpuPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 2, MetricType = AlertMetricType.MemoryPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 3, MetricType = AlertMetricType.DiskPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 4, MetricType = AlertMetricType.TcpRttUs, Value = 200_000, Enabled = true }
        );
    }
}
```

수정 후(전체 교체):
```csharp
using Microsoft.EntityFrameworkCore;

namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

/*---------------
    ApmDbContext
-----------------*/
// WebServer 자체 소유의 메트릭 저장소(SQLite/TimescaleDB 선택 가능) - Collector가 네트워크로
// 보내주는 데이터를 MetricsReceiverService가 이 컨텍스트를 통해 씀. 예전에는 Collector의
// SQLite 파일을 직접 읽기만 했지만(같은 장비 전제), 이제 완전히 독립된 저장소라 스키마도
// EF Core 기본 관례를 그대로 씀(수동 컬럼명 매핑/keyless 설정 불필요).

public class ApmDbContext : DbContext
{
    public ApmDbContext(DbContextOptions<ApmDbContext> options) : base(options) { }

    public DbSet<MetricRecord> Metrics => Set<MetricRecord>();
    public DbSet<AlertThreshold> AlertThresholds => Set<AlertThreshold>();
    public DbSet<AlertRecord> AlertRecords => Set<AlertRecord>();

    // EnsureCreated()가 스키마를 처음 만들 때 이 시드 데이터도 같이 넣어줌(마이그레이션 없이도
    // 동작 - MetricsReceiverService가 EnsureCreated()를 쓰는 기존 방식 그대로 유지).
    // CPU/메모리/디스크는 90%, RTT는 200ms(200,000us) - 로컬 테스트 환경 기준 보수적인 기본값.
    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        modelBuilder.Entity<AlertThreshold>().HasData(
            new AlertThreshold { Id = 1, MetricType = AlertMetricType.CpuPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 2, MetricType = AlertMetricType.MemoryPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 3, MetricType = AlertMetricType.DiskPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 4, MetricType = AlertMetricType.TcpRttUs, Value = 200_000, Enabled = true }
        );

        // RetentionService가 매시간 Ts/ClosedAt 기준으로 WHERE 삭제를 돌리므로 인덱스가 없으면
        // 테이블이 커질수록 이 삭제 자체가 풀스캔이 됨(2026-07-26 3순위 설계) - AlertRecords.ClosedAt
        // 인덱스는 EvaluateAlertsAsync/AlertsController의 "현재 열린 알림" 조회에도 같이 도움됨.
        // 주의: EnsureCreated()는 신규 DB 파일에만 이 인덱스를 만듦 - 이미 존재하는 apm_metrics.db/
        // webserver_apm.db 파일에는 소급 적용 안 됨(마이그레이션 시스템이 아니라 EnsureCreated라
        // 스키마 변경이 자동 반영되지 않음 - 기존 파일은 수동 CREATE INDEX 또는 파일 재생성 필요).
        modelBuilder.Entity<MetricRecord>().HasIndex(m => m.Ts);
        modelBuilder.Entity<AlertRecord>().HasIndex(a => a.ClosedAt);
    }
}
```

**`ApmModule.cs`** — `RegisterServices()`에 `RetentionService` 등록 한 줄만 추가(그 외 전부 동일, `using`도 추가분 없음).

수정 전(2순위 적용 후 현재 디스크 상태의 `RegisterServices` 본문):
```csharp
	public void RegisterServices(IServiceCollection services, IConfiguration configuration)
	{
		var backend = configuration["Apm:StorageBackend"] ?? "SQLite";
		var connectionString = configuration["Apm:ConnectionString"]
			?? throw new InvalidOperationException("appsettings.json에 Apm:ConnectionString 설정이 필요합니다.");

		services.AddDbContext<ApmDbContext>(options =>
		{
			if (backend == "TimescaleDB")
				options.UseNpgsql(connectionString);
			else
				options.UseSqlite(connectionString);
		});

		// AlertRecord/AlertThreshold의 MetricType enum을 JSON에서 정수가 아니라 이름
		// 문자열로 내려줌 - JS 쪽에서 지표별 한글 라벨을 매핑할 때 enum 값 순서(0,1,2,3)에
		// 의존하지 않고 이름으로 매핑할 수 있게(2026-07-26, 알림 기능 추가).
		services.AddSignalR()
			.AddJsonProtocol(options =>
				options.PayloadSerializerOptions.Converters.Add(new JsonStringEnumConverter()));
		services.AddHostedService<MetricsReceiverService>();
	}
```

수정 후:
```csharp
	public void RegisterServices(IServiceCollection services, IConfiguration configuration)
	{
		var backend = configuration["Apm:StorageBackend"] ?? "SQLite";
		var connectionString = configuration["Apm:ConnectionString"]
			?? throw new InvalidOperationException("appsettings.json에 Apm:ConnectionString 설정이 필요합니다.");

		services.AddDbContext<ApmDbContext>(options =>
		{
			if (backend == "TimescaleDB")
				options.UseNpgsql(connectionString);
			else
				options.UseSqlite(connectionString);
		});

		// AlertRecord/AlertThreshold의 MetricType enum을 JSON에서 정수가 아니라 이름
		// 문자열로 내려줌 - JS 쪽에서 지표별 한글 라벨을 매핑할 때 enum 값 순서(0,1,2,3)에
		// 의존하지 않고 이름으로 매핑할 수 있게(2026-07-26, 알림 기능 추가).
		services.AddSignalR()
			.AddJsonProtocol(options =>
				options.PayloadSerializerOptions.Converters.Add(new JsonStringEnumConverter()));
		services.AddHostedService<MetricsReceiverService>();
		services.AddHostedService<RetentionService>();
	}
```

**`appsettings.json`**

수정 전:
```json
{
  "Logging": {
    "LogLevel": {
      "Default": "Information",
      "Microsoft.AspNetCore": "Warning"
    }
  },
  "AllowedHosts": "*",
  "EnabledDomains": [ "Apm", "Game" ],
  "Apm": {
    "StorageBackend": "SQLite",
    "ConnectionString": "Data Source=/home/shkim/dev/gw2-cross/APM_Console/webserver_apm.db",
    "ReceiverPort": "9100",
    "WebServerAesKeyPath": "/home/shkim/dev/gw2-cross/APM_Agent/certs/webserver_aes.key",
    "ReceiverCertPath": "/home/shkim/dev/gw2-cross/APM_Console/certs/webserver.crt",
    "ReceiverKeyPath": "/home/shkim/dev/gw2-cross/APM_Console/certs/webserver.key"
  }
}
```

수정 후:
```json
{
  "Logging": {
    "LogLevel": {
      "Default": "Information",
      "Microsoft.AspNetCore": "Warning"
    }
  },
  "AllowedHosts": "*",
  "EnabledDomains": [ "Apm", "Game" ],
  "Apm": {
    "StorageBackend": "SQLite",
    "ConnectionString": "Data Source=/home/shkim/dev/gw2-cross/APM_Console/webserver_apm.db",
    "ReceiverPort": "9100",
    "WebServerAesKeyPath": "/home/shkim/dev/gw2-cross/APM_Agent/certs/webserver_aes.key",
    "ReceiverCertPath": "/home/shkim/dev/gw2-cross/APM_Console/certs/webserver.crt",
    "ReceiverKeyPath": "/home/shkim/dev/gw2-cross/APM_Console/certs/webserver.key",
    "MetricsRetentionDays": "30",
    "AlertRetentionDays": "180"
  }
}
```

**참고(이번 제안과 무관, 발견 사항만 기록)**: `ConnectionString`/키·인증서 경로가 전부 옛 모노레포 경로(`/home/shkim/dev/gw2-cross/...`)로 남아있음 — 저장소 이관(2026-07-26) 이후 갱신 안 된 것으로 보임. 이번 3순위 범위 밖이라 손대지 않았고, 실제 실행 시 이 경로들이 무효라면 사용자 쪽에서 `APM_Console`/`APM_Agent` 기준으로 별도 수정 필요.

### 제안 — Collector(C++) 기존 파일 수정 (수정 전 / 수정 후)

**변경 사유**: `IMetricStore`에 `Prune()` 추가 → 두 구현체가 백엔드 특성에 맞게 다르게 구현(SQLite는 직접 DELETE, TimescaleDB는 네이티브 정책이라 no-op) → Collector가 이걸 주기적으로 호출.

**`APM_Agent/Storage/IMetricStore.h`**

수정 전:
```cpp
#pragma once
#include "pch.h"
#include "Protocol/Metric.pb.h"

/*----------------
	IMetricStore
------------------*/
// 메트릭 저장 백엔드 추상 인터페이스. 구현체는 컴파일 타임에 하나만 선택됨
// (CMakeLists.txt의 APM_STORAGE_BACKEND 옵션 참고) - Collector는 이 인터페이스만 알면 됨.
class IMetricStore
{
public:
    virtual ~IMetricStore() = default;
    virtual void Store(const apm::Metric& metric) = 0;
};
```

수정 후(전체 교체):
```cpp
#pragma once
#include "pch.h"
#include "Protocol/Metric.pb.h"

/*----------------
	IMetricStore
------------------*/
// 메트릭 저장 백엔드 추상 인터페이스. 구현체는 컴파일 타임에 하나만 선택됨
// (CMakeLists.txt의 APM_STORAGE_BACKEND 옵션 참고) - Collector는 이 인터페이스만 알면 됨.
class IMetricStore
{
public:
    virtual ~IMetricStore() = default;
    virtual void Store(const apm::Metric& metric) = 0;

    // retentionDays보다 오래된 행 삭제 - 백엔드별 구현 방식이 다름(SqliteMetricStore는
    // 직접 DELETE, TimescaleMetricStore는 생성자에서 등록한 네이티브 보존 정책이 백그라운드로
    // 알아서 처리하므로 이 함수는 사실상 no-op, 2026-07-26 3순위 설계).
    virtual void Prune(int retentionDays) = 0;
};
```

**`APM_Agent/Storage/SqliteMetricStore.h`**

수정 전:
```cpp
#pragma once
#include "pch.h"
#include "IMetricStore.h"
#include <sqlite3.h>

/*-------------------
	SqliteMetricStore
---------------------*/
// 파일 하나로 동작하는 임베디드 저장소 - 서버 프로세스/네트워크 설정 불필요.

class SqliteMetricStore : public IMetricStore
{
public:
	explicit SqliteMetricStore(const String& dbPath);
	~SqliteMetricStore() override;

	void Store(const apm::Metric& metric) override;

private:
	sqlite3* _db = nullptr;
	sqlite3_stmt* _insertStmt = nullptr;
};
```

수정 후(전체 교체):
```cpp
#pragma once
#include "pch.h"
#include "IMetricStore.h"
#include <sqlite3.h>

/*-------------------
	SqliteMetricStore
---------------------*/
// 파일 하나로 동작하는 임베디드 저장소 - 서버 프로세스/네트워크 설정 불필요.

class SqliteMetricStore : public IMetricStore
{
public:
	explicit SqliteMetricStore(const String& dbPath);
	~SqliteMetricStore() override;

	void Store(const apm::Metric& metric) override;
	void Prune(int retentionDays) override;

private:
	sqlite3* _db = nullptr;
	sqlite3_stmt* _insertStmt = nullptr;
};
```

**`APM_Agent/Storage/SqliteMetricStore.cpp`** — 전문(생성자에 `auto_vacuum` PRAGMA 추가 + `Prune()` 신설).

수정 전:
```cpp
#include "pch.h"
#include "SqliteMetricStore.h"

namespace
{
	constexpr const char* CREATE_TABLE_SQL =
		"CREATE TABLE IF NOT EXISTS metrics ("
		"  ts INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
		"  cpu_usage_percent REAL,"
		"  mem_used_bytes INTEGER,"
		"  mem_total_bytes INTEGER,"
		"  disk_used_bytes INTEGER,"
		"  disk_total_bytes INTEGER,"
		"  net_rx_bytes_per_sec INTEGER,"
		"  net_tx_bytes_per_sec INTEGER,"
		"  tcp_rtt_us INTEGER,"
		"  tcp_retransmits INTEGER"
		");";

	constexpr const char* INSERT_SQL =
		"INSERT INTO metrics "
		"(cpu_usage_percent, mem_used_bytes, mem_total_bytes, disk_used_bytes, disk_total_bytes, "
		" net_rx_bytes_per_sec, net_tx_bytes_per_sec, tcp_rtt_us, tcp_retransmits) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
}

SqliteMetricStore::SqliteMetricStore(const String& dbPath)
{
	if (::sqlite3_open(dbPath.c_str(), &_db) != SQLITE_OK)
		throw std::runtime_error("SqliteMetricStore - open failed: " + String(::sqlite3_errmsg(_db)));

	char* errMsg = nullptr;
	if (::sqlite3_exec(_db, CREATE_TABLE_SQL, nullptr, nullptr, &errMsg) != SQLITE_OK)
	{
		String err = errMsg ? errMsg : "unknown";
		::sqlite3_free(errMsg);
		throw std::runtime_error("SqliteMetricStore - CREATE TABLE failed: " + err);
	}

	if (::sqlite3_prepare_v2(_db, INSERT_SQL, -1, &_insertStmt, nullptr) != SQLITE_OK)
		throw std::runtime_error("SqliteMetricStore - prepare failed: " + String(::sqlite3_errmsg(_db)));
}

SqliteMetricStore::~SqliteMetricStore()
{
	if (_insertStmt)
		::sqlite3_finalize(_insertStmt);
	if (_db)
		::sqlite3_close(_db);
}

void SqliteMetricStore::Store(const apm::Metric& metric)
{
	::sqlite3_reset(_insertStmt);
	::sqlite3_bind_double(_insertStmt, 1, metric.cpu_usage_percent());
	::sqlite3_bind_int64(_insertStmt, 2, static_cast<sqlite3_int64>(metric.mem_used_bytes()));
	::sqlite3_bind_int64(_insertStmt, 3, static_cast<sqlite3_int64>(metric.mem_total_bytes()));
	::sqlite3_bind_int64(_insertStmt, 4, static_cast<sqlite3_int64>(metric.disk_used_bytes()));
	::sqlite3_bind_int64(_insertStmt, 5, static_cast<sqlite3_int64>(metric.disk_total_bytes()));
	::sqlite3_bind_int64(_insertStmt, 6, static_cast<sqlite3_int64>(metric.net_rx_bytes_per_sec()));
	::sqlite3_bind_int64(_insertStmt, 7, static_cast<sqlite3_int64>(metric.net_tx_bytes_per_sec()));
	::sqlite3_bind_int(_insertStmt, 8, static_cast<int>(metric.tcp_rtt_us()));
	::sqlite3_bind_int(_insertStmt, 9, static_cast<int>(metric.tcp_retransmits()));

	if (::sqlite3_step(_insertStmt) != SQLITE_DONE)
		std::cerr << "[SqliteMetricStore] insert failed: " << ::sqlite3_errmsg(_db) << std::endl;
}
```

수정 후(전체 교체):
```cpp
#include "pch.h"
#include "SqliteMetricStore.h"

namespace
{
	constexpr const char* CREATE_TABLE_SQL =
		"CREATE TABLE IF NOT EXISTS metrics ("
		"  ts INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
		"  cpu_usage_percent REAL,"
		"  mem_used_bytes INTEGER,"
		"  mem_total_bytes INTEGER,"
		"  disk_used_bytes INTEGER,"
		"  disk_total_bytes INTEGER,"
		"  net_rx_bytes_per_sec INTEGER,"
		"  net_tx_bytes_per_sec INTEGER,"
		"  tcp_rtt_us INTEGER,"
		"  tcp_retransmits INTEGER"
		");";

	constexpr const char* INSERT_SQL =
		"INSERT INTO metrics "
		"(cpu_usage_percent, mem_used_bytes, mem_total_bytes, disk_used_bytes, disk_total_bytes, "
		" net_rx_bytes_per_sec, net_tx_bytes_per_sec, tcp_rtt_us, tcp_retransmits) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";

	// retentionDays를 바인딩 파라미터로 받음(문자열 조립 없이) - 삭제 기준을 매번
	// "지금 - N일"로 재계산(2026-07-26 3순위 설계).
	constexpr const char* PRUNE_SQL =
		"DELETE FROM metrics WHERE ts < strftime('%s','now') - (? * 86400);";
}

SqliteMetricStore::SqliteMetricStore(const String& dbPath)
{
	if (::sqlite3_open(dbPath.c_str(), &_db) != SQLITE_OK)
		throw std::runtime_error("SqliteMetricStore - open failed: " + String(::sqlite3_errmsg(_db)));

	// DELETE만으로는 SQLite 파일 크기가 줄지 않음(빈 페이지가 파일 내부에서 재사용될 뿐 OS에
	// 반환되지 않음) - incremental_vacuum 모드로 열어두면 Prune() 직후 PRAGMA incremental_vacuum
	// 한 번으로 빈 페이지를 점진적으로 반환할 수 있음(풀 VACUUM처럼 테이블 전체를 오래 잠그지
	// 않음). auto_vacuum 모드는 빈 DB에만 적용되므로 CREATE TABLE보다 먼저 설정해야 함 - 이미
	// auto_vacuum=NONE으로 만들어진 기존 apm_metrics.db 파일에는 소급 적용 안 됨(2026-07-26 확인,
	// 기존 파일 전환은 이번 설계 범위 밖 - 필요시 수동 VACUUM 한 번 또는 파일 재생성).
	::sqlite3_exec(_db, "PRAGMA auto_vacuum = INCREMENTAL;", nullptr, nullptr, nullptr);

	char* errMsg = nullptr;
	if (::sqlite3_exec(_db, CREATE_TABLE_SQL, nullptr, nullptr, &errMsg) != SQLITE_OK)
	{
		String err = errMsg ? errMsg : "unknown";
		::sqlite3_free(errMsg);
		throw std::runtime_error("SqliteMetricStore - CREATE TABLE failed: " + err);
	}

	if (::sqlite3_prepare_v2(_db, INSERT_SQL, -1, &_insertStmt, nullptr) != SQLITE_OK)
		throw std::runtime_error("SqliteMetricStore - prepare failed: " + String(::sqlite3_errmsg(_db)));
}

SqliteMetricStore::~SqliteMetricStore()
{
	if (_insertStmt)
		::sqlite3_finalize(_insertStmt);
	if (_db)
		::sqlite3_close(_db);
}

void SqliteMetricStore::Store(const apm::Metric& metric)
{
	::sqlite3_reset(_insertStmt);
	::sqlite3_bind_double(_insertStmt, 1, metric.cpu_usage_percent());
	::sqlite3_bind_int64(_insertStmt, 2, static_cast<sqlite3_int64>(metric.mem_used_bytes()));
	::sqlite3_bind_int64(_insertStmt, 3, static_cast<sqlite3_int64>(metric.mem_total_bytes()));
	::sqlite3_bind_int64(_insertStmt, 4, static_cast<sqlite3_int64>(metric.disk_used_bytes()));
	::sqlite3_bind_int64(_insertStmt, 5, static_cast<sqlite3_int64>(metric.disk_total_bytes()));
	::sqlite3_bind_int64(_insertStmt, 6, static_cast<sqlite3_int64>(metric.net_rx_bytes_per_sec()));
	::sqlite3_bind_int64(_insertStmt, 7, static_cast<sqlite3_int64>(metric.net_tx_bytes_per_sec()));
	::sqlite3_bind_int(_insertStmt, 8, static_cast<int>(metric.tcp_rtt_us()));
	::sqlite3_bind_int(_insertStmt, 9, static_cast<int>(metric.tcp_retransmits()));

	if (::sqlite3_step(_insertStmt) != SQLITE_DONE)
		std::cerr << "[SqliteMetricStore] insert failed: " << ::sqlite3_errmsg(_db) << std::endl;
}

void SqliteMetricStore::Prune(int retentionDays)
{
	sqlite3_stmt* pruneStmt = nullptr;
	if (::sqlite3_prepare_v2(_db, PRUNE_SQL, -1, &pruneStmt, nullptr) != SQLITE_OK)
	{
		std::cerr << "[SqliteMetricStore] prune prepare failed: " << ::sqlite3_errmsg(_db) << std::endl;
		return;
	}

	::sqlite3_bind_int(pruneStmt, 1, retentionDays);

	if (::sqlite3_step(pruneStmt) != SQLITE_DONE)
		std::cerr << "[SqliteMetricStore] prune failed: " << ::sqlite3_errmsg(_db) << std::endl;
	else
		std::cout << "[SqliteMetricStore] prune 완료: " << ::sqlite3_changes(_db)
			<< "건 삭제 (retention=" << retentionDays << "일)" << std::endl;

	::sqlite3_finalize(pruneStmt);

	// 빈 페이지를 점진적으로 OS에 반환 - 호출 1번에 일부만 처리되므로(전체 VACUUM처럼
	// 오래 잠그지 않음) 매 Prune() 호출마다 같이 실행해도 부담이 적음.
	::sqlite3_exec(_db, "PRAGMA incremental_vacuum;", nullptr, nullptr, nullptr);
}
```

**`APM_Agent/Storage/TimescaleMetricStore.h`**

수정 전:
```cpp
#pragma once
#include "pch.h"
#include "IMetricStore.h"
#include "DBConnection.h"   // GW2_CrossPlatformCore/DB - 구 Phase 4에서 만든 libpq 래퍼 재사용

/*----------------------
	TimescaleMetricStore
------------------------*/
// TimescaleDB(Postgres 확장) 백엔드. 연결/쿼리 실행은 기존 DBConnection에 위임.

class TimescaleMetricStore : public IMetricStore
{
public:
	explicit TimescaleMetricStore(const String& connectionString);

	void Store(const apm::Metric& metric) override;

private:
	DBConnection _connection;
};
```

수정 후(전체 교체):
```cpp
#pragma once
#include "pch.h"
#include "IMetricStore.h"
#include "DBConnection.h"   // GW2_CrossPlatformCore/DB - 구 Phase 4에서 만든 libpq 래퍼 재사용

/*----------------------
	TimescaleMetricStore
------------------------*/
// TimescaleDB(Postgres 확장) 백엔드. 연결/쿼리 실행은 기존 DBConnection에 위임.

class TimescaleMetricStore : public IMetricStore
{
public:
	explicit TimescaleMetricStore(const String& connectionString, int retentionDays);

	void Store(const apm::Metric& metric) override;
	void Prune(int retentionDays) override;

private:
	DBConnection _connection;
};
```

**`APM_Agent/Storage/TimescaleMetricStore.cpp`** — 전문(생성자에 보존 정책 등록 추가 + `Prune()`은 no-op).

수정 전:
```cpp
#include "pch.h"
#include "TimescaleMetricStore.h"
#include <cstdio>

TimescaleMetricStore::TimescaleMetricStore(const String& connectionString)
{
	if (!_connection.Connect(connectionString.c_str()))
		throw std::runtime_error("TimescaleMetricStore - DB 연결 실패");

	const char* createSql =
		"CREATE TABLE IF NOT EXISTS metrics ("
		"  ts TIMESTAMPTZ NOT NULL DEFAULT now(),"
		"  cpu_usage_percent DOUBLE PRECISION,"
		"  mem_used_bytes BIGINT,"
		"  mem_total_bytes BIGINT,"
		"  disk_used_bytes BIGINT,"
		"  disk_total_bytes BIGINT,"
		"  net_rx_bytes_per_sec BIGINT,"
		"  net_tx_bytes_per_sec BIGINT,"
		"  tcp_rtt_us INTEGER,"
		"  tcp_retransmits INTEGER"
		");"
		"SELECT create_hypertable('metrics', 'ts', if_not_exists => TRUE);";

	if (!_connection.Execute(createSql, 0, nullptr))
		throw std::runtime_error("TimescaleMetricStore - 테이블/하이퍼테이블 생성 실패");
}

void TimescaleMetricStore::Store(const apm::Metric& metric)
{
	char cpu[64], memUsed[32], memTotal[32], diskUsed[32], diskTotal[32];
	char netRx[32], netTx[32], rtt[16], retrans[16];

	std::snprintf(cpu, sizeof(cpu), "%f", metric.cpu_usage_percent());
	std::snprintf(memUsed, sizeof(memUsed), "%llu", (unsigned long long)metric.mem_used_bytes());
	std::snprintf(memTotal, sizeof(memTotal), "%llu", (unsigned long long)metric.mem_total_bytes());
	std::snprintf(diskUsed, sizeof(diskUsed), "%llu", (unsigned long long)metric.disk_used_bytes());
	std::snprintf(diskTotal, sizeof(diskTotal), "%llu", (unsigned long long)metric.disk_total_bytes());
	std::snprintf(netRx, sizeof(netRx), "%llu", (unsigned long long)metric.net_rx_bytes_per_sec());
	std::snprintf(netTx, sizeof(netTx), "%llu", (unsigned long long)metric.net_tx_bytes_per_sec());
	std::snprintf(rtt, sizeof(rtt), "%u", metric.tcp_rtt_us());
	std::snprintf(retrans, sizeof(retrans), "%u", metric.tcp_retransmits());

	const char* params[9] = { cpu, memUsed, memTotal, diskUsed, diskTotal, netRx, netTx, rtt, retrans };

	const char* insertSql =
		"INSERT INTO metrics "
		"(cpu_usage_percent, mem_used_bytes, mem_total_bytes, disk_used_bytes, disk_total_bytes, "
		" net_rx_bytes_per_sec, net_tx_bytes_per_sec, tcp_rtt_us, tcp_retransmits) "
		"VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9);";

	if (!_connection.Execute(insertSql, 9, params))
		std::cerr << "[TimescaleMetricStore] insert failed" << std::endl;
}
```

수정 후(전체 교체):
```cpp
#include "pch.h"
#include "TimescaleMetricStore.h"
#include <cstdio>

TimescaleMetricStore::TimescaleMetricStore(const String& connectionString, int retentionDays)
{
	if (!_connection.Connect(connectionString.c_str()))
		throw std::runtime_error("TimescaleMetricStore - DB 연결 실패");

	const char* createSql =
		"CREATE TABLE IF NOT EXISTS metrics ("
		"  ts TIMESTAMPTZ NOT NULL DEFAULT now(),"
		"  cpu_usage_percent DOUBLE PRECISION,"
		"  mem_used_bytes BIGINT,"
		"  mem_total_bytes BIGINT,"
		"  disk_used_bytes BIGINT,"
		"  disk_total_bytes BIGINT,"
		"  net_rx_bytes_per_sec BIGINT,"
		"  net_tx_bytes_per_sec BIGINT,"
		"  tcp_rtt_us INTEGER,"
		"  tcp_retransmits INTEGER"
		");"
		"SELECT create_hypertable('metrics', 'ts', if_not_exists => TRUE);";

	if (!_connection.Execute(createSql, 0, nullptr))
		throw std::runtime_error("TimescaleMetricStore - 테이블/하이퍼테이블 생성 실패");

	// TimescaleDB 네이티브 보존 정책 - 청크(시간 범위 파티션) 단위로 통째로 드롭하므로
	// SqliteMetricStore처럼 행 단위 DELETE보다 훨씬 저렴함. 등록 후엔 TimescaleDB 백그라운드
	// 잡이 알아서 주기 실행 - Collector가 반복 호출할 필요가 없음(Prune()이 no-op인 이유,
	// 2026-07-26 3순위 설계). if_not_exists=>TRUE라 재시작마다 다시 호출해도 안전(중복 등록 안 됨).
	char retentionDaysStr[16];
	std::snprintf(retentionDaysStr, sizeof(retentionDaysStr), "%d", retentionDays);
	const char* policyParams[1] = { retentionDaysStr };
	const char* policySql =
		"SELECT add_retention_policy('metrics', INTERVAL '1 day' * $1::int, if_not_exists => TRUE);";

	if (!_connection.Execute(policySql, 1, policyParams))
		throw std::runtime_error("TimescaleMetricStore - 보존 정책 등록 실패");
}

void TimescaleMetricStore::Store(const apm::Metric& metric)
{
	char cpu[64], memUsed[32], memTotal[32], diskUsed[32], diskTotal[32];
	char netRx[32], netTx[32], rtt[16], retrans[16];

	std::snprintf(cpu, sizeof(cpu), "%f", metric.cpu_usage_percent());
	std::snprintf(memUsed, sizeof(memUsed), "%llu", (unsigned long long)metric.mem_used_bytes());
	std::snprintf(memTotal, sizeof(memTotal), "%llu", (unsigned long long)metric.mem_total_bytes());
	std::snprintf(diskUsed, sizeof(diskUsed), "%llu", (unsigned long long)metric.disk_used_bytes());
	std::snprintf(diskTotal, sizeof(diskTotal), "%llu", (unsigned long long)metric.disk_total_bytes());
	std::snprintf(netRx, sizeof(netRx), "%llu", (unsigned long long)metric.net_rx_bytes_per_sec());
	std::snprintf(netTx, sizeof(netTx), "%llu", (unsigned long long)metric.net_tx_bytes_per_sec());
	std::snprintf(rtt, sizeof(rtt), "%u", metric.tcp_rtt_us());
	std::snprintf(retrans, sizeof(retrans), "%u", metric.tcp_retransmits());

	const char* params[9] = { cpu, memUsed, memTotal, diskUsed, diskTotal, netRx, netTx, rtt, retrans };

	const char* insertSql =
		"INSERT INTO metrics "
		"(cpu_usage_percent, mem_used_bytes, mem_total_bytes, disk_used_bytes, disk_total_bytes, "
		" net_rx_bytes_per_sec, net_tx_bytes_per_sec, tcp_rtt_us, tcp_retransmits) "
		"VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9);";

	if (!_connection.Execute(insertSql, 9, params))
		std::cerr << "[TimescaleMetricStore] insert failed" << std::endl;
}

void TimescaleMetricStore::Prune(int /*retentionDays*/)
{
	// no-op - 생성자에서 등록한 add_retention_policy가 TimescaleDB 백그라운드 워커로
	// 알아서 처리함(위 생성자 주석 참고). Collector 쪽에서 주기 호출은 하지만 여기선 아무것도
	// 안 함 - IMetricStore 인터페이스를 통일하기 위한 형식상의 오버라이드.
}
```

**`APM_Agent/Storage/MetricStoreFactory.h`**

수정 전:
```cpp
#pragma once
#include "pch.h"
#include "IMetricStore.h"
#include <memory>

// 컴파일 타임에 선택된 백엔드에 맞는 IMetricStore 구현체를 생성.
// connectionInfo의 의미는 백엔드마다 다름 (SQLite: 파일 경로, TimescaleDB: libpq 연결 문자열).
std::unique_ptr<IMetricStore> CreateMetricStore(const String& connectionInfo);
```

수정 후(전체 교체):
```cpp
#pragma once
#include "pch.h"
#include "IMetricStore.h"
#include <memory>

// 컴파일 타임에 선택된 백엔드에 맞는 IMetricStore 구현체를 생성.
// connectionInfo의 의미는 백엔드마다 다름 (SQLite: 파일 경로, TimescaleDB: libpq 연결 문자열).
// retentionDays는 SqliteMetricStore는 그냥 들고 있다가 나중에 Prune() 호출 때 쓰고,
// TimescaleMetricStore는 생성자에서 곧바로 네이티브 보존 정책 등록에 씀(2026-07-26 3순위 설계).
std::unique_ptr<IMetricStore> CreateMetricStore(const String& connectionInfo, int retentionDays);
```

**`APM_Agent/Storage/MetricStoreFactory.cpp`**

수정 전:
```cpp
#include "pch.h"
#include "MetricStoreFactory.h"

#if defined(APM_STORAGE_SQLITE)
#include "SqliteMetricStore.h"
#elif defined(APM_STORAGE_TIMESCALEDB)
#include "TimescaleMetricStore.h"
#else
#error "APM_STORAGE_SQLITE 또는 APM_STORAGE_TIMESCALEDB 중 하나가 정의되어야 함 - CMakeLists.txt의 APM_STORAGE_BACKEND 확인"
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

수정 후(전체 교체):
```cpp
#include "pch.h"
#include "MetricStoreFactory.h"

#if defined(APM_STORAGE_SQLITE)
#include "SqliteMetricStore.h"
#elif defined(APM_STORAGE_TIMESCALEDB)
#include "TimescaleMetricStore.h"
#else
#error "APM_STORAGE_SQLITE 또는 APM_STORAGE_TIMESCALEDB 중 하나가 정의되어야 함 - CMakeLists.txt의 APM_STORAGE_BACKEND 확인"
#endif

std::unique_ptr<IMetricStore> CreateMetricStore(const String& connectionInfo, int retentionDays)
{
#if defined(APM_STORAGE_SQLITE)
	return std::make_unique<SqliteMetricStore>(connectionInfo);
#elif defined(APM_STORAGE_TIMESCALEDB)
	return std::make_unique<TimescaleMetricStore>(connectionInfo, retentionDays);
#endif
}
```

**`APM_Agent/Collector/CollectorConfig.h`**

수정 전:
```cpp
#pragma once
#include "pch.h"

/*------------------
    CollectorConfig
--------------------*/
// Collector가 WebServer로 데이터를 보낼 때 쓰는 설정. JSON 파일로 관리 - 차후
// WebServer의 웹 UI에서 이 값을 읽거나 편집하기 쉽도록 미리 대비한 포맷
// (지금은 편집 기능 자체는 구현 범위 밖 - 파일 포맷만 준비).

struct CollectorConfig
{
    String webServerHost = "127.0.0.1";
    unsigned short webServerPort = 9100;
    int pushIntervalSeconds = 10;
};

// JSON 파일을 읽어 설정을 만듦. 파일이 없으면 기본값을 그대로 씀(로컬 데모 시
// 설정 파일 없이도 동작). 파일은 있는데 특정 키가 빠졌으면 그 키만 기본값 유지.
CollectorConfig LoadCollectorConfig(const String& path);
```

수정 후(전체 교체):
```cpp
#pragma once
#include "pch.h"

/*------------------
    CollectorConfig
--------------------*/
// Collector가 WebServer로 데이터를 보낼 때 쓰는 설정. JSON 파일로 관리 - 차후
// WebServer의 웹 UI에서 이 값을 읽거나 편집하기 쉽도록 미리 대비한 포맷
// (지금은 편집 기능 자체는 구현 범위 밖 - 파일 포맷만 준비).

struct CollectorConfig
{
    String webServerHost = "127.0.0.1";
    unsigned short webServerPort = 9100;
    int pushIntervalSeconds = 10;
    int metricsRetentionDays = 30;   // Collector 로컬 저장소 보존 기간(2026-07-26 3순위)
};

// JSON 파일을 읽어 설정을 만듦. 파일이 없으면 기본값을 그대로 씀(로컬 데모 시
// 설정 파일 없이도 동작). 파일은 있는데 특정 키가 빠졌으면 그 키만 기본값 유지.
CollectorConfig LoadCollectorConfig(const String& path);
```

**`APM_Agent/Collector/CollectorConfig.cpp`**

수정 전:
```cpp
#include "pch.h"
#include "CollectorConfig.h"
#include <nlohmann/json.hpp>
#include <fstream>

CollectorConfig LoadCollectorConfig(const String& path)
{
    CollectorConfig config;

    std::ifstream file(path);
    if (!file.is_open())
        return config;

    nlohmann::json json;
    file >> json;

    if (json.contains("webserver_host"))
        config.webServerHost = json["webserver_host"].get<String>();
    if (json.contains("webserver_port"))
        config.webServerPort = json["webserver_port"].get<unsigned short>();
    if (json.contains("push_interval_seconds"))
        config.pushIntervalSeconds = json["push_interval_seconds"].get<int>();

    return config;
}
```

수정 후(전체 교체):
```cpp
#include "pch.h"
#include "CollectorConfig.h"
#include <nlohmann/json.hpp>
#include <fstream>

CollectorConfig LoadCollectorConfig(const String& path)
{
    CollectorConfig config;

    std::ifstream file(path);
    if (!file.is_open())
        return config;

    nlohmann::json json;
    file >> json;

    if (json.contains("webserver_host"))
        config.webServerHost = json["webserver_host"].get<String>();
    if (json.contains("webserver_port"))
        config.webServerPort = json["webserver_port"].get<unsigned short>();
    if (json.contains("push_interval_seconds"))
        config.pushIntervalSeconds = json["push_interval_seconds"].get<int>();
    if (json.contains("metrics_retention_days"))
        config.metricsRetentionDays = json["metrics_retention_days"].get<int>();

    return config;
}
```

**`APM_Agent/Collector/main.cpp`** — `CreateMetricStore` 호출에 `retentionDays` 인자 추가 + `pushTimer`와 같은 패턴으로 `pruneTimer` 신설(24시간 간격). 함수 전문:

수정 전:
```cpp
#include "pch.h"
#include "ApmSession.h"
#include "AesGcmPayload.h"
#include "ResilientSender.h"
#include "PrivilegeDrop.h"
#include "PacketHandler.h"
#include "KeyLoader.h"
#include "CollectorConfig.h"
#include "Protocol/Metric.pb.h"
#include "Storage/MetricStoreFactory.h"
#include <thread>

namespace
{
    constexpr unsigned short PORT = 9000;
    // SQLite: 파일 경로 / TimescaleDB: libpq 연결 문자열("host=... dbname=... user=...")
    constexpr const char* STORAGE_CONNECTION_INFO = "apm_metrics.db";
}

int main()
{
#ifdef _WIN32
    // 소스가 UTF-8(/utf-8)로 컴파일되는데 Windows 콘솔 기본 코드페이지(한글 Windows는 949)는
    // 그와 달라서, std::cout으로 찍는 한글 문자열이 콘솔에서 깨져 보임 - 출력 코드페이지를
    // UTF-8로 맞춰서 해결. Linux는 기본이 UTF-8이라 이 문제 자체가 없음.
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        // Agent<->Collector, Collector<->WebServer 두 구간 모두 AES-256-GCM(AesGcmPayload) -
        // 구간별 키는 분리 유지(한 쪽이 유출돼도 다른 구간은 안전). 배경: Docs/ARIA_TO_AES_MIGRATION.md
        AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");
        AesGcmCipher::Key webServerKey = LoadKeyFromHexFile("certs/webserver_aes.key");

        CollectorConfig config = LoadCollectorConfig("collector_config.json");

        auto store = CreateMetricStore(STORAGE_CONNECTION_INFO);

        // Collector가 받은 뒤 아직 WebServer로 안 보낸 메트릭들 - 주기/CLI 트리거로 비워짐.
        std::vector<apm::Metric> pendingMetrics;

        asio::io_context ioContext;

        // Agent 접속을 받는 서버 역할 컨텍스트(기존)
        asio::ssl::context sslContext(asio::ssl::context::tls_server);
        sslContext.use_certificate_chain_file("certs/server.crt");
        sslContext.use_private_key_file("certs/server.key", asio::ssl::context::pem);

        // WebServer에 접속하는 클라이언트 역할 컨텍스트(신규) - Agent용과 모드가 달라 별도 필요.
        asio::ssl::context webServerSslContext(asio::ssl::context::tls_client);
        // 테스트용 자체 서명 인증서라 CA 검증 생략(Agent->Collector와 동일한 이유, 프로덕션 금지).
        webServerSslContext.set_verify_mode(asio::ssl::verify_none);

        ResilientSender webServerSender(ioContext, webServerSslContext,
            config.webServerHost, config.webServerPort,
            [webServerKey]() { return std::make_unique<AesGcmPayload>(webServerKey); });

        PacketHandler::Register<apm::Metric>(
            [&store, &pendingMetrics](const apm::Metric& pkt)
            {
                store->Store(pkt);
                pendingMetrics.push_back(pkt);
                std::cout << "[Collector] metric stored: cpu=" << pkt.cpu_usage_percent()
                    << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                    << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                    << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                    << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                    << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                    << ",cwnd:" << pkt.tcp_snd_cwnd()
                    << std::endl;
            });

        asio::ip::tcp::acceptor acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), PORT));

        // 포트바인딩 : 특권이 필요할 수 있는 유일한 단계, 완료 직후 권한 하향
        PrivilegeDrop::DropTo("nobody");

        std::function<void()> doAccept;
        doAccept = [&]()
        {
            acceptor.async_accept(
                [&](const asio::error_code &ec, asio::ip::tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::cout << "[Collector] connection accepted" << std::endl;
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::cerr << "[Collector] accept error : " << ec.message() << std::endl;
                    }
                    doAccept();
                });
        };
        doAccept();

        // pendingMetrics에 쌓인 걸 전부 WebServer로 보내고 비움 - 주기 타이머와 CLI 트리거
        // 둘 다 이 함수 하나를 호출함(로직 중복 방지).
        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (pendingMetrics.empty())
                return;

            std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
            for (const auto& m : pendingMetrics)
                webServerSender.Enqueue(m);

            pendingMetrics.clear();
        };

        asio::steady_timer pushTimer(ioContext);
        std::function<void()> schedulePush;
        schedulePush = [&]()
        {
            pushTimer.expires_after(std::chrono::seconds(config.pushIntervalSeconds));
            pushTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        flushToWebServer();
                        schedulePush();
                    }
                });
        };
        schedulePush();

        // Collector 콘솔에 "send"를 입력하면 즉시 전송. stdin 읽기는 블로킹이라 별도 스레드에서
        // 돌리고, 실제 전송(flushToWebServer)은 asio::post로 io_context 스레드에 넘김 -
        // pendingMetrics/webServerSender를 항상 단일 스레드에서만 건드리게 되어 락이 불필요함.
        std::thread cliThread(
            [&ioContext, &flushToWebServer]()
            {
                String line;
                while (std::getline(std::cin, line))
                {
                    if (line == "send")
                        asio::post(ioContext, flushToWebServer);
                }
            });
        cliThread.detach();

        std::cout << "Collector listening on port " << PORT << " (TLS)" << std::endl;
        std::cout << "[Collector] WebServer(" << config.webServerHost << ":" << config.webServerPort
            << ")로 " << config.pushIntervalSeconds << "초마다 전송 (콘솔에 'send' 입력 시 즉시 전송)" << std::endl;
        ioContext.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Collector] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

수정 후(전체 교체):
```cpp
#include "pch.h"
#include "ApmSession.h"
#include "AesGcmPayload.h"
#include "ResilientSender.h"
#include "PrivilegeDrop.h"
#include "PacketHandler.h"
#include "KeyLoader.h"
#include "CollectorConfig.h"
#include "Protocol/Metric.pb.h"
#include "Storage/MetricStoreFactory.h"
#include <thread>

namespace
{
    constexpr unsigned short PORT = 9000;
    // SQLite: 파일 경로 / TimescaleDB: libpq 연결 문자열("host=... dbname=... user=...")
    constexpr const char* STORAGE_CONNECTION_INFO = "apm_metrics.db";
}

int main()
{
#ifdef _WIN32
    // 소스가 UTF-8(/utf-8)로 컴파일되는데 Windows 콘솔 기본 코드페이지(한글 Windows는 949)는
    // 그와 달라서, std::cout으로 찍는 한글 문자열이 콘솔에서 깨져 보임 - 출력 코드페이지를
    // UTF-8로 맞춰서 해결. Linux는 기본이 UTF-8이라 이 문제 자체가 없음.
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        // Agent<->Collector, Collector<->WebServer 두 구간 모두 AES-256-GCM(AesGcmPayload) -
        // 구간별 키는 분리 유지(한 쪽이 유출돼도 다른 구간은 안전). 배경: Docs/ARIA_TO_AES_MIGRATION.md
        AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");
        AesGcmCipher::Key webServerKey = LoadKeyFromHexFile("certs/webserver_aes.key");

        CollectorConfig config = LoadCollectorConfig("collector_config.json");

        auto store = CreateMetricStore(STORAGE_CONNECTION_INFO, config.metricsRetentionDays);

        // Collector가 받은 뒤 아직 WebServer로 안 보낸 메트릭들 - 주기/CLI 트리거로 비워짐.
        std::vector<apm::Metric> pendingMetrics;

        asio::io_context ioContext;

        // Agent 접속을 받는 서버 역할 컨텍스트(기존)
        asio::ssl::context sslContext(asio::ssl::context::tls_server);
        sslContext.use_certificate_chain_file("certs/server.crt");
        sslContext.use_private_key_file("certs/server.key", asio::ssl::context::pem);

        // WebServer에 접속하는 클라이언트 역할 컨텍스트(신규) - Agent용과 모드가 달라 별도 필요.
        asio::ssl::context webServerSslContext(asio::ssl::context::tls_client);
        // 테스트용 자체 서명 인증서라 CA 검증 생략(Agent->Collector와 동일한 이유, 프로덕션 금지).
        webServerSslContext.set_verify_mode(asio::ssl::verify_none);

        ResilientSender webServerSender(ioContext, webServerSslContext,
            config.webServerHost, config.webServerPort,
            [webServerKey]() { return std::make_unique<AesGcmPayload>(webServerKey); });

        PacketHandler::Register<apm::Metric>(
            [&store, &pendingMetrics](const apm::Metric& pkt)
            {
                store->Store(pkt);
                pendingMetrics.push_back(pkt);
                std::cout << "[Collector] metric stored: cpu=" << pkt.cpu_usage_percent()
                    << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                    << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                    << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                    << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                    << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                    << ",cwnd:" << pkt.tcp_snd_cwnd()
                    << std::endl;
            });

        asio::ip::tcp::acceptor acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), PORT));

        // 포트바인딩 : 특권이 필요할 수 있는 유일한 단계, 완료 직후 권한 하향
        PrivilegeDrop::DropTo("nobody");

        std::function<void()> doAccept;
        doAccept = [&]()
        {
            acceptor.async_accept(
                [&](const asio::error_code &ec, asio::ip::tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::cout << "[Collector] connection accepted" << std::endl;
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::cerr << "[Collector] accept error : " << ec.message() << std::endl;
                    }
                    doAccept();
                });
        };
        doAccept();

        // pendingMetrics에 쌓인 걸 전부 WebServer로 보내고 비움 - 주기 타이머와 CLI 트리거
        // 둘 다 이 함수 하나를 호출함(로직 중복 방지).
        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (pendingMetrics.empty())
                return;

            std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
            for (const auto& m : pendingMetrics)
                webServerSender.Enqueue(m);

            pendingMetrics.clear();
        };

        asio::steady_timer pushTimer(ioContext);
        std::function<void()> schedulePush;
        schedulePush = [&]()
        {
            pushTimer.expires_after(std::chrono::seconds(config.pushIntervalSeconds));
            pushTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        flushToWebServer();
                        schedulePush();
                    }
                });
        };
        schedulePush();

        // 로컬 저장소(store) 보존 정책 - pushTimer와 같은 패턴, 24시간 간격으로
        // 오래된 행 정리(TimescaleMetricStore는 내부적으로 no-op, SqliteMetricStore만 실제
        // DELETE 수행 - IMetricStore::Prune 문서 참고, 2026-07-26 3순위 설계).
        asio::steady_timer pruneTimer(ioContext);
        std::function<void()> schedulePrune;
        schedulePrune = [&]()
        {
            pruneTimer.expires_after(std::chrono::hours(24));
            pruneTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        store->Prune(config.metricsRetentionDays);
                        schedulePrune();
                    }
                });
        };
        schedulePrune();

        // Collector 콘솔에 "send"를 입력하면 즉시 전송. stdin 읽기는 블로킹이라 별도 스레드에서
        // 돌리고, 실제 전송(flushToWebServer)은 asio::post로 io_context 스레드에 넘김 -
        // pendingMetrics/webServerSender를 항상 단일 스레드에서만 건드리게 되어 락이 불필요함.
        std::thread cliThread(
            [&ioContext, &flushToWebServer]()
            {
                String line;
                while (std::getline(std::cin, line))
                {
                    if (line == "send")
                        asio::post(ioContext, flushToWebServer);
                }
            });
        cliThread.detach();

        std::cout << "Collector listening on port " << PORT << " (TLS)" << std::endl;
        std::cout << "[Collector] WebServer(" << config.webServerHost << ":" << config.webServerPort
            << ")로 " << config.pushIntervalSeconds << "초마다 전송 (콘솔에 'send' 입력 시 즉시 전송)" << std::endl;
        ioContext.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Collector] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

**`APM_Agent/collector_config.json`(있다면)** — `metrics_retention_days` 키 추가는 선택 사항(없으면 기본값 30일 그대로 사용, `CollectorConfig.cpp`의 "키가 없으면 기본값 유지" 관례 그대로). 실제 배포 시 파일이 있다면 사용자 쪽에서 `"metrics_retention_days": 30` 한 줄만 추가하면 됨.

### 확인 필요 없음 — 재확인한 기존 결정 그대로 적용

이번 항목은 앞서 사용자가 확정한 4건(적용 범위/정책 방식/기본 보존 기간/AlertRecord 보존 기간)을 그대로 구현한 것 — 추가로 확인받을 판단 지점은 없다고 보고 전체를 한 번에 제안함.

### 결정 사항

- 아직 파일 생성/수정 전 — `CLAUDE.md` 규칙대로 제안 단계(사용자가 이번엔 "설계 착수"만 확인, "적용해줘"는 아직). 사용자가 "적용해줘" 하면 위 파일들(Console 신규 1 + 수정 3, Collector 수정 8: `IMetricStore.h`/`SqliteMetricStore.h`/`SqliteMetricStore.cpp`/`TimescaleMetricStore.h`/`TimescaleMetricStore.cpp`/`MetricStoreFactory.h`/`MetricStoreFactory.cpp`/`CollectorConfig.h`/`CollectorConfig.cpp`/`main.cpp` — 총 9개 수정, 정확히는 `CollectorConfig.h`+`.cpp` 별개 카운트 시 10개)을 전부 실제로 반영 예정.
- 빌드 검증: Console 쪽은 `dotnet build`(사용자 환경), Collector 쪽은 `cmake --build build`(사용자 WSL 환경, TimescaleDB 백엔드는 실제 TimescaleDB 인스턴스 없이는 `add_retention_policy` 런타임 동작까지는 검증 불가 — SQLite 백엔드로 우선 빌드/실행 검증 권장, TimescaleDB 경로는 컴파일만 확인 가능).

## 2026-07-26 — 4순위(함수/트랜잭션 레벨 계측) 설계 제안

### 배경

3순위(데이터 보존 정책) 코드 적용 + Console `dotnet build`/Collector `cmake --build build` 둘 다 빌드 성공 확인 완료. 로드맵 순서를 건너뛰고(1순위 남은 실측 매트릭스, 2순위 `/apm/alerts` 브라우저 검증은 사용자 판단으로 뒤로 미룸 — "5순위 완료 후 넘어가겠다") 4순위로 전환.

착수 전 재확인: 지금 `Agent`는 순수 시스템 리소스 폴러(`MetricScheduler`가 5초마다 CPU/메모리/디스크/TCP만 수집해 `apm::Metric`으로 전송)라, "함수/트랜잭션"을 잴 대상 자체가 프로젝트 안에 없음 — WORK_STATUS.md가 이 항목을 "시스템 리소스 모니터링과 APM의 정체성 갭을 메우는 항목"이라고 표현한 이유. 그래서 계측 SDK 자체와, 그 SDK를 실제로 어디에 붙여 시연할지를 함께 설계해야 함.

**확인 필요 4건 → 사용자 결정**:
1. 계측 대상: **이 프로젝트 자체 코드**(Console(.NET)의 API 핸들러 + Collector(C++)의 패킷 처리 함수) — 별도 데모 앱 신설 없이 "우리 APM으로 우리 자신을 모니터링"
2. 데이터 모델: **개별 span을 그대로 저장**(사전 집계 안 함) — 5순위 백분위 통계가 원본 위에서 정확히 계산되도록
3. 계측 API 형태: **RAII 스코프 기반** — C++은 생성자/소멸자, .NET은 `IAsyncDisposable`로 동일한 발상
4. 대상 언어: **C++ + .NET 둘 다**

### 설계

**핵심 판단 — Console은 이미 자기 자신의 DB를 갖고 있어서 네트워크가 필요 없음**:
- **Collector(C++) 쪽 span**: Collector는 별도 프로세스라 Console로 데이터를 보내려면 네트워크를 거쳐야 함 — 이미 존재하는 `apm::Metric` 전송 파이프라인(`ResilientSender`/`ApmSession`/`PacketHandler`, `Collector<->WebServer` 구간)을 그대로 재사용. `Metric.proto`에 `TransactionSpan` 메시지를 하나 더 추가(기존 메시지 뒤에 이어 붙임 — 아키텍처 제약 그대로 준수)하면 끝. **새 포트/새 리스너 불필요**.
- **Console(.NET) 쪽 span**: Console 프로세스 자신이 이미 `ApmDbContext`(DB)를 갖고 있으므로, 계측한 걸 네트워크로 내보냈다가 자기가 다시 받는 건 낭비 — `TraceScope`(.NET SDK)가 곧바로 `ApmDbContext`에 저장. **네트워크 왕복 자체가 없음**.
- 두 경로 모두 최종적으로 Console의 새 테이블 `TransactionSpans`(`TransactionSpanRecord` 엔티티) 하나로 합쳐짐 — `Source` 컬럼("Collector"/"Console")으로만 구분. 5순위(백분위 통계)는 이 테이블 하나만 보면 됨.

**패킷 ID 디스패치**: 기존 `MetricsReceiverService.ReadOnePacketAsync`는 "메시지 타입이 `Metric` 하나뿐이라 header의 `id`를 무시"하고 있었음(주석에 명시) — 이제 타입이 2개가 되므로 실제로 분기해야 함. C++ 쪽 `PacketType::descriptor()->index()`(.proto 파일 내 선언 순서 기반)와 동일한 값을 C# `Google.Protobuf`의 `MessageDescriptor.Index`가 그대로 제공하므로(둘 다 같은 `.proto`에서 코드 생성 — 언어 무관하게 결정적), `Metric.Descriptor.Index`(=0)/`TransactionSpan.Descriptor.Index`(=1)로 매직 넘버 없이 분기 가능.

**중요 — 빌드 순서 비대칭**: `Metric.proto`를 수정한 뒤,
- **C++ 쪽**: `Metric.pb.h`/`.pb.cc`는 커밋된 Linux protoc 생성본이라(아키텍처 제약 참고) **사용자가 Linux(WSL)에서 `protoc`를 수동으로 재실행해서 재생성 후 커밋**해야 함 — 이 세션은 실행 환경이 아니라 재생성을 대신 못 함.
- **C# 쪽**: `ApmConsole.Domain.Apm.csproj`의 `<Protobuf Include="...Metric.proto" .../>`(Grpc.Tools MSBuild 타겟)가 `dotnet build` 시점에 **자동으로** `Metric.cs`를 재생성함 — 수동 작업 불필요.
- 따라서 적용 순서: `Metric.proto` 수정 → **Linux에서 protoc 재생성 + 커밋** → 그 다음에야 Collector 쪽 코드(`apm::TransactionSpan` 참조)가 컴파일됨. Console 쪽은 `dotnet build` 한 번이면 됨.

**RAII 계측 흐름**:
- C++: `ScopedSpan`(생성자에서 시작 시각 기록, 소멸자에서 경과 시간 계산 후 `SpanRecorder`에 적재) + `APM_TRACE_SCOPE(name)` 매크로. `SpanRecorder`는 프로세스 전역 싱글턴 큐(뮤텍스로 보호 — `cliThread`처럼 다른 스레드에서도 계측이 쓰일 수 있어서 `pendingMetrics`와 달리 락 필요). `Collector/main.cpp`가 기존 `flushToWebServer`(주기 타이머 + "send" CLI 트리거로 이미 호출되던 함수)를 확장해서 `SpanRecorder`에 쌓인 걸 같이 비움 — 새 타이머 불필요, 기존 `pushTimer` 재사용.
- 데모 계측 지점: `PacketHandler::Register<apm::Metric>`의 핸들러 본문(Agent가 보낸 메트릭 패킷 하나를 처리하는 구간) — Collector 안에서 "트랜잭션"이라 부를 만한 몇 안 되는 지점 중 가장 자연스러운 곳.
- .NET: `TraceScope`(`IAsyncDisposable`) — `await using var span = TraceScope.Start(_db, "이름")`. `Dispose` 대신 `DisposeAsync`를 쓴 이유는 저장이 `SaveChangesAsync`(비동기 DB I/O)라 동기 `Dispose`에 억지로 우겨넣지 않기 위함.
- 데모 계측 지점: `AlertsController.Index()` — 이미 있는 실제 액션 핸들러에 한 줄만 추가.
- 실패 표시는 양쪽 다 **자동 감지가 아니라 명시적 opt-in**(`MarkFailed()`) — 예외가 스코프를 빠져나가며 그대로 전파돼도 소멸자/`DisposeAsync`는 반드시 호출되어 span 자체는 기록되지만(측정 누락 없음), `success` 플래그는 호출부가 catch에서 `MarkFailed()`를 불러야 `false`로 남음. 이번 데모 지점(정상 경로만 있는 조회 함수)에서는 실패 케이스를 시연할 게 없어서 자동 예외 후킹까지는 만들지 않음(과설계 방지) — 필요해지면 나중에 호출부에서 `try/catch`로 감싸면 됨.

**3순위(보존 정책)와의 연결**: `TransactionSpans`도 `Metrics`처럼 매 요청마다 쌓이는 고빈도 원본 데이터라 무한정 늘어남 — 새 테이블/새 설정값을 만들지 않고 이미 만들어둔 `RetentionService`가 `_metricsRetentionDays`(기본 30일) 기준으로 같이 정리하도록 확장(재확인받을 새 판단 지점이 아니라 이미 확정된 3순위 정책의 자연스러운 연장).

### 제안 — 프로토콜(공통)

**`APM_Agent/Protocol/Metric.proto`**

수정 전:
```protobuf
syntax = "proto3";
package apm;

message Metric
{
    double cpu_usage_percent = 1;
    uint64 mem_used_bytes = 2;
    uint64 mem_total_bytes = 3;
    uint64 disk_used_bytes = 4;
    uint64 disk_total_bytes = 5;
    uint64 net_rx_bytes_per_sec = 6;
    uint64 net_tx_bytes_per_sec = 7;
    uint32 tcp_rtt_us = 8;
    uint32 tcp_rtt_var_us = 9;
    uint32 tcp_retransmits = 10;
    uint32 tcp_total_retrans = 11;
    uint32 tcp_snd_cwnd = 12;
}
```

수정 후(전체 교체):
```protobuf
syntax = "proto3";
package apm;

message Metric
{
    double cpu_usage_percent = 1;
    uint64 mem_used_bytes = 2;
    uint64 mem_total_bytes = 3;
    uint64 disk_used_bytes = 4;
    uint64 disk_total_bytes = 5;
    uint64 net_rx_bytes_per_sec = 6;
    uint64 net_tx_bytes_per_sec = 7;
    uint32 tcp_rtt_us = 8;
    uint32 tcp_rtt_var_us = 9;
    uint32 tcp_retransmits = 10;
    uint32 tcp_total_retrans = 11;
    uint32 tcp_snd_cwnd = 12;
}

// 함수/트랜잭션 1건 계측 결과(2026-07-26 4순위) - 반드시 Metric 뒤에 이어 붙일 것
// (패킷 ID가 .proto 파일 내 선언 순서로 결정됨 - WORK_STATUS.md 아키텍처 제약 참고).
// Ts는 일부러 안 넣음 - Metric과 동일하게 Console(수신 측)이 도착 시각 기준으로 채움
// (Collector/Agent 쪽 시계 동기화를 신뢰하지 않기 위함, MetricRecord.Ts와 같은 이유).
message TransactionSpan
{
    string operation_name = 1;
    uint64 duration_us = 2;
    bool success = 3;
}
```

### 제안 — Collector(C++) 신규 파일

**`APM_Agent/Common/SpanRecorder.h`** (신규)
```cpp
#pragma once
#include "pch.h"
#include <mutex>
#include <vector>

struct SpanRecord
{
    String operationName;
    uint64 durationUs;
    bool success;
};

/*-------------
    SpanRecorder
---------------*/
// ScopedSpan이 다 끝난 span을 여기 적재만 해두는 프로세스 전역 큐 - 실제 WebServer
// 전송은 Collector/main.cpp의 flushToWebServer가 주기적으로 DrainAll()해서 비움.
// pendingMetrics(main.cpp 지역 변수, io_context 스레드 전용이라 락 불필요)와 달리
// 이건 어떤 스레드에서 계측 매크로가 쓰일지 SDK 입장에서 보장할 수 없어 뮤텍스로 보호
// (지금 당장은 io_context 스레드에서만 쓰지만, 나중에 cliThread 같은 별도 스레드의
// 코드에 계측을 추가해도 안전해야 함 - 2026-07-26 4순위 설계).
class SpanRecorder
{
public:
    static SpanRecorder& Instance();

    void Record(const String& operationName, uint64 durationUs, bool success);

    // 지금까지 쌓인 걸 전부 꺼내고 비움.
    std::vector<SpanRecord> DrainAll();

private:
    std::mutex _mutex;
    std::vector<SpanRecord> _pending;
};
```

**`APM_Agent/Common/SpanRecorder.cpp`** (신규)
```cpp
#include "pch.h"
#include "SpanRecorder.h"

SpanRecorder& SpanRecorder::Instance()
{
    static SpanRecorder instance;
    return instance;
}

void SpanRecorder::Record(const String& operationName, uint64 durationUs, bool success)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _pending.push_back(SpanRecord{ operationName, durationUs, success });
}

std::vector<SpanRecord> SpanRecorder::DrainAll()
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<SpanRecord> result = std::move(_pending);
    _pending.clear();
    return result;
}
```

**`APM_Agent/Common/ScopedSpan.h`** (신규)
```cpp
#pragma once
#include "pch.h"
#include <chrono>

/*------------
    ScopedSpan
--------------*/
// C++ 쪽 계측 SDK - 생성자~소멸자 구간(RAII)을 자동으로 측정해서 SpanRecorder에 적재.
// 스택 언와인딩(예외로 스코프를 빠져나가는 경우) 중에도 소멸자는 반드시 호출되므로
// 측정 자체는 누락되지 않음 - "RAII 스코프 기반" 확정(2026-07-26 4순위 설계).
class ScopedSpan
{
public:
    explicit ScopedSpan(String operationName);
    ~ScopedSpan();

    // 계측 대상 코드가 실패를 명시적으로 표시할 때 호출(기본은 성공으로 간주) -
    // 예외가 던져져도 자동으로 실패 처리되진 않음(과설계 방지, .NET TraceScope.MarkFailed()와 동일 설계).
    void MarkFailed() { _success = false; }

private:
    String _operationName;
    std::chrono::steady_clock::time_point _start;
    bool _success = true;
};

#define APM_CONCAT_INNER(a, b) a##b
#define APM_CONCAT(a, b) APM_CONCAT_INNER(a, b)
// 함수/블록 진입 지점에 이 한 줄만 추가하면 스코프를 빠져나갈 때(정상 반환/예외 무관)
// 자동으로 측정되어 SpanRecorder에 쌓임.
#define APM_TRACE_SCOPE(name) ScopedSpan APM_CONCAT(_apmSpan_, __LINE__)(name)
```

**`APM_Agent/Common/ScopedSpan.cpp`** (신규)
```cpp
#include "pch.h"
#include "ScopedSpan.h"
#include "SpanRecorder.h"

ScopedSpan::ScopedSpan(String operationName)
    : _operationName(std::move(operationName)), _start(std::chrono::steady_clock::now())
{
}

ScopedSpan::~ScopedSpan()
{
    auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - _start).count();

    SpanRecorder::Instance().Record(_operationName, static_cast<uint64>(durationUs), _success);
}
```

### 제안 — Collector(C++) 기존 파일 수정 (수정 전 / 수정 후)

**변경 사유**: `Common/CMakeLists.txt`에 새 소스 2개 추가 필요. `Collector/main.cpp`는 (1) 메트릭 패킷 핸들러에 `APM_TRACE_SCOPE` 계측 삽입, (2) `flushToWebServer`가 `SpanRecorder`도 같이 비워서 `apm::TransactionSpan`으로 전송하도록 확장.

**`APM_Agent/Common/CMakeLists.txt`**

수정 전:
```cmake
add_library(APM_Common STATIC
    ApmSession.cpp
    AriaCipher.cpp
    AesGcmCipher.cpp
    AesGcmPayload.cpp
    HmacUtil.cpp
    SecurePayload.cpp
    KeyLoader.cpp
    PrivilegeDrop.cpp
    ResourceCollector.cpp
    MetricScheduler.cpp
    ResilientSender.cpp
    PacketHandler.cpp
    ../Protocol/Metric.pb.cc
)

target_include_directories(APM_Common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(APM_Common PUBLIC GW2_CrossPlatformCore protobuf::libprotobuf)

if(WIN32)
    # ResourceCollector::ComputeNetworkUsage(GetIfTable2/FreeMibTable)가 필요로 함 -
    # Linux는 /proc/net/dev를 읽어서 별도 링크가 필요 없지만 Windows는 IP Helper API 필요.
    target_link_libraries(APM_Common PUBLIC iphlpapi)
endif()
```

수정 후(전체 교체):
```cmake
add_library(APM_Common STATIC
    ApmSession.cpp
    AriaCipher.cpp
    AesGcmCipher.cpp
    AesGcmPayload.cpp
    HmacUtil.cpp
    SecurePayload.cpp
    KeyLoader.cpp
    PrivilegeDrop.cpp
    ResourceCollector.cpp
    MetricScheduler.cpp
    ResilientSender.cpp
    PacketHandler.cpp
    SpanRecorder.cpp
    ScopedSpan.cpp
    ../Protocol/Metric.pb.cc
)

target_include_directories(APM_Common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(APM_Common PUBLIC GW2_CrossPlatformCore protobuf::libprotobuf)

if(WIN32)
    # ResourceCollector::ComputeNetworkUsage(GetIfTable2/FreeMibTable)가 필요로 함 -
    # Linux는 /proc/net/dev를 읽어서 별도 링크가 필요 없지만 Windows는 IP Helper API 필요.
    target_link_libraries(APM_Common PUBLIC iphlpapi)
endif()
```

**`APM_Agent/Collector/main.cpp`** — 전문(헤더 include 1줄 + 핸들러 계측 1줄 + `flushToWebServer` 확장).

수정 전:
```cpp
#include "pch.h"
#include "ApmSession.h"
#include "AesGcmPayload.h"
#include "ResilientSender.h"
#include "PrivilegeDrop.h"
#include "PacketHandler.h"
#include "KeyLoader.h"
#include "CollectorConfig.h"
#include "Protocol/Metric.pb.h"
#include "Storage/MetricStoreFactory.h"
#include <thread>

namespace
{
    constexpr unsigned short PORT = 9000;
    // SQLite: 파일 경로 / TimescaleDB: libpq 연결 문자열("host=... dbname=... user=...")
    constexpr const char* STORAGE_CONNECTION_INFO = "apm_metrics.db";
}

int main()
{
#ifdef _WIN32
    // 소스가 UTF-8(/utf-8)로 컴파일되는데 Windows 콘솔 기본 코드페이지(한글 Windows는 949)는
    // 그와 달라서, std::cout으로 찍는 한글 문자열이 콘솔에서 깨져 보임 - 출력 코드페이지를
    // UTF-8로 맞춰서 해결. Linux는 기본이 UTF-8이라 이 문제 자체가 없음.
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        // Agent<->Collector, Collector<->WebServer 두 구간 모두 AES-256-GCM(AesGcmPayload) -
        // 구간별 키는 분리 유지(한 쪽이 유출돼도 다른 구간은 안전). 배경: Docs/ARIA_TO_AES_MIGRATION.md
        AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");
        AesGcmCipher::Key webServerKey = LoadKeyFromHexFile("certs/webserver_aes.key");

        CollectorConfig config = LoadCollectorConfig("collector_config.json");

        auto store = CreateMetricStore(STORAGE_CONNECTION_INFO, config.metricsRetentionDays);

        // Collector가 받은 뒤 아직 WebServer로 안 보낸 메트릭들 - 주기/CLI 트리거로 비워짐.
        std::vector<apm::Metric> pendingMetrics;

        asio::io_context ioContext;

        // Agent 접속을 받는 서버 역할 컨텍스트(기존)
        asio::ssl::context sslContext(asio::ssl::context::tls_server);
        sslContext.use_certificate_chain_file("certs/server.crt");
        sslContext.use_private_key_file("certs/server.key", asio::ssl::context::pem);

        // WebServer에 접속하는 클라이언트 역할 컨텍스트(신규) - Agent용과 모드가 달라 별도 필요.
        asio::ssl::context webServerSslContext(asio::ssl::context::tls_client);
        // 테스트용 자체 서명 인증서라 CA 검증 생략(Agent->Collector와 동일한 이유, 프로덕션 금지).
        webServerSslContext.set_verify_mode(asio::ssl::verify_none);

        ResilientSender webServerSender(ioContext, webServerSslContext,
            config.webServerHost, config.webServerPort,
            [webServerKey]() { return std::make_unique<AesGcmPayload>(webServerKey); });

        PacketHandler::Register<apm::Metric>(
            [&store, &pendingMetrics](const apm::Metric& pkt)
            {
                store->Store(pkt);
                pendingMetrics.push_back(pkt);
                std::cout << "[Collector] metric stored: cpu=" << pkt.cpu_usage_percent()
                    << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                    << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                    << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                    << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                    << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                    << ",cwnd:" << pkt.tcp_snd_cwnd()
                    << std::endl;
            });

        asio::ip::tcp::acceptor acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), PORT));

        // 포트바인딩 : 특권이 필요할 수 있는 유일한 단계, 완료 직후 권한 하향
        PrivilegeDrop::DropTo("nobody");

        std::function<void()> doAccept;
        doAccept = [&]()
        {
            acceptor.async_accept(
                [&](const asio::error_code &ec, asio::ip::tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::cout << "[Collector] connection accepted" << std::endl;
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::cerr << "[Collector] accept error : " << ec.message() << std::endl;
                    }
                    doAccept();
                });
        };
        doAccept();

        // pendingMetrics에 쌓인 걸 전부 WebServer로 보내고 비움 - 주기 타이머와 CLI 트리거
        // 둘 다 이 함수 하나를 호출함(로직 중복 방지).
        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (pendingMetrics.empty())
                return;

            std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
            for (const auto& m : pendingMetrics)
                webServerSender.Enqueue(m);

            pendingMetrics.clear();
        };

        asio::steady_timer pushTimer(ioContext);
        std::function<void()> schedulePush;
        schedulePush = [&]()
        {
            pushTimer.expires_after(std::chrono::seconds(config.pushIntervalSeconds));
            pushTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        flushToWebServer();
                        schedulePush();
                    }
                });
        };
        schedulePush();

        // 로컬 저장소(store) 보존 정책 - pushTimer와 같은 패턴, 24시간 간격으로
        // 오래된 행 정리(TimescaleMetricStore는 내부적으로 no-op, SqliteMetricStore만 실제
        // DELETE 수행 - IMetricStore::Prune 문서 참고, 2026-07-26 3순위 설계).
        asio::steady_timer pruneTimer(ioContext);
        std::function<void()> schedulePrune;
        schedulePrune = [&]()
        {
            pruneTimer.expires_after(std::chrono::hours(24));
            pruneTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        store->Prune(config.metricsRetentionDays);
                        schedulePrune();
                    }
                });
        };
        schedulePrune();

        // Collector 콘솔에 "send"를 입력하면 즉시 전송. stdin 읽기는 블로킹이라 별도 스레드에서
        // 돌리고, 실제 전송(flushToWebServer)은 asio::post로 io_context 스레드에 넘김 -
        // pendingMetrics/webServerSender를 항상 단일 스레드에서만 건드리게 되어 락이 불필요함.
        std::thread cliThread(
            [&ioContext, &flushToWebServer]()
            {
                String line;
                while (std::getline(std::cin, line))
                {
                    if (line == "send")
                        asio::post(ioContext, flushToWebServer);
                }
            });
        cliThread.detach();

        std::cout << "Collector listening on port " << PORT << " (TLS)" << std::endl;
        std::cout << "[Collector] WebServer(" << config.webServerHost << ":" << config.webServerPort
            << ")로 " << config.pushIntervalSeconds << "초마다 전송 (콘솔에 'send' 입력 시 즉시 전송)" << std::endl;
        ioContext.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Collector] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

수정 후(전체 교체):
```cpp
#include "pch.h"
#include "ApmSession.h"
#include "AesGcmPayload.h"
#include "ResilientSender.h"
#include "PrivilegeDrop.h"
#include "PacketHandler.h"
#include "KeyLoader.h"
#include "CollectorConfig.h"
#include "ScopedSpan.h"
#include "SpanRecorder.h"
#include "Protocol/Metric.pb.h"
#include "Storage/MetricStoreFactory.h"
#include <thread>

namespace
{
    constexpr unsigned short PORT = 9000;
    // SQLite: 파일 경로 / TimescaleDB: libpq 연결 문자열("host=... dbname=... user=...")
    constexpr const char* STORAGE_CONNECTION_INFO = "apm_metrics.db";
}

int main()
{
#ifdef _WIN32
    // 소스가 UTF-8(/utf-8)로 컴파일되는데 Windows 콘솔 기본 코드페이지(한글 Windows는 949)는
    // 그와 달라서, std::cout으로 찍는 한글 문자열이 콘솔에서 깨져 보임 - 출력 코드페이지를
    // UTF-8로 맞춰서 해결. Linux는 기본이 UTF-8이라 이 문제 자체가 없음.
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        // Agent<->Collector, Collector<->WebServer 두 구간 모두 AES-256-GCM(AesGcmPayload) -
        // 구간별 키는 분리 유지(한 쪽이 유출돼도 다른 구간은 안전). 배경: Docs/ARIA_TO_AES_MIGRATION.md
        AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");
        AesGcmCipher::Key webServerKey = LoadKeyFromHexFile("certs/webserver_aes.key");

        CollectorConfig config = LoadCollectorConfig("collector_config.json");

        auto store = CreateMetricStore(STORAGE_CONNECTION_INFO, config.metricsRetentionDays);

        // Collector가 받은 뒤 아직 WebServer로 안 보낸 메트릭들 - 주기/CLI 트리거로 비워짐.
        std::vector<apm::Metric> pendingMetrics;

        asio::io_context ioContext;

        // Agent 접속을 받는 서버 역할 컨텍스트(기존)
        asio::ssl::context sslContext(asio::ssl::context::tls_server);
        sslContext.use_certificate_chain_file("certs/server.crt");
        sslContext.use_private_key_file("certs/server.key", asio::ssl::context::pem);

        // WebServer에 접속하는 클라이언트 역할 컨텍스트(신규) - Agent용과 모드가 달라 별도 필요.
        asio::ssl::context webServerSslContext(asio::ssl::context::tls_client);
        // 테스트용 자체 서명 인증서라 CA 검증 생략(Agent->Collector와 동일한 이유, 프로덕션 금지).
        webServerSslContext.set_verify_mode(asio::ssl::verify_none);

        ResilientSender webServerSender(ioContext, webServerSslContext,
            config.webServerHost, config.webServerPort,
            [webServerKey]() { return std::make_unique<AesGcmPayload>(webServerKey); });

        PacketHandler::Register<apm::Metric>(
            [&store, &pendingMetrics](const apm::Metric& pkt)
            {
                // Collector 안에서 "트랜잭션"이라 부를 만한 지점 중 가장 자연스러운 곳 -
                // Agent가 보낸 메트릭 패킷 하나를 받아 저장하는 구간(2026-07-26 4순위 데모 계측).
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                store->Store(pkt);
                pendingMetrics.push_back(pkt);
                std::cout << "[Collector] metric stored: cpu=" << pkt.cpu_usage_percent()
                    << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                    << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                    << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                    << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                    << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                    << ",cwnd:" << pkt.tcp_snd_cwnd()
                    << std::endl;
            });

        asio::ip::tcp::acceptor acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), PORT));

        // 포트바인딩 : 특권이 필요할 수 있는 유일한 단계, 완료 직후 권한 하향
        PrivilegeDrop::DropTo("nobody");

        std::function<void()> doAccept;
        doAccept = [&]()
        {
            acceptor.async_accept(
                [&](const asio::error_code &ec, asio::ip::tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::cout << "[Collector] connection accepted" << std::endl;
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::cerr << "[Collector] accept error : " << ec.message() << std::endl;
                    }
                    doAccept();
                });
        };
        doAccept();

        // pendingMetrics/SpanRecorder에 쌓인 걸 전부 WebServer로 보내고 비움 - 주기 타이머와
        // CLI 트리거 둘 다 이 함수 하나를 호출함(로직 중복 방지). span 전송을 여기 얹은 이유:
        // 이미 "주기적으로 WebServer에 밀어넣는" 책임을 지고 있는 함수라 새 타이머를 또
        // 만들 필요가 없음(2026-07-26 4순위 설계).
        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (!pendingMetrics.empty())
            {
                std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
                for (const auto& m : pendingMetrics)
                    webServerSender.Enqueue(m);

                pendingMetrics.clear();
            }

            auto spans = SpanRecorder::Instance().DrainAll();
            if (!spans.empty())
            {
                std::cout << "[Collector] WebServer로 span " << spans.size() << "건 전송 시도" << std::endl;
                for (const auto& s : spans)
                {
                    apm::TransactionSpan pkt;
                    pkt.set_operation_name(s.operationName);
                    pkt.set_duration_us(s.durationUs);
                    pkt.set_success(s.success);
                    webServerSender.Enqueue(pkt);
                }
            }
        };

        asio::steady_timer pushTimer(ioContext);
        std::function<void()> schedulePush;
        schedulePush = [&]()
        {
            pushTimer.expires_after(std::chrono::seconds(config.pushIntervalSeconds));
            pushTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        flushToWebServer();
                        schedulePush();
                    }
                });
        };
        schedulePush();

        // 로컬 저장소(store) 보존 정책 - pushTimer와 같은 패턴, 24시간 간격으로
        // 오래된 행 정리(TimescaleMetricStore는 내부적으로 no-op, SqliteMetricStore만 실제
        // DELETE 수행 - IMetricStore::Prune 문서 참고, 2026-07-26 3순위 설계).
        asio::steady_timer pruneTimer(ioContext);
        std::function<void()> schedulePrune;
        schedulePrune = [&]()
        {
            pruneTimer.expires_after(std::chrono::hours(24));
            pruneTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        store->Prune(config.metricsRetentionDays);
                        schedulePrune();
                    }
                });
        };
        schedulePrune();

        // Collector 콘솔에 "send"를 입력하면 즉시 전송. stdin 읽기는 블로킹이라 별도 스레드에서
        // 돌리고, 실제 전송(flushToWebServer)은 asio::post로 io_context 스레드에 넘김 -
        // pendingMetrics/webServerSender를 항상 단일 스레드에서만 건드리게 되어 락이 불필요함.
        std::thread cliThread(
            [&ioContext, &flushToWebServer]()
            {
                String line;
                while (std::getline(std::cin, line))
                {
                    if (line == "send")
                        asio::post(ioContext, flushToWebServer);
                }
            });
        cliThread.detach();

        std::cout << "Collector listening on port " << PORT << " (TLS)" << std::endl;
        std::cout << "[Collector] WebServer(" << config.webServerHost << ":" << config.webServerPort
            << ")로 " << config.pushIntervalSeconds << "초마다 전송 (콘솔에 'send' 입력 시 즉시 전송)" << std::endl;
        ioContext.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Collector] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

### 제안 — Console(.NET) 신규 파일

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Infrastructure/Persistence/TransactionSpanRecord.cs`** (신규)
```csharp
namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

// 함수/트랜잭션 1건 계측 결과 - Collector(C++, 네트워크로 수신)와 Console(.NET, 같은 프로세스라
// 직접 저장) 양쪽에서 다 이 테이블로 모임. Source로만 출처를 구분(2026-07-26 4순위 설계).
public class TransactionSpanRecord
{
    public int Id { get; set; }
    public DateTimeOffset Ts { get; set; }
    public string Source { get; set; } = "";          // "Collector" | "Console"
    public string OperationName { get; set; } = "";
    public long DurationUs { get; set; }
    public bool Success { get; set; }
}
```

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Infrastructure/TraceScope.cs`** (신규)
```csharp
using System.Diagnostics;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;

namespace ApmConsole.Domain.Apm.Infrastructure;

/*-----------
    TraceScope
-------------*/
// .NET 쪽 계측 SDK - IAsyncDisposable로 진입~탈출 구간을 자동 측정해서 TransactionSpanRecord로
// 직접 저장. Collector(C++)와 달리 네트워크를 거칠 필요가 없음 - Console 프로세스가 이미
// ApmDbContext(DB)를 갖고 있으므로 곧장 씀(2026-07-26 4순위 설계). Dispose가 아니라
// DisposeAsync를 쓴 이유: 저장이 SaveChangesAsync(비동기 DB I/O)라서.
public sealed class TraceScope : IAsyncDisposable
{
    private readonly ApmDbContext _db;
    private readonly string _operationName;
    private readonly Stopwatch _stopwatch;
    private bool _success = true;

    private TraceScope(ApmDbContext db, string operationName)
    {
        _db = db;
        _operationName = operationName;
        _stopwatch = Stopwatch.StartNew();
    }

    public static TraceScope Start(ApmDbContext db, string operationName) => new(db, operationName);

    // 계측 대상 코드가 실패를 명시적으로 표시할 때 호출(기본은 성공으로 간주) - 예외가
    // 스코프를 빠져나가도 자동으로 실패 처리되진 않음(과설계 방지, ScopedSpan.MarkFailed()와 동일 설계).
    public void MarkFailed() => _success = false;

    public async ValueTask DisposeAsync()
    {
        _stopwatch.Stop();

        var record = new TransactionSpanRecord
        {
            Ts = DateTimeOffset.UtcNow,
            Source = "Console",
            OperationName = _operationName,
            DurationUs = _stopwatch.ElapsedTicks * 1_000_000 / Stopwatch.Frequency,
            Success = _success,
        };

        _db.TransactionSpans.Add(record);
        await _db.SaveChangesAsync();
    }
}
```

### 제안 — Console(.NET) 기존 파일 수정 (수정 전 / 수정 후)

**변경 사유**: `ApmDbContext`에 `TransactionSpans` 테이블 추가. `MetricsReceiverService`는 패킷 id로 `Metric`/`TransactionSpan`을 분기해서 처리해야 함(기존엔 id를 아예 무시). `AlertsController.Index()`에 데모 계측 삽입. `RetentionService`가 새 테이블도 같이 정리.

**`ApmDbContext.cs`**

수정 전(3순위 적용 후 현재 디스크 상태):
```csharp
using Microsoft.EntityFrameworkCore;

namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

/*---------------
    ApmDbContext
-----------------*/
// WebServer 자체 소유의 메트릭 저장소(SQLite/TimescaleDB 선택 가능) - Collector가 네트워크로
// 보내주는 데이터를 MetricsReceiverService가 이 컨텍스트를 통해 씀. 예전에는 Collector의
// SQLite 파일을 직접 읽기만 했지만(같은 장비 전제), 이제 완전히 독립된 저장소라 스키마도
// EF Core 기본 관례를 그대로 씀(수동 컬럼명 매핑/keyless 설정 불필요).

public class ApmDbContext : DbContext
{
    public ApmDbContext(DbContextOptions<ApmDbContext> options) : base(options) { }

    public DbSet<MetricRecord> Metrics => Set<MetricRecord>();
    public DbSet<AlertThreshold> AlertThresholds => Set<AlertThreshold>();
    public DbSet<AlertRecord> AlertRecords => Set<AlertRecord>();

    // EnsureCreated()가 스키마를 처음 만들 때 이 시드 데이터도 같이 넣어줌(마이그레이션 없이도
    // 동작 - MetricsReceiverService가 EnsureCreated()를 쓰는 기존 방식 그대로 유지).
    // CPU/메모리/디스크는 90%, RTT는 200ms(200,000us) - 로컬 테스트 환경 기준 보수적인 기본값.
    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        modelBuilder.Entity<AlertThreshold>().HasData(
            new AlertThreshold { Id = 1, MetricType = AlertMetricType.CpuPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 2, MetricType = AlertMetricType.MemoryPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 3, MetricType = AlertMetricType.DiskPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 4, MetricType = AlertMetricType.TcpRttUs, Value = 200_000, Enabled = true }
        );

        // RetentionService가 매시간 Ts/ClosedAt 기준으로 WHERE 삭제를 돌리므로 인덱스가 없으면
        // 테이블이 커질수록 이 삭제 자체가 풀스캔이 됨(2026-07-26 3순위 설계) - AlertRecords.ClosedAt
        // 인덱스는 EvaluateAlertsAsync/AlertsController의 "현재 열린 알림" 조회에도 같이 도움됨.
        // 주의: EnsureCreated()는 신규 DB 파일에만 이 인덱스를 만듦 - 이미 존재하는 apm_metrics.db/
        // webserver_apm.db 파일에는 소급 적용 안 됨(마이그레이션 시스템이 아니라 EnsureCreated라
        // 스키마 변경이 자동 반영되지 않음 - 기존 파일은 수동 CREATE INDEX 또는 파일 재생성 필요).
        modelBuilder.Entity<MetricRecord>().HasIndex(m => m.Ts);
        modelBuilder.Entity<AlertRecord>().HasIndex(a => a.ClosedAt);
    }
}
```

수정 후(전체 교체):
```csharp
using Microsoft.EntityFrameworkCore;

namespace ApmConsole.Domain.Apm.Infrastructure.Persistence;

/*---------------
    ApmDbContext
-----------------*/
// WebServer 자체 소유의 메트릭 저장소(SQLite/TimescaleDB 선택 가능) - Collector가 네트워크로
// 보내주는 데이터를 MetricsReceiverService가 이 컨텍스트를 통해 씀. 예전에는 Collector의
// SQLite 파일을 직접 읽기만 했지만(같은 장비 전제), 이제 완전히 독립된 저장소라 스키마도
// EF Core 기본 관례를 그대로 씀(수동 컬럼명 매핑/keyless 설정 불필요).

public class ApmDbContext : DbContext
{
    public ApmDbContext(DbContextOptions<ApmDbContext> options) : base(options) { }

    public DbSet<MetricRecord> Metrics => Set<MetricRecord>();
    public DbSet<AlertThreshold> AlertThresholds => Set<AlertThreshold>();
    public DbSet<AlertRecord> AlertRecords => Set<AlertRecord>();
    public DbSet<TransactionSpanRecord> TransactionSpans => Set<TransactionSpanRecord>();

    // EnsureCreated()가 스키마를 처음 만들 때 이 시드 데이터도 같이 넣어줌(마이그레이션 없이도
    // 동작 - MetricsReceiverService가 EnsureCreated()를 쓰는 기존 방식 그대로 유지).
    // CPU/메모리/디스크는 90%, RTT는 200ms(200,000us) - 로컬 테스트 환경 기준 보수적인 기본값.
    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        modelBuilder.Entity<AlertThreshold>().HasData(
            new AlertThreshold { Id = 1, MetricType = AlertMetricType.CpuPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 2, MetricType = AlertMetricType.MemoryPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 3, MetricType = AlertMetricType.DiskPercent, Value = 90, Enabled = true },
            new AlertThreshold { Id = 4, MetricType = AlertMetricType.TcpRttUs, Value = 200_000, Enabled = true }
        );

        // RetentionService가 매시간 Ts/ClosedAt 기준으로 WHERE 삭제를 돌리므로 인덱스가 없으면
        // 테이블이 커질수록 이 삭제 자체가 풀스캔이 됨(2026-07-26 3순위 설계) - AlertRecords.ClosedAt
        // 인덱스는 EvaluateAlertsAsync/AlertsController의 "현재 열린 알림" 조회에도 같이 도움됨.
        // 주의: EnsureCreated()는 신규 DB 파일에만 이 인덱스를 만듦 - 이미 존재하는 apm_metrics.db/
        // webserver_apm.db 파일에는 소급 적용 안 됨(마이그레이션 시스템이 아니라 EnsureCreated라
        // 스키마 변경이 자동 반영되지 않음 - 기존 파일은 수동 CREATE INDEX 또는 파일 재생성 필요).
        modelBuilder.Entity<MetricRecord>().HasIndex(m => m.Ts);
        modelBuilder.Entity<AlertRecord>().HasIndex(a => a.ClosedAt);

        // 5순위(백분위 통계)가 "특정 OperationName의 최근 N분 구간" 같은 쿼리를 돌릴 걸 감안한
        // 복합 인덱스 - RetentionService의 Ts 기준 삭제에도 같이 도움됨(2026-07-26 4순위 설계).
        modelBuilder.Entity<TransactionSpanRecord>().HasIndex(s => new { s.OperationName, s.Ts });
    }
}
```

**`MetricsReceiverService.cs`** — 전문(패킷 id 분기 추가가 `HandleClientAsync`/`ReadOnePacketAsync`를 관통해서 구조가 바뀌므로 전체 교체로 제시).

수정 전(3순위 적용 후 현재 디스크 상태):
```csharp
using System.Buffers.Binary;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Apm;
using ApmConsole.Domain.Apm.Hubs;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using Microsoft.AspNetCore.SignalR;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;

namespace ApmConsole.Domain.Apm.Infrastructure;

/*------------------------
    MetricsReceiverService
--------------------------*/
// Collector가 주기적으로(또는 CLI 트리거로) 보내는 메트릭을 받는 TLS 리스너.
// PacketHeader{size,id} 프레이밍 + AES-256-GCM 복호화 + Protobuf 역직렬화를 직접 구현 -
// C++ Collector가 쓰는 것과 같은 와이어 포맷(APM_Agent의 ApmSession.cpp/AesGcmPayload.cpp
// 참고)을 그대로 맞춰야 함. 저장 직후 SignalR로 즉시 푸시(폴링 없음).

public class MetricsReceiverService : BackgroundService
{
    internal const int NonceSize = 12;
    internal const int TagSize = 16;

    private readonly int _port;
    private readonly byte[] _aesKey;
    private readonly X509Certificate2 _serverCert;
    private readonly IServiceScopeFactory _scopeFactory;
    private readonly IHubContext<MetricsHub> _hubContext;

    public MetricsReceiverService(IConfiguration configuration, IServiceScopeFactory scopeFactory, IHubContext<MetricsHub> hubContext)
    {
        _port = int.Parse(configuration["Apm:ReceiverPort"] ?? "9100");

        var keyPath = configuration["Apm:WebServerAesKeyPath"]
            ?? throw new InvalidOperationException("Apm:WebServerAesKeyPath 설정이 필요합니다.");
        _aesKey = Convert.FromHexString(File.ReadAllText(keyPath).Trim());

        var certPath = configuration["Apm:ReceiverCertPath"]
            ?? throw new InvalidOperationException("Apm:ReceiverCertPath 설정이 필요합니다.");
        var certKeyPath = configuration["Apm:ReceiverKeyPath"]
            ?? throw new InvalidOperationException("Apm:ReceiverKeyPath 설정이 필요합니다.");
        _serverCert = X509Certificate2.CreateFromPemFile(certPath, certKeyPath);

        _scopeFactory = scopeFactory;
        _hubContext = hubContext;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using (var scope = _scopeFactory.CreateScope())
        {
            var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();
            db.Database.EnsureCreated();
        }

        var listener = new TcpListener(IPAddress.Any, _port);
        listener.Start();
        Console.WriteLine($"[MetricsReceiverService] {_port}번 포트에서 Collector 연결 대기 중 (TLS)");

        try
        {
            while (!stoppingToken.IsCancellationRequested)
            {
                var client = await listener.AcceptTcpClientAsync(stoppingToken);
                _ = HandleClientAsync(client, stoppingToken);
            }
        }
        finally
        {
            listener.Stop();
        }
    }

    private async Task HandleClientAsync(TcpClient client, CancellationToken stoppingToken)
    {
        using (client)
        using (var sslStream = new SslStream(client.GetStream(), leaveInnerStreamOpen: false))
        {
            try
            {
                await sslStream.AuthenticateAsServerAsync(_serverCert, clientCertificateRequired: false,
                    checkCertificateRevocation: false);

                Console.WriteLine("[MetricsReceiverService] Collector 연결됨");

                while (!stoppingToken.IsCancellationRequested)
                {
                    var metric = await ReadOnePacketAsync(sslStream, stoppingToken);
                    if (metric == null)
                        break;

                    await StoreAndBroadcastAsync(metric);
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[MetricsReceiverService] 연결 처리 중 오류: {ex.Message}");
            }
        }
    }

    // internal(private 아님) - ReadExactAsync/DecryptAndParse는 순수 로직이라 테스트 대상으로 삼음
    // (ApmConsole.Domain.Apm.Tests에 InternalsVisibleTo로 접근 허용, AssemblyInfo.cs 참고).
    internal static async Task<byte[]?> ReadExactAsync(Stream stream, int length, CancellationToken ct)
    {
        var buffer = new byte[length];
        var offset = 0;
        while (offset < length)
        {
            var read = await stream.ReadAsync(buffer.AsMemory(offset, length - offset), ct);
            if (read == 0)
                return null;   // 연결 종료
            offset += read;
        }
        return buffer;
    }

    private async Task<Metric?> ReadOnePacketAsync(Stream stream, CancellationToken ct)
    {
        // PacketHeader{ uint16 size; uint16 id; } - C++와 동일하게 4바이트, 리틀엔디안.
        var header = await ReadExactAsync(stream, 4, ct);
        if (header == null)
            return null;

        ushort totalSize = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(0, 2));
        // id(header[2..4])는 지금 메시지 타입이 Metric 하나뿐이라 별도 분기 없이 무시.

        var sealedPayload = await ReadExactAsync(stream, totalSize - 4, ct);
        if (sealedPayload == null)
            return null;

        return DecryptAndParse(sealedPayload, _aesKey);
    }

    // Span<T>(ref struct)를 async 메서드 안에서 지역 변수로 두면 .NET 8/C# 12 기준
    // "ref and unsafe in async and iterator methods"가 preview 전용이라 컴파일 에러(CS8652) -
    // 이 로직만 별도의 동기 메서드로 분리해서 async 메서드 밖에 둠(preview 언어 기능 의존 회피).
    // static + aesKey를 파라미터로 받도록 함 - _aesKey(인스턴스 필드, 생성자가 파일 I/O로 채움) 대신
    // 순수 입력만으로 테스트할 수 있게(서비스 전체를 DI로 구성하지 않아도 됨).
    internal static Metric DecryptAndParse(byte[] sealedPayload, byte[] aesKey)
    {
        // AesGcmPayload::Seal()의 와이어 포맷: [Nonce(12B)][ciphertext(가변)][Tag(16B)]
        var nonce = sealedPayload.AsSpan(0, NonceSize);
        var tag = sealedPayload.AsSpan(sealedPayload.Length - TagSize, TagSize);
        var ciphertext = sealedPayload.AsSpan(NonceSize, sealedPayload.Length - NonceSize - TagSize);

        var plaintext = new byte[ciphertext.Length];
        using var aesGcm = new AesGcm(aesKey, TagSize);
        aesGcm.Decrypt(nonce, ciphertext, tag, plaintext);

        return Metric.Parser.ParseFrom(plaintext);
    }

    private async Task StoreAndBroadcastAsync(Metric metric)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        var record = new MetricRecord
        {
            Ts = DateTimeOffset.UtcNow,
            CpuUsagePercent = metric.CpuUsagePercent,
            MemUsedBytes = (long)metric.MemUsedBytes,
            MemTotalBytes = (long)metric.MemTotalBytes,
            DiskUsedBytes = (long)metric.DiskUsedBytes,
            DiskTotalBytes = (long)metric.DiskTotalBytes,
            NetRxBytesPerSec = (long)metric.NetRxBytesPerSec,
            NetTxBytesPerSec = (long)metric.NetTxBytesPerSec,
            TcpRttUs = (int)metric.TcpRttUs,
            TcpRttVarUs = (int)metric.TcpRttVarUs,
            TcpRetransmits = (int)metric.TcpRetransmits,
            TcpTotalRetrans = (int)metric.TcpTotalRetrans,
            TcpSndCwnd = (int)metric.TcpSndCwnd,
        };

        db.Metrics.Add(record);
        await db.SaveChangesAsync();

        Console.WriteLine($"[MetricsReceiverService] 저장 완료: cpu={record.CpuUsagePercent}%");

        await _hubContext.Clients.All.SendAsync("NewMetric", record);

        await EvaluateAlertsAsync(db, record);
    }

    // 저장된 메트릭 1건에 대해 활성화된 임계치를 전부 평가 - 상태 전이(Opened/Resolved)가
    // 있을 때만 AlertRecord를 기록하고 SignalR로 알림(2026-07-26, WORK_STATUS.md 2순위).
    private async Task EvaluateAlertsAsync(ApmDbContext db, MetricRecord record)
    {
        var memPercent = record.MemTotalBytes > 0 ? record.MemUsedBytes * 100.0 / record.MemTotalBytes : 0;
        var diskPercent = record.DiskTotalBytes > 0 ? record.DiskUsedBytes * 100.0 / record.DiskTotalBytes : 0;

        var currentValues = new Dictionary<AlertMetricType, double>
        {
            [AlertMetricType.CpuPercent] = record.CpuUsagePercent,
            [AlertMetricType.MemoryPercent] = memPercent,
            [AlertMetricType.DiskPercent] = diskPercent,
            [AlertMetricType.TcpRttUs] = record.TcpRttUs,
        };

        var thresholds = await db.AlertThresholds.AsNoTracking().Where(t => t.Enabled).ToListAsync();

        foreach (var threshold in thresholds)
        {
            var currentValue = currentValues[threshold.MetricType];

            var openAlert = await db.AlertRecords
                .Where(a => a.MetricType == threshold.MetricType && a.ClosedAt == null)
                .OrderByDescending(a => a.Id)
                .FirstOrDefaultAsync();

            var transition = AlertEvaluator.Evaluate(currentValue, threshold.Value, openAlert != null);

            if (transition == AlertTransition.Opened)
            {
                var opened = new AlertRecord
                {
                    MetricType = threshold.MetricType,
                    ThresholdValue = threshold.Value,
                    TriggerValue = currentValue,
                    OpenedAt = DateTimeOffset.UtcNow,
                };
                db.AlertRecords.Add(opened);
                await db.SaveChangesAsync();

                Console.WriteLine($"[MetricsReceiverService] 알림 발생: {threshold.MetricType}={currentValue:F1} (임계치 {threshold.Value})");
                await _hubContext.Clients.All.SendAsync("AlertOpened", opened);
            }
            else if (transition == AlertTransition.Resolved && openAlert != null)
            {
                openAlert.ResolvedValue = currentValue;
                openAlert.ClosedAt = DateTimeOffset.UtcNow;
                await db.SaveChangesAsync();

                Console.WriteLine($"[MetricsReceiverService] 알림 해제: {threshold.MetricType}={currentValue:F1}");
                await _hubContext.Clients.All.SendAsync("AlertResolved", openAlert);
            }
        }
    }
}
```

수정 후(전체 교체):
```csharp
using System.Buffers.Binary;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Apm;
using ApmConsole.Domain.Apm.Hubs;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using Microsoft.AspNetCore.SignalR;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;

namespace ApmConsole.Domain.Apm.Infrastructure;

/*------------------------
    MetricsReceiverService
--------------------------*/
// Collector가 주기적으로(또는 CLI 트리거로) 보내는 메트릭/트랜잭션 span을 받는 TLS 리스너.
// PacketHeader{size,id} 프레이밍 + AES-256-GCM 복호화 + Protobuf 역직렬화를 직접 구현 -
// C++ Collector가 쓰는 것과 같은 와이어 포맷(APM_Agent의 ApmSession.cpp/AesGcmPayload.cpp
// 참고)을 그대로 맞춰야 함. 저장 직후 SignalR로 즉시 푸시(폴링 없음).
// id 분기(Metric.Descriptor.Index/TransactionSpan.Descriptor.Index)는 C++ 쪽
// PacketType::descriptor()->index()와 동일한 규칙(.proto 선언 순서) - 2026-07-26 4순위.

public class MetricsReceiverService : BackgroundService
{
    internal const int NonceSize = 12;
    internal const int TagSize = 16;

    private readonly int _port;
    private readonly byte[] _aesKey;
    private readonly X509Certificate2 _serverCert;
    private readonly IServiceScopeFactory _scopeFactory;
    private readonly IHubContext<MetricsHub> _hubContext;

    public MetricsReceiverService(IConfiguration configuration, IServiceScopeFactory scopeFactory, IHubContext<MetricsHub> hubContext)
    {
        _port = int.Parse(configuration["Apm:ReceiverPort"] ?? "9100");

        var keyPath = configuration["Apm:WebServerAesKeyPath"]
            ?? throw new InvalidOperationException("Apm:WebServerAesKeyPath 설정이 필요합니다.");
        _aesKey = Convert.FromHexString(File.ReadAllText(keyPath).Trim());

        var certPath = configuration["Apm:ReceiverCertPath"]
            ?? throw new InvalidOperationException("Apm:ReceiverCertPath 설정이 필요합니다.");
        var certKeyPath = configuration["Apm:ReceiverKeyPath"]
            ?? throw new InvalidOperationException("Apm:ReceiverKeyPath 설정이 필요합니다.");
        _serverCert = X509Certificate2.CreateFromPemFile(certPath, certKeyPath);

        _scopeFactory = scopeFactory;
        _hubContext = hubContext;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using (var scope = _scopeFactory.CreateScope())
        {
            var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();
            db.Database.EnsureCreated();
        }

        var listener = new TcpListener(IPAddress.Any, _port);
        listener.Start();
        Console.WriteLine($"[MetricsReceiverService] {_port}번 포트에서 Collector 연결 대기 중 (TLS)");

        try
        {
            while (!stoppingToken.IsCancellationRequested)
            {
                var client = await listener.AcceptTcpClientAsync(stoppingToken);
                _ = HandleClientAsync(client, stoppingToken);
            }
        }
        finally
        {
            listener.Stop();
        }
    }

    private async Task HandleClientAsync(TcpClient client, CancellationToken stoppingToken)
    {
        using (client)
        using (var sslStream = new SslStream(client.GetStream(), leaveInnerStreamOpen: false))
        {
            try
            {
                await sslStream.AuthenticateAsServerAsync(_serverCert, clientCertificateRequired: false,
                    checkCertificateRevocation: false);

                Console.WriteLine("[MetricsReceiverService] Collector 연결됨");

                while (!stoppingToken.IsCancellationRequested)
                {
                    var packet = await ReadOnePacketAsync(sslStream, stoppingToken);
                    if (packet == null)
                        break;

                    var (id, sealedPayload) = packet.Value;

                    if (id == (ushort)Metric.Descriptor.Index)
                        await StoreAndBroadcastAsync(DecryptAndParse(sealedPayload, _aesKey));
                    else if (id == (ushort)TransactionSpan.Descriptor.Index)
                        await StoreSpanAsync(DecryptAndParseSpan(sealedPayload, _aesKey));
                    else
                        Console.WriteLine($"[MetricsReceiverService] 알 수 없는 패킷 id={id}, 무시");
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[MetricsReceiverService] 연결 처리 중 오류: {ex.Message}");
            }
        }
    }

    // internal(private 아님) - ReadExactAsync/DecryptAndParse는 순수 로직이라 테스트 대상으로 삼음
    // (ApmConsole.Domain.Apm.Tests에 InternalsVisibleTo로 접근 허용, AssemblyInfo.cs 참고).
    internal static async Task<byte[]?> ReadExactAsync(Stream stream, int length, CancellationToken ct)
    {
        var buffer = new byte[length];
        var offset = 0;
        while (offset < length)
        {
            var read = await stream.ReadAsync(buffer.AsMemory(offset, length - offset), ct);
            if (read == 0)
                return null;   // 연결 종료
            offset += read;
        }
        return buffer;
    }

    // 헤더만 읽고 id/암호화된 payload를 그대로 반환 - 어느 메시지 타입인지는 호출자
    // (HandleClientAsync)가 id로 분기해서 결정(2026-07-26 4순위, 메시지 타입이 2개가 되면서
    // 예전처럼 "무조건 Metric으로 파싱"할 수 없게 됨).
    private async Task<(ushort Id, byte[] SealedPayload)?> ReadOnePacketAsync(Stream stream, CancellationToken ct)
    {
        // PacketHeader{ uint16 size; uint16 id; } - C++와 동일하게 4바이트, 리틀엔디안.
        var header = await ReadExactAsync(stream, 4, ct);
        if (header == null)
            return null;

        ushort totalSize = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(0, 2));
        ushort id = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(2, 2));

        var sealedPayload = await ReadExactAsync(stream, totalSize - 4, ct);
        if (sealedPayload == null)
            return null;

        return (id, sealedPayload);
    }

    // Span<T>(ref struct)를 async 메서드 안에서 지역 변수로 두면 .NET 8/C# 12 기준
    // "ref and unsafe in async and iterator methods"가 preview 전용이라 컴파일 에러(CS8652) -
    // 이 로직만 별도의 동기 메서드로 분리해서 async 메서드 밖에 둠(preview 언어 기능 의존 회피).
    // static + aesKey를 파라미터로 받도록 함 - _aesKey(인스턴스 필드, 생성자가 파일 I/O로 채움) 대신
    // 순수 입력만으로 테스트할 수 있게(서비스 전체를 DI로 구성하지 않아도 됨).
    internal static Metric DecryptAndParse(byte[] sealedPayload, byte[] aesKey)
    {
        var plaintext = Unseal(sealedPayload, aesKey);
        return Metric.Parser.ParseFrom(plaintext);
    }

    // DecryptAndParse와 완전히 같은 와이어 포맷(AesGcmPayload::Seal 기준)에 메시지 타입만 다름 -
    // Unseal()로 복호화 로직(신경 써야 할 crypto 슬라이싱 부분)만 공유하고, 기존 DecryptAndParse의
    // 시그니처/테스트(DecryptAndParseTests.cs)는 그대로 둠(2026-07-26 4순위 설계 - 제네릭화 대신
    // 이 방식을 택한 이유는 기존 테스트 영향 없이 가장 작은 변경으로 끝내기 위함).
    internal static TransactionSpan DecryptAndParseSpan(byte[] sealedPayload, byte[] aesKey)
    {
        var plaintext = Unseal(sealedPayload, aesKey);
        return TransactionSpan.Parser.ParseFrom(plaintext);
    }

    private static byte[] Unseal(byte[] sealedPayload, byte[] aesKey)
    {
        // AesGcmPayload::Seal()의 와이어 포맷: [Nonce(12B)][ciphertext(가변)][Tag(16B)]
        var nonce = sealedPayload.AsSpan(0, NonceSize);
        var tag = sealedPayload.AsSpan(sealedPayload.Length - TagSize, TagSize);
        var ciphertext = sealedPayload.AsSpan(NonceSize, sealedPayload.Length - NonceSize - TagSize);

        var plaintext = new byte[ciphertext.Length];
        using var aesGcm = new AesGcm(aesKey, TagSize);
        aesGcm.Decrypt(nonce, ciphertext, tag, plaintext);

        return plaintext;
    }

    private async Task StoreAndBroadcastAsync(Metric metric)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        var record = new MetricRecord
        {
            Ts = DateTimeOffset.UtcNow,
            CpuUsagePercent = metric.CpuUsagePercent,
            MemUsedBytes = (long)metric.MemUsedBytes,
            MemTotalBytes = (long)metric.MemTotalBytes,
            DiskUsedBytes = (long)metric.DiskUsedBytes,
            DiskTotalBytes = (long)metric.DiskTotalBytes,
            NetRxBytesPerSec = (long)metric.NetRxBytesPerSec,
            NetTxBytesPerSec = (long)metric.NetTxBytesPerSec,
            TcpRttUs = (int)metric.TcpRttUs,
            TcpRttVarUs = (int)metric.TcpRttVarUs,
            TcpRetransmits = (int)metric.TcpRetransmits,
            TcpTotalRetrans = (int)metric.TcpTotalRetrans,
            TcpSndCwnd = (int)metric.TcpSndCwnd,
        };

        db.Metrics.Add(record);
        await db.SaveChangesAsync();

        Console.WriteLine($"[MetricsReceiverService] 저장 완료: cpu={record.CpuUsagePercent}%");

        await _hubContext.Clients.All.SendAsync("NewMetric", record);

        await EvaluateAlertsAsync(db, record);
    }

    // 저장된 메트릭 1건에 대해 활성화된 임계치를 전부 평가 - 상태 전이(Opened/Resolved)가
    // 있을 때만 AlertRecord를 기록하고 SignalR로 알림(2026-07-26, WORK_STATUS.md 2순위).
    private async Task EvaluateAlertsAsync(ApmDbContext db, MetricRecord record)
    {
        var memPercent = record.MemTotalBytes > 0 ? record.MemUsedBytes * 100.0 / record.MemTotalBytes : 0;
        var diskPercent = record.DiskTotalBytes > 0 ? record.DiskUsedBytes * 100.0 / record.DiskTotalBytes : 0;

        var currentValues = new Dictionary<AlertMetricType, double>
        {
            [AlertMetricType.CpuPercent] = record.CpuUsagePercent,
            [AlertMetricType.MemoryPercent] = memPercent,
            [AlertMetricType.DiskPercent] = diskPercent,
            [AlertMetricType.TcpRttUs] = record.TcpRttUs,
        };

        var thresholds = await db.AlertThresholds.AsNoTracking().Where(t => t.Enabled).ToListAsync();

        foreach (var threshold in thresholds)
        {
            var currentValue = currentValues[threshold.MetricType];

            var openAlert = await db.AlertRecords
                .Where(a => a.MetricType == threshold.MetricType && a.ClosedAt == null)
                .OrderByDescending(a => a.Id)
                .FirstOrDefaultAsync();

            var transition = AlertEvaluator.Evaluate(currentValue, threshold.Value, openAlert != null);

            if (transition == AlertTransition.Opened)
            {
                var opened = new AlertRecord
                {
                    MetricType = threshold.MetricType,
                    ThresholdValue = threshold.Value,
                    TriggerValue = currentValue,
                    OpenedAt = DateTimeOffset.UtcNow,
                };
                db.AlertRecords.Add(opened);
                await db.SaveChangesAsync();

                Console.WriteLine($"[MetricsReceiverService] 알림 발생: {threshold.MetricType}={currentValue:F1} (임계치 {threshold.Value})");
                await _hubContext.Clients.All.SendAsync("AlertOpened", opened);
            }
            else if (transition == AlertTransition.Resolved && openAlert != null)
            {
                openAlert.ResolvedValue = currentValue;
                openAlert.ClosedAt = DateTimeOffset.UtcNow;
                await db.SaveChangesAsync();

                Console.WriteLine($"[MetricsReceiverService] 알림 해제: {threshold.MetricType}={currentValue:F1}");
                await _hubContext.Clients.All.SendAsync("AlertResolved", openAlert);
            }
        }
    }

    // Collector가 보낸 span을 저장만 함(대시보드 실시간 갱신은 이번 범위 밖 - 5순위에서
    // 집계 뷰를 만들 때 같이 고려, 2026-07-26 4순위 설계).
    private async Task StoreSpanAsync(TransactionSpan span)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        var record = new TransactionSpanRecord
        {
            Ts = DateTimeOffset.UtcNow,
            Source = "Collector",
            OperationName = span.OperationName,
            DurationUs = (long)span.DurationUs,
            Success = span.Success,
        };

        db.TransactionSpans.Add(record);
        await db.SaveChangesAsync();

        Console.WriteLine($"[MetricsReceiverService] span 저장: {record.OperationName} ({record.DurationUs}us, success={record.Success})");
    }
}
```

**`AlertsController.cs`** — `Index()` 액션에 데모 계측 한 줄 추가.

수정 전:
```csharp
    [HttpGet]
    public async Task<IActionResult> Index()
    {
        var thresholds = await _db.AlertThresholds.AsNoTracking()
            .OrderBy(t => t.MetricType)
            .ToListAsync();

        var active = await _db.AlertRecords.AsNoTracking()
            .Where(a => a.ClosedAt == null)
            .OrderByDescending(a => a.Id)
            .ToListAsync();

        var history = await _db.AlertRecords.AsNoTracking()
            .Where(a => a.ClosedAt != null)
            .OrderByDescending(a => a.Id)
            .Take(20)
            .ToListAsync();

        return View(new AlertsViewModel(thresholds, active, history));
    }
```

수정 후:
```csharp
    [HttpGet]
    public async Task<IActionResult> Index()
    {
        // 이 액션 자체를 계측 데모로 삼음(2026-07-26 4순위) - 실제 운영 중인 컨트롤러
        // 액션이라 "우리 APM으로 우리 자신을 모니터링"하는 스토리에 맞음.
        await using var span = TraceScope.Start(_db, "AlertsController.Index");

        var thresholds = await _db.AlertThresholds.AsNoTracking()
            .OrderBy(t => t.MetricType)
            .ToListAsync();

        var active = await _db.AlertRecords.AsNoTracking()
            .Where(a => a.ClosedAt == null)
            .OrderByDescending(a => a.Id)
            .ToListAsync();

        var history = await _db.AlertRecords.AsNoTracking()
            .Where(a => a.ClosedAt != null)
            .OrderByDescending(a => a.Id)
            .Take(20)
            .ToListAsync();

        return View(new AlertsViewModel(thresholds, active, history));
    }
```

**`RetentionService.cs`** — `TransactionSpans`도 `Metrics`와 같은 보존 기간(`_metricsRetentionDays`)으로 같이 정리.

수정 전:
```csharp
    private async Task PruneAsync(CancellationToken ct)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        var metricsCutoff = DateTimeOffset.UtcNow.AddDays(-_metricsRetentionDays);
        var metricsDeleted = await db.Metrics
            .Where(m => m.Ts < metricsCutoff)
            .ExecuteDeleteAsync(ct);

        // 진행 중인 알림(ClosedAt == null)은 기간과 무관하게 항상 보존 - 오래됐다고 지우면
        // 활성 알림 목록에서 사라지는 버그가 됨(AlertsController.Index의 active 조회 기준과 동일).
        var alertsCutoff = DateTimeOffset.UtcNow.AddDays(-_alertRetentionDays);
        var alertsDeleted = await db.AlertRecords
            .Where(a => a.ClosedAt != null && a.ClosedAt < alertsCutoff)
            .ExecuteDeleteAsync(ct);

        if (metricsDeleted > 0 || alertsDeleted > 0)
            Console.WriteLine($"[RetentionService] 정리 완료: metrics {metricsDeleted}건, alerts {alertsDeleted}건 삭제");
    }
```

수정 후:
```csharp
    private async Task PruneAsync(CancellationToken ct)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        var metricsCutoff = DateTimeOffset.UtcNow.AddDays(-_metricsRetentionDays);
        var metricsDeleted = await db.Metrics
            .Where(m => m.Ts < metricsCutoff)
            .ExecuteDeleteAsync(ct);

        // 진행 중인 알림(ClosedAt == null)은 기간과 무관하게 항상 보존 - 오래됐다고 지우면
        // 활성 알림 목록에서 사라지는 버그가 됨(AlertsController.Index의 active 조회 기준과 동일).
        var alertsCutoff = DateTimeOffset.UtcNow.AddDays(-_alertRetentionDays);
        var alertsDeleted = await db.AlertRecords
            .Where(a => a.ClosedAt != null && a.ClosedAt < alertsCutoff)
            .ExecuteDeleteAsync(ct);

        // TransactionSpans도 Metrics와 같은 고빈도 원본 데이터라 같은 보존 기간을 적용
        // (2026-07-26 4순위 설계 - 새 설정값을 따로 만들지 않고 기존 정책을 자연스럽게 확장).
        var spansDeleted = await db.TransactionSpans
            .Where(s => s.Ts < metricsCutoff)
            .ExecuteDeleteAsync(ct);

        if (metricsDeleted > 0 || alertsDeleted > 0 || spansDeleted > 0)
            Console.WriteLine($"[RetentionService] 정리 완료: metrics {metricsDeleted}건, alerts {alertsDeleted}건, spans {spansDeleted}건 삭제");
    }
```

### 확인 필요 없음 — 재확인한 기존 결정 그대로 적용

이번 항목은 앞서 사용자가 확정한 4건(계측 대상/데이터 모델/API 형태/대상 언어)을 그대로 구현한 것 — 추가로 확인받을 판단 지점은 없다고 보고 전체를 한 번에 제안함.

### 결정 사항

- 아직 파일 생성/수정 전 — `CLAUDE.md` 규칙대로 제안 단계(사용자가 이번엔 "4순위로 넘어가자"만 확인, "적용해줘"는 아직). 사용자가 "적용해줘" 하면 위 파일들(C++ 신규 4 + 수정 3: `Metric.proto`/`Common/CMakeLists.txt`/`Collector/main.cpp`, C# 신규 2 + 수정 4: `ApmDbContext.cs`/`MetricsReceiverService.cs`/`AlertsController.cs`/`RetentionService.cs` — 총 13개)을 전부 실제로 반영 예정.
- **적용 순서 주의**: `Metric.proto` 수정 후 반드시 **Linux(WSL)에서 `protoc` 재생성 + 커밋**이 먼저 이루어져야 C++ 쪽(`apm::TransactionSpan` 참조)이 컴파일됨 — 이 세션은 그 재생성을 대신 실행할 수 없음(아키텍처 제약, WORK_STATUS.md 참고). C# 쪽은 `dotnet build` 시 자동 재생성이라 별도 조치 불필요.
- 빌드 검증: Console은 `dotnet build`(+가능하면 `dotnet test` — 이번엔 새 테스트를 추가하지 않았으므로 기존 13건 통과만 재확인), Collector는 `cmake --build build`.

## 2026-07-26 — 5순위(백분위/집계 통계) 설계 제안

### 배경

4순위(함수/트랜잭션 레벨 계측) 코드 적용 + protoc 재생성 + Console/Collector 빌드·테스트 검증 + 커밋(`33ec155`)까지 완료 후 5순위로 전환.

**확인 필요 4건 → 사용자 결정**:
1. 대상 데이터: **`TransactionSpans`만**(`Metrics`는 이미 대시보드 시계열 그래프로 보이고 있어 백분위 필요성이 상대적으로 낮다고 판단)
2. 계산 시점: **조회 시점에 계산**(사전 집계 테이블 없음)
3. 집계 시간 창: **사용자가 선택**(1시간/24시간/7일)
4. 노출 위치: **새 페이지 `/apm/traces`**

### 설계

**핵심 판단 — SQLite에는 백분위 SQL 함수가 없음**: PostgreSQL/TimescaleDB는 `percentile_cont`를 지원하지만 SQLite는 없음. 백엔드 분기 없이 통일하기 위해, 시간 창으로 거른 `TransactionSpans`를 `.Select(필요 컬럼만).ToListAsync()`로 메모리에 가져온 뒤 **C#에서 직접** `GroupBy(OperationName)` → 정렬 → 백분위 계산. EF Core의 `GroupBy`를 SQL로 번역시키려다 실패/저효율 쿼리가 나는 걸 피하는 목적도 겸함. 데이터량은 3순위(보존정책, 기본 30일)로 이미 상한이 있고, 이번엔 최대 7일 창만 보므로 실용적인 범위.

**인덱스 재사용**: 4순위에서 이미 만들어둔 `TransactionSpanRecord`의 `(OperationName, Ts)` 복합 인덱스(`ApmDbContext.OnModelCreating`)가 "시간 창으로 거르고 OperationName으로 묶는" 이번 쿼리에 정확히 맞음 — **스키마 변경 불필요**.

**백분위 계산 방식**: 선형 보간(linear interpolation, `numpy.percentile` 기본값과 동일한 정의) — `AlertEvaluator`와 같은 패턴으로 `PercentileCalculator`를 순수 로직(DB/HTTP 의존 없음)으로 분리해 테스트 대상으로 삼음. 정렬은 호출자(`TracesController`) 책임 — 같은 정렬된 배열에서 p50/p95/p99를 여러 번 뽑아 쓰므로 매번 재정렬하지 않기 위함.

**단위 표시**: 저장은 `DurationUs`(정수, 마이크로초 — `MetricRecord.TcpRttUs`와 같은 관례)지만, 화면엔 **밀리초**로 환산해서 보여줌(레이턴시 백분위는 ms 단위가 업계 관례이자 가독성이 더 좋음 — 원시 메트릭 표(`Dashboard/Index.cshtml`)가 단위 변환 없이 그대로 보여주는 것과는 이 페이지의 목적 자체가 달라서 의도적으로 다르게 감).

**실시간 갱신 없음**: 2/4순위의 Alerts 페이지와 달리 SignalR을 안 씀 — "조회 시점에 계산" 결정과 상충되고(모든 span 저장마다 재계산해서 브로드캐스트하면 사실상 사전 집계와 다를 게 없어짐), 사용자가 매번 원하는 시간 창을 골라 새로고침하는 것으로 충분하다고 판단.

### 제안 — 신규 파일

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Infrastructure/PercentileCalculator.cs`** (신규)
```csharp
namespace ApmConsole.Domain.Apm.Infrastructure;

/*----------------------
    PercentileCalculator
------------------------*/
// 순수 로직만 담당(DB I/O 없음) - AlertEvaluator와 같은 이유로 public + 테스트 대상.
// SQLite는 PERCENTILE_CONT 같은 SQL 백분위 함수가 없어(PostgreSQL/TimescaleDB에는 있음)
// 백엔드 무관하게 동작하도록 C# 메모리 계산으로 통일(2026-07-26 5순위 결정).
public static class PercentileCalculator
{
    // 선형 보간(linear interpolation) 방식 - numpy.percentile 기본값과 동일한 정의.
    // sortedValues는 호출자가 오름차순 정렬해서 넘겨야 함(같은 배열로 p50/p95/p99를
    // 여러 번 구할 때 매번 재정렬하지 않기 위해 정렬 책임을 분리).
    public static double Compute(IReadOnlyList<long> sortedValues, double percentile)
    {
        if (sortedValues.Count == 0)
            return 0;
        if (sortedValues.Count == 1)
            return sortedValues[0];

        var rank = (percentile / 100.0) * (sortedValues.Count - 1);
        var lowerIndex = (int)Math.Floor(rank);
        var upperIndex = (int)Math.Ceiling(rank);

        if (lowerIndex == upperIndex)
            return sortedValues[lowerIndex];

        var fraction = rank - lowerIndex;
        return sortedValues[lowerIndex] + (sortedValues[upperIndex] - sortedValues[lowerIndex]) * fraction;
    }
}
```

**`APM_Console/tests/ApmConsole.Domain.Apm.Tests/Infrastructure/PercentileCalculatorTests.cs`** (신규, 기존 테스트 컨벤션 그대로 따름)
```csharp
using ApmConsole.Domain.Apm.Infrastructure;

namespace ApmConsole.Domain.Apm.Tests.Infrastructure;

public class PercentileCalculatorTests
{
    [Fact]
    public void 빈_배열이면_0을_반환한다()
    {
        var result = PercentileCalculator.Compute(Array.Empty<long>(), 50);

        Assert.Equal(0, result);
    }

    [Fact]
    public void 값이_하나뿐이면_그_값을_그대로_반환한다()
    {
        var result = PercentileCalculator.Compute(new long[] { 42 }, 99);

        Assert.Equal(42, result);
    }

    [Fact]
    public void P50은_중앙값과_같다()
    {
        var sorted = new long[] { 10, 20, 30, 40, 50 };

        var result = PercentileCalculator.Compute(sorted, 50);

        Assert.Equal(30, result);
    }

    [Fact]
    public void 순위가_두_값_사이에_있으면_선형보간한다()
    {
        // 4개 값(인덱스 0~3) 기준 p90 -> rank = 0.9 * 3 = 2.7 -> 인덱스 2와 3 사이를 30% 보간.
        var sorted = new long[] { 10, 20, 30, 40 };

        var result = PercentileCalculator.Compute(sorted, 90);

        Assert.Equal(37, result);   // 30 + (40-30)*0.7
    }

    [Fact]
    public void P100은_최댓값과_같다()
    {
        var sorted = new long[] { 5, 15, 25 };

        var result = PercentileCalculator.Compute(sorted, 100);

        Assert.Equal(25, result);
    }
}
```

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Models/TracesViewModel.cs`** (신규)
```csharp
namespace ApmConsole.Domain.Apm.Models;

public record OperationStatsRow(
    string OperationName,
    int Count,
    double P50Ms,
    double P95Ms,
    double P99Ms,
    double SuccessRatePercent);

public record TracesViewModel(string Window, List<OperationStatsRow> Rows);
```

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Controllers/TracesController.cs`** (신규)
```csharp
using ApmConsole.Domain.Apm.Infrastructure;
using ApmConsole.Domain.Apm.Infrastructure.Persistence;
using ApmConsole.Domain.Apm.Models;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace ApmConsole.Domain.Apm.Controllers;

[Area("Apm")]
[Route("apm/traces")]
public class TracesController : Controller
{
    private readonly ApmDbContext _db;

    public TracesController(ApmDbContext db)
    {
        _db = db;
    }

    // window: "1h" | "24h" | "7d" - 기본 1시간(2026-07-26 5순위 결정: 사용자가 링크로 선택).
    // 인식 못 하는 값은 "1h"로 취급(방어적) - 쿼리스트링을 직접 조작해도 안전하게 기본값으로 수렴.
    [HttpGet]
    public async Task<IActionResult> Index(string window = "1h")
    {
        window = window is "24h" or "7d" ? window : "1h";

        var lookback = window switch
        {
            "24h" => TimeSpan.FromHours(24),
            "7d" => TimeSpan.FromDays(7),
            _ => TimeSpan.FromHours(1),
        };
        var cutoff = DateTimeOffset.UtcNow - lookback;

        // 조회 시점에 계산(사전 집계 테이블 없음, 2026-07-26 5순위 결정) - GroupBy를 SQL로
        // 번역시키지 않고 필요한 컬럼만 뽑아 메모리로 가져온 뒤 C#에서 묶음 - SQLite/TimescaleDB
        // 백엔드 차이(SQLite는 PERCENTILE_CONT 같은 SQL 백분위 함수가 없음)를 아예 우회.
        // OperationName+Ts 복합 인덱스(ApmDbContext, 4순위)가 이 WHERE+묶음에 그대로 맞음.
        var spans = await _db.TransactionSpans
            .AsNoTracking()
            .Where(s => s.Ts >= cutoff)
            .Select(s => new { s.OperationName, s.DurationUs, s.Success })
            .ToListAsync();

        var rows = spans
            .GroupBy(s => s.OperationName)
            .Select(g =>
            {
                var sorted = g.Select(s => s.DurationUs).OrderBy(us => us).ToList();
                return new OperationStatsRow(
                    OperationName: g.Key,
                    Count: sorted.Count,
                    P50Ms: PercentileCalculator.Compute(sorted, 50) / 1000.0,
                    P95Ms: PercentileCalculator.Compute(sorted, 95) / 1000.0,
                    P99Ms: PercentileCalculator.Compute(sorted, 99) / 1000.0,
                    SuccessRatePercent: g.Count(s => s.Success) * 100.0 / g.Count());
            })
            .OrderByDescending(r => r.Count)
            .ToList();

        return View(new TracesViewModel(window, rows));
    }
}
```

**`APM_Console/src/Domains/Apm/ApmConsole.Domain.Apm/Areas/Apm/Views/Traces/Index.cshtml`** (신규)
```html
@model ApmConsole.Domain.Apm.Models.TracesViewModel

<link rel="stylesheet" href="~/css/site.css" />

<div class="dashboard">
	<h1>트랜잭션 통계</h1>
	<a href="/apm/dashboard">← 대시보드로</a>

	<div class="card">
		<h2>집계 구간</h2>
		@if (Model.Window == "1h")
		{
			<strong>최근 1시간</strong>
		}
		else
		{
			<a href="/apm/traces?window=1h">최근 1시간</a>
		}
		&nbsp;|&nbsp;
		@if (Model.Window == "24h")
		{
			<strong>최근 24시간</strong>
		}
		else
		{
			<a href="/apm/traces?window=24h">최근 24시간</a>
		}
		&nbsp;|&nbsp;
		@if (Model.Window == "7d")
		{
			<strong>최근 7일</strong>
		}
		else
		{
			<a href="/apm/traces?window=7d">최근 7일</a>
		}
	</div>

	<div class="card">
		<h2>연산별 레이턴시 백분위(ms)</h2>
		@if (!Model.Rows.Any())
		{
			<p class="empty-state">선택한 구간에 계측 데이터가 없습니다.</p>
		}
		else
		{
			<table>
				<thead>
					<tr><th>연산</th><th>건수</th><th>P50</th><th>P95</th><th>P99</th><th>성공률</th></tr>
				</thead>
				<tbody>
				@foreach (var r in Model.Rows)
				{
					<tr>
						<td>@r.OperationName</td>
						<td>@r.Count</td>
						<td>@r.P50Ms.ToString("F2")</td>
						<td>@r.P95Ms.ToString("F2")</td>
						<td>@r.P99Ms.ToString("F2")</td>
						<td>@r.SuccessRatePercent.ToString("F1")%</td>
					</tr>
				}
				</tbody>
			</table>
		}
	</div>
</div>
```

### 제안 — 기존 파일 수정 (수정 전 / 수정 후)

**변경 사유**: 대시보드에서 새 페이지로 가는 링크 하나만 추가(2순위 때 알림 페이지 링크를 추가한 것과 동일한 패턴).

**`Areas/Apm/Views/Dashboard/Index.cshtml`**

수정 전:
```html
<div class="dashboard">
	<h1>Apm 대시보드</h1>
	<a href="/apm/alerts">알림 설정/이력 →</a>
	<span id="connection-status" class="status-pill">실시간 연결 중...</span>
```

수정 후:
```html
<div class="dashboard">
	<h1>Apm 대시보드</h1>
	<a href="/apm/alerts">알림 설정/이력 →</a>
	<a href="/apm/traces">트랜잭션 통계 →</a>
	<span id="connection-status" class="status-pill">실시간 연결 중...</span>
```

### 확인 필요 없음 — 재확인한 기존 결정 그대로 적용

이번 항목은 앞서 사용자가 확정한 4건(대상 데이터/계산 시점/시간 창/노출 위치)을 그대로 구현한 것 — 추가로 확인받을 판단 지점은 없다고 보고 전체를 한 번에 제안함.

### 결정 사항

- 아직 파일 생성/수정 전 — `CLAUDE.md` 규칙대로 제안 단계(사용자가 이번엔 "5순위로 넘어가자"만 확인, "적용해줘"는 아직). 사용자가 "적용해줘" 하면 위 파일들(신규 5 + 수정 1: `Dashboard/Index.cshtml` — 총 6개)을 전부 실제로 반영 예정.
- 스키마 변경 없음(4순위에서 만든 인덱스 재사용) — Console 쪽만 닫히는 작업(Agent/Collector 변경 불필요).
- 빌드 검증: `dotnet build` + `dotnet test`(신규 `PercentileCalculatorTests` 5건 포함 — 기존 13건과 합쳐 18건 통과 기대).

---

## 2026-07-27 — 1-7(신규, Collector `Store()` 블로킹 개선) 설계 확정 + 코드 제안

### 배경

지난 세션(2026-07-26) 중단 지점 재개. 남은 미결정 사항이던 "`PRAGMA journal_mode=WAL` + `synchronous=NORMAL` 병행 여부"를 사용자와 논의:
- WAL의 동작 원리(롤백 저널 대비 fsync 1회로 감소, `synchronous=NORMAL`은 매 트랜잭션 fsync를 스킵하고 체크포인트 시점에만 동기화), JobQueue 비동기화와의 관계(서로 다른 계층 — JobQueue는 "어느 스레드가 블로킹되는지", WAL은 "블로킹 비용 자체의 크기") 설명.
- 사용자가 "결국 미루는 것뿐 아닌가, 싱글 스레드 블로킹 문제가 해결되는 게 맞냐"고 정확히 지적 — **맞다**: WAL+NORMAL 단독으로는 블로킹 문제(72개 하드 리밋의 근본 원인)를 해결하지 못함. 체크포인트 순간(기본 ~1000건마다 1회)엔 그 호출 스레드가 몰아서 블로킹을 그대로 맞음 — "평균 블로킹 시간을 줄이고 가끔 몰아서 미룬다"가 정확한 설명. 문제를 구조적으로 해결하는 건 JobQueue(스레드 분리)뿐, WAL은 JobQueue 적용 이후 워커 스레드의 총 부하를 줄여주는 보조 최적화.
- **사용자 결정**: WAL은 정보 부족으로 이번엔 보류(거부 아님 — 추후 재검토). **이번 세션은 JobQueue 비동기화만 진행.**

### 발견 — 초안(2026-07-26)의 "헤더 5개만 추가하면 됨" 가정이 부정확했음

지난 세션 조사에서 "`Collector/main.cpp`에 `JobQueue.h`/`ObjectPool.h`/`ThreadManager.h`/`CoreGlobal.h`/`CoreTLS.h`만 추가하면 된다"고 적어뒀는데, 이번에 `GW2_CrossPlatformCore/Thread/*.h` 전체를 실제로 열어 의존 관계를 추적해보니 부족했다:

- `JobQueue.h`는 `LockQueue.h`를 include하는데, `LockQueue.h`는 `USE_LOCK`(→ `Lock _locks[1];`) 매크로를 쓰지만 정작 자신은 `CoreMacro.h`(매크로 정의)도 `Lock.h`(`Lock` 클래스 정의)도 include하지 않음 — **포함하는 쪽(TU)이 먼저 include해뒀다는 걸 전제로 짜여진 헤더**(자기완결적이지 않음). `GW2_CrossPlatformCore/Main/CorePch.h`(이 모듈 자신의 pch)를 열어보니 정확히 이 순서로 나열돼 있었음: `CoreMacro.h` → `CoreGlobal.h` → `CoreTLS.h` → `Lock.h` → `ObjectPool.h` → `LockQueue.h` → `JobTimer.h` → `JobQueue.h`.
- `APM_Agent/pch.h`(Collector가 실제로 쓰는 pch)는 `Types.h`/`Container.h`만 가져오고 `CorePch.h`는 안 씀 — 그래서 저 체인을 `Collector/main.cpp`가 직접 `#include`로 채워줘야 함. `ThreadManager.h`는 `CorePch.h`에도 없어서(이 서브시스템을 실제로 기동하는 코드가 이 프레임워크 어디에도 없었다는 뜻) 마찬가지로 직접 추가 필요.
- CMake 쪽은 기존 조사(`Main`/`Thread`가 `GW2_CrossPlatformCore`의 public include 디렉터리)가 맞았음 — 재확인 완료, 변경 불필요.

**최종 include 목록(순서 중요, `CorePch.h`와 동일 순서로 추가)**:
```
CoreMacro.h → CoreGlobal.h → CoreTLS.h → Lock.h → ObjectPool.h → LockQueue.h → JobTimer.h → JobQueue.h → ThreadManager.h
```

### 제안 — `Collector/main.cpp` 수정 (수정 전 / 수정 후, 함수 전문)

**변경 사유**: `store->Store(pkt)`가 네트워크 스레드(`ioContext.run()`을 도는 유일한 스레드)를 SQLite `fdatasync` 완료까지 블로킹시키는 게 1-5 실측에서 확인된 근본 원인. `JobQueue` 하나를 전용 워커 스레드에 연결해 `Store()` 호출 자체를 그 워커로 넘기면, 네트워크 스레드는 저장 완료를 기다리지 않고 바로 다음 accept/dispatch로 넘어갈 수 있음. `JobQueue::Push(job, /*pushOnly=*/true)`를 명시적으로 써야 하는 이유: 기본값(`pushOnly=false`)은 호출 스레드가 다른 `JobQueue::Execute()` 안에 있지 않으면 **그 자리에서 동기 실행**해버려서(`JobQueue.cpp:18`), 아무것도 안 바뀐 것처럼 되어버림.

**include 블록 (파일 상단, 수정 전 / 수정 후)**

수정 전:
```cpp
#include "pch.h"
#include "ApmSession.h"
#include "AesGcmPayload.h"
#include "ResilientSender.h"
#include "PrivilegeDrop.h"
#include "PacketHandler.h"
#include "KeyLoader.h"
#include "CollectorConfig.h"
#include "ScopedSpan.h"
#include "SpanRecorder.h"
#include "Protocol/Metric.pb.h"
#include "Storage/MetricStoreFactory.h"
#include <thread>
```

수정 후:
```cpp
#include "pch.h"
#include "ApmSession.h"
#include "AesGcmPayload.h"
#include "ResilientSender.h"
#include "PrivilegeDrop.h"
#include "PacketHandler.h"
#include "KeyLoader.h"
#include "CollectorConfig.h"
#include "ScopedSpan.h"
#include "SpanRecorder.h"
#include "Protocol/Metric.pb.h"
#include "Storage/MetricStoreFactory.h"
#include <thread>

// SqliteMetricStore::Store()의 fdatasync가 네트워크 스레드를 블로킹하는 문제(1-5 실측 발견) 개선용
// - 저장 작업을 전용 워커 스레드로 넘기기 위한 JobQueue 서브시스템. 이 서브시스템은 여태 APM_Agent
// 어디서도 안 쓰였고, 자신의 pch(CorePch.h)에 기대는 방식이라 자기완결적이지 않음 - 그 pch가 쓰는
// 순서 그대로 나열해야 컴파일됨(2026-07-27 확인).
#include "CoreMacro.h"      // WRITE_LOCK/USE_LOCK 매크로, GetCurrentTick()
#include "CoreGlobal.h"     // extern GThreadManager
#include "CoreTLS.h"        // thread_local LEndTickCount
#include "Lock.h"           // LockQueue가 쓰는 Lock 클래스
#include "ObjectPool.h"     // 전역 MakeShared<T>()
#include "LockQueue.h"      // JobQueue 내부 큐
#include "JobTimer.h"       // JobQueue가 참조
#include "JobQueue.h"       // JobQueue, JobQueueRef
#include "ThreadManager.h"  // GThreadManager->Launch(), DoGlobalQueueWork()
```

**`main()` 함수 (수정 전 / 수정 후, 전문)**

수정 전:
```cpp
int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");
        AesGcmCipher::Key webServerKey = LoadKeyFromHexFile("certs/webserver_aes.key");

        CollectorConfig config = LoadCollectorConfig("collector_config.json");

        auto store = CreateMetricStore(STORAGE_CONNECTION_INFO, config.metricsRetentionDays);

        std::vector<apm::Metric> pendingMetrics;

        asio::io_context ioContext;

        asio::ssl::context sslContext(asio::ssl::context::tls_server);
        sslContext.use_certificate_chain_file("certs/server.crt");
        sslContext.use_private_key_file("certs/server.key", asio::ssl::context::pem);

        asio::ssl::context webServerSslContext(asio::ssl::context::tls_client);
        webServerSslContext.set_verify_mode(asio::ssl::verify_none);

        ResilientSender webServerSender(ioContext, webServerSslContext,
            config.webServerHost, config.webServerPort,
            [webServerKey]() { return std::make_unique<AesGcmPayload>(webServerKey); });

        PacketHandler::Register<apm::Metric>(
            [&store, &pendingMetrics](const apm::Metric& pkt)
            {
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                store->Store(pkt);
                pendingMetrics.push_back(pkt);
                std::cout << "[Collector] metric stored: cpu=" << pkt.cpu_usage_percent()
                    << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                    << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                    << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                    << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                    << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                    << ",cwnd:" << pkt.tcp_snd_cwnd()
                    << std::endl;
            });

        asio::ip::tcp::acceptor acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), PORT));

        PrivilegeDrop::DropTo("nobody");

        std::function<void()> doAccept;
        doAccept = [&]()
        {
            acceptor.async_accept(
                [&](const asio::error_code &ec, asio::ip::tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::cout << "[Collector] connection accepted" << std::endl;
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::cerr << "[Collector] accept error : " << ec.message() << std::endl;
                    }
                    doAccept();
                });
        };
        doAccept();

        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (!pendingMetrics.empty())
            {
                std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
                for (const auto& m : pendingMetrics)
                    webServerSender.Enqueue(m);

                pendingMetrics.clear();
            }

            auto spans = SpanRecorder::Instance().DrainAll();
            if (!spans.empty())
            {
                std::cout << "[Collector] WebServer로 span " << spans.size() << "건 전송 시도" << std::endl;
                for (const auto& s : spans)
                {
                    apm::TransactionSpan pkt;
                    pkt.set_operation_name(s.operationName);
                    pkt.set_duration_us(s.durationUs);
                    pkt.set_success(s.success);
                    webServerSender.Enqueue(pkt);
                }
            }
        };

        asio::steady_timer pushTimer(ioContext);
        std::function<void()> schedulePush;
        schedulePush = [&]()
        {
            pushTimer.expires_after(std::chrono::seconds(config.pushIntervalSeconds));
            pushTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        flushToWebServer();
                        schedulePush();
                    }
                });
        };
        schedulePush();

        asio::steady_timer pruneTimer(ioContext);
        std::function<void()> schedulePrune;
        schedulePrune = [&]()
        {
            pruneTimer.expires_after(std::chrono::hours(24));
            pruneTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        store->Prune(config.metricsRetentionDays);
                        schedulePrune();
                    }
                });
        };
        schedulePrune();

        std::thread cliThread(
            [&ioContext, &flushToWebServer]()
            {
                String line;
                while (std::getline(std::cin, line))
                {
                    if (line == "send")
                        asio::post(ioContext, flushToWebServer);
                }
            });
        cliThread.detach();

        std::cout << "Collector listening on port " << PORT << " (TLS)" << std::endl;
        std::cout << "[Collector] WebServer(" << config.webServerHost << ":" << config.webServerPort
            << ")로 " << config.pushIntervalSeconds << "초마다 전송 (콘솔에 'send' 입력 시 즉시 전송)" << std::endl;
        ioContext.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Collector] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

수정 후:
```cpp
int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");
        AesGcmCipher::Key webServerKey = LoadKeyFromHexFile("certs/webserver_aes.key");

        CollectorConfig config = LoadCollectorConfig("collector_config.json");

        auto store = CreateMetricStore(STORAGE_CONNECTION_INFO, config.metricsRetentionDays);

        // 1-7: SqliteMetricStore::Store()의 fdatasync가 네트워크 스레드를 막지 못하게, 저장 호출을
        // 전용 워커 스레드로 넘기는 큐. storePtr은 store(unique_ptr)가 main() 스코프 내내 살아있는
        // 것에 기대는 non-owning 포인터 - 워커 스레드도 main()이 끝나기 전까지만 존재하므로 안전.
        IMetricStore* storePtr = store.get();
        JobQueueRef metricStoreQueue = MakeShared<JobQueue>();

        // 전용 워커 스레드 1개 - SQLite는 어차피 단일 writer라 여러 개 띄워도 JobQueue 자체가
        // 직렬화함(늘릴 이유 없음). LEndTickCount를 루프마다 먼저 세팅해야 DoGlobalQueueWork()가
        // 즉시 break하지 않음(GW2 틱 서버 관례) - ThreadManager.cpp:63 참고.
        GThreadManager->Launch([]()
            {
                while (true)
                {
                    LEndTickCount = GetCurrentTick() + 100;
                    ThreadManager::DistributeReservedJobs();
                    ThreadManager::DoGlobalQueueWork();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });

        std::vector<apm::Metric> pendingMetrics;

        asio::io_context ioContext;

        asio::ssl::context sslContext(asio::ssl::context::tls_server);
        sslContext.use_certificate_chain_file("certs/server.crt");
        sslContext.use_private_key_file("certs/server.key", asio::ssl::context::pem);

        asio::ssl::context webServerSslContext(asio::ssl::context::tls_client);
        webServerSslContext.set_verify_mode(asio::ssl::verify_none);

        ResilientSender webServerSender(ioContext, webServerSslContext,
            config.webServerHost, config.webServerPort,
            [webServerKey]() { return std::make_unique<AesGcmPayload>(webServerKey); });

        PacketHandler::Register<apm::Metric>(
            [storePtr, metricStoreQueue, &pendingMetrics](const apm::Metric& pkt)
            {
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                // store->Store(pkt) 직접 호출(동기, fdatasync 블로킹 포함) 대신 워커 스레드로 위임.
                // pushOnly=true 필수 - 기본값(false)이면 호출 스레드가 다른 JobQueue::Execute() 안이
                // 아닐 때 그 자리에서 동기 실행해버려 아무 효과가 없어짐(JobQueue.cpp:18).
                // pkt은 값 복사로 캡처 - 비동기 실행 시점까지 살아있어야 함.
                metricStoreQueue->Push(MakeShared<Job>([storePtr, pkt]() { storePtr->Store(pkt); }), /*pushOnly=*/true);

                pendingMetrics.push_back(pkt);
                // 저장이 이제 비동기라 이 시점엔 아직 안 끝났을 수 있음 - "stored"는 부정확한 표현이라 정정.
                std::cout << "[Collector] metric received: cpu=" << pkt.cpu_usage_percent()
                    << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                    << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                    << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                    << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                    << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                    << ",cwnd:" << pkt.tcp_snd_cwnd()
                    << std::endl;
            });

        asio::ip::tcp::acceptor acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), PORT));

        PrivilegeDrop::DropTo("nobody");

        std::function<void()> doAccept;
        doAccept = [&]()
        {
            acceptor.async_accept(
                [&](const asio::error_code &ec, asio::ip::tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::cout << "[Collector] connection accepted" << std::endl;
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::cerr << "[Collector] accept error : " << ec.message() << std::endl;
                    }
                    doAccept();
                });
        };
        doAccept();

        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (!pendingMetrics.empty())
            {
                std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
                for (const auto& m : pendingMetrics)
                    webServerSender.Enqueue(m);

                pendingMetrics.clear();
            }

            auto spans = SpanRecorder::Instance().DrainAll();
            if (!spans.empty())
            {
                std::cout << "[Collector] WebServer로 span " << spans.size() << "건 전송 시도" << std::endl;
                for (const auto& s : spans)
                {
                    apm::TransactionSpan pkt;
                    pkt.set_operation_name(s.operationName);
                    pkt.set_duration_us(s.durationUs);
                    pkt.set_success(s.success);
                    webServerSender.Enqueue(pkt);
                }
            }
        };

        asio::steady_timer pushTimer(ioContext);
        std::function<void()> schedulePush;
        schedulePush = [&]()
        {
            pushTimer.expires_after(std::chrono::seconds(config.pushIntervalSeconds));
            pushTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        flushToWebServer();
                        schedulePush();
                    }
                });
        };
        schedulePush();

        asio::steady_timer pruneTimer(ioContext);
        std::function<void()> schedulePrune;
        schedulePrune = [&]()
        {
            pruneTimer.expires_after(std::chrono::hours(24));
            pruneTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        store->Prune(config.metricsRetentionDays);
                        schedulePrune();
                    }
                });
        };
        schedulePrune();

        std::thread cliThread(
            [&ioContext, &flushToWebServer]()
            {
                String line;
                while (std::getline(std::cin, line))
                {
                    if (line == "send")
                        asio::post(ioContext, flushToWebServer);
                }
            });
        cliThread.detach();

        std::cout << "Collector listening on port " << PORT << " (TLS)" << std::endl;
        std::cout << "[Collector] WebServer(" << config.webServerHost << ":" << config.webServerPort
            << ")로 " << config.pushIntervalSeconds << "초마다 전송 (콘솔에 'send' 입력 시 즉시 전송)" << std::endl;
        ioContext.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Collector] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

### 짚어둘 캐치사항 — 워커 스레드는 정상 종료 경로가 없음

`GThreadManager->Launch(...)`로 띄운 워커는 `while(true)` 무한 루프라 빠져나올 방법이 없다. `GThreadManager`는 전역 정적 객체(`CoreGlobal.cpp`의 `GCoreGlobal`)라 그 소멸자가 `ThreadManager::~ThreadManager()` → `Join()`을 호출하는데, `Join()`은 스레드가 끝나길 기다리므로 **이 워커가 도는 한 프로그램이 정상 종료(정적 소멸자 실행 경로)를 절대 못 빠져나옴**. 다만:
- 지금 `Collector`는 애초에 `ioContext.run()`도 무한 루프고 별도 종료 처리(시그널 핸들러 등)가 없어 "프로세스를 죽여서 끝낸다"가 유일한 종료 방법 - 이 워커 스레드도 그 관례를 그대로 따르는 것뿐이라 **새로 생기는 문제는 아님**.
- `GThreadManager::Launch`가 GW2 틱 서버 관례(무한 루프 워커, kill로 종료)를 그대로 쓰는 API라 이 패턴이 프레임워크 의도와도 맞음.
- 정리해서 종료하는 기능이 필요해지면(예: 나중에 graceful shutdown 요구사항이 생기면) 별도로 다뤄야 함 - 이번 범위 밖.

### 결정 사항

- WAL(`journal_mode=WAL` + `synchronous=NORMAL`)은 이번 라운드에서 보류 - 정보 부족 사유, 거부 아님. 다음에 재검토 시 이 항목 다시 열 것.
- 아직 파일 수정 전 - `CLAUDE.md` 규칙대로 제안 단계. 사용자가 "적용해줘" 하면 `Collector/main.cpp` 1개 파일(include 블록 + `main()` 함수) 반영 예정.
- 적용 후 검증 계획: `cmake --build build`로 `Collector`/`Agent`/`LoadTester`/`APM_Common_Tests` 빌드 확인 → 가능하면 LoadTester로 개선 전/후 재실측(1-5와 동일 매트릭스, 72개 하드 리밋이 실제로 풀리는지 비교) → `/apm/traces`에서 `Collector.HandleMetricPacket` span 지연시간이 줄었는지 확인(선택, 4/5순위 계측 재활용).

### 적용 완료 (2026-07-27, 같은 세션에서 "바로 적용해줘" 확인)

위 코드 전문 그대로 `Collector/main.cpp`에 반영. 빌드 시도 중 추가로 발견한 문제 1건 — `CoreMacro.h`의 `PrintStackTrace()`/`CrashLog()`가 `backtrace()`/`backtrace_symbols()`(`<execinfo.h>`)와 `ofstream`(`<fstream>`)을 쓰는데 자기 스스로 그 헤더들을 include 안 하고 `CorePch.h`가 먼저 include해줬다는 전제로 짜여 있었음(이번 설계 조사 때 정정한 "9개 헤더" 목록에서도 못 잡았던 부분 — `LockQueue.h`류의 매크로 의존과는 다른 종류의 누락이라 정적으로 안 읽히고 컴파일러가 잡아줌).

**수정 (include 블록, `CoreMacro.h` include 직전에 추가)**:
```cpp
#include <fstream>
#ifdef _WIN32
#include <dbghelp.h>
#else
#include <execinfo.h>
#endif
#include "CoreMacro.h"      // WRITE_LOCK/USE_LOCK 매크로, GetCurrentTick()
```

**검증(WSL)**: `cmake --build build --target Collector` 성공 → `cmake --build build`(전체) 성공(`GW2_CrossPlatformCore`/`APM_Storage`/`APM_Common`/`Collector`/`APM_Common_Tests`/`Agent`/`LoadTester`) → `ctest --test-dir build` `9/9 tests passed`(기존 암호화 테스트, 회귀 없음 — Collector `main()` 자체를 검증하는 유닛테스트는 없음).

실행 관점(Collector 실제 기동 + LoadTester 재실측)은 이번 세션 범위 밖으로 남겨둠 — 필요시 별도로 진행.

---

## 2026-07-27 — 1-7 재실측 중 크래시 발견 + `JobQueue` → 자체 `WorkerQueue` 전환 설계

### 배경

WAL 재검토 논의 끝에 사용자가 "1-5와 동일한 스트레스 매트릭스 재실행"을 선택. `run_load_test.sh`로 6단계(1/10/50/100/100+ramp5s/300 에이전트) 재실행 → **전 단계에서 Collector가 시작 약 10초 만에 크래시**(`connect_fail`이 agents=10부터 이미 발생, agents=1도 뒤늦게 크래시).

### 원인 분석 — `GW2_CrossPlatformCore/Thread/Lock.cpp`의 `WriteUnlock()` 버그

`collector_stdout.log`에서 `[CRASH] cause=LOCK_TIMEOUT ... func=WriteLock` 확인. `Lock.cpp` 재확인 결과:

```cpp
void Lock::WriteUnlock(const char* name)
{
	const uint32 threadId = GetThisThreadId();
	const uint32 lockThreadId = (_lockFlag.load() & WRITE_THREAD_MASK) >> 16;

	if ((threadId & 0xFFFF) == lockThreadId)
	{
		_lockFlag.fetch_add(1);   // 버그: _writeCount를 안 줄이고 무관한 하위 비트만 건드림 -
		return;                   // WRITE_THREAD_MASK(소유자 스레드 ID)가 절대 안 지워짐.
	}
	// ...
}
```

소유 스레드가 언락해도 `_lockFlag`의 소유자 ID 비트가 안 지워져, **그 락을 처음 잡은 스레드가 영구 소유한 것처럼 남음**. 다른 스레드가 나중에 같은 락을 잡으려 하면 `WriteLock()`의 CAS가 계속 실패 → 10초(`ACQUIRE_TIMEOUT_TICK`) 스핀 → `CRASH("LOCK_TIMEOUT")`.

1-7 이전엔 `APM_Agent`가 `Lock`/`LockQueue`/`JobQueue`를 전혀 안 써서(이미 조사에서 확인) 이 버그가 한 번도 안 드러났음 — 1-7에서 **네트워크 스레드가 `_jobs.Push()`(락을 처음 잡았다 놓음 → 이 시점부터 락이 네트워크 스레드 소유로 영구 고정)한 뒤, 워커 스레드가 `Execute()`→`PopAll()`에서 같은 락을 잡으려다** 최초로 재현됨.

### 사용자 결정 (2026-07-27)

- `GW2_CrossPlatformCore/Thread/Lock.cpp`는 수정하지 않음 — "여러 프로젝트를 통해 이미 검증한 내용"이라는 사용자 판단.
- **`APM_Agent` 쪽에서 우회** — `JobQueue` 대신 `std::mutex`/`condition_variable`만 쓰는 자체 큐(`WorkerQueue`)를 새로 만들어 대체하는 방향으로 확정.

### 제안 — `Common/WorkerQueue.h`/`.cpp` (신규, 전체 파일)

**변경 사유**: `JobQueue`가 의존하는 `Lock`은 크로스 스레드(네트워크→워커) 사용에서 크래시하므로 못 씀. 표준 라이브러리 프리미티브만으로 같은 역할(단일 워커 스레드가 순차 소비)을 하는 작은 큐로 대체 — `GW2_CrossPlatformCore/Thread/*`(`JobQueue`/`ThreadManager`/`Lock` 등) 의존을 완전히 제거하므로, 1-7에서 추가했던 9개 헤더 + `<fstream>`/`<execinfo.h>` 우회 코드도 전부 걷어낼 수 있음(부수적으로 코드 단순화).

**`Common/WorkerQueue.h`**
```cpp
#pragma once
#include "pch.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>

/*-------------
	WorkerQueue
---------------*/
// 단일 워커 스레드가 순차적으로 소비하는 스레드 안전 작업 큐. std::mutex/condition_variable만
// 사용 - GW2_CrossPlatformCore/Thread/JobQueue(+Lock)는 여러 프로젝트에서 이미 검증된 코드라
// 그대로 두기로 하고(2026-07-27), 대신 이 용도 전용으로 APM_Agent 안에 작은 큐를 새로 둠.
// 배경: JobQueue가 기대는 Lock::WriteUnlock()이 소유 스레드 비트를 절대 안 지우는 버그가 있어
// 네트워크 스레드가 한 번 락을 잡았다 놓으면 워커 스레드가 영원히 못 잡고 10초 뒤 크래시함 -
// 1-7 재실측 중 실제로 재현됨.
class WorkerQueue
{
public:
	WorkerQueue();
	~WorkerQueue();

	void Push(std::function<void()> job);

private:
	void WorkerLoop();

private:
	std::mutex _mutex;
	std::condition_variable _cv;
	std::queue<std::function<void()>> _jobs;
	bool _stop = false;
	std::thread _worker;   // 마지막에 선언 - 위 멤버들이 이미 다 만들어진 뒤에 워커 스레드를 기동
};
```

**`Common/WorkerQueue.cpp`**
```cpp
#include "pch.h"
#include "WorkerQueue.h"

WorkerQueue::WorkerQueue()
	: _worker([this]() { WorkerLoop(); })
{
}

WorkerQueue::~WorkerQueue()
{
	{
		std::lock_guard<std::mutex> guard(_mutex);
		_stop = true;
	}
	_cv.notify_one();
	_worker.join();
}

void WorkerQueue::Push(std::function<void()> job)
{
	{
		std::lock_guard<std::mutex> guard(_mutex);
		_jobs.push(std::move(job));
	}
	_cv.notify_one();
}

void WorkerQueue::WorkerLoop()
{
	while (true)
	{
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_cv.wait(lock, [this]() { return _stop || !_jobs.empty(); });

			// _stop이 요청됐어도 남은 job은 마저 비우고 종료 - 큐가 실제로 빈 경우에만 탈출.
			if (_jobs.empty())
				return;

			job = std::move(_jobs.front());
			_jobs.pop();
		}

		job();
	}
}
```

**`Common/CMakeLists.txt`** — `SpanRecorder.cpp` 옆에 한 줄 추가:

수정 전:
```cmake
add_library(APM_Common STATIC
    ApmSession.cpp
    AriaCipher.cpp
    AesGcmCipher.cpp
    AesGcmPayload.cpp
    HmacUtil.cpp
    SecurePayload.cpp
    KeyLoader.cpp
    PrivilegeDrop.cpp
    ResourceCollector.cpp
    MetricScheduler.cpp
    ResilientSender.cpp
    PacketHandler.cpp
    SpanRecorder.cpp
    ScopedSpan.cpp
    ../Protocol/Metric.pb.cc
)
```

수정 후:
```cmake
add_library(APM_Common STATIC
    ApmSession.cpp
    AriaCipher.cpp
    AesGcmCipher.cpp
    AesGcmPayload.cpp
    HmacUtil.cpp
    SecurePayload.cpp
    KeyLoader.cpp
    PrivilegeDrop.cpp
    ResourceCollector.cpp
    MetricScheduler.cpp
    ResilientSender.cpp
    PacketHandler.cpp
    SpanRecorder.cpp
    ScopedSpan.cpp
    WorkerQueue.cpp
    ../Protocol/Metric.pb.cc
)
```

### 제안 — `Collector/main.cpp` 수정 (수정 전 / 수정 후)

**include 블록**

수정 전(1-7 적용분):
```cpp
#include "Protocol/Metric.pb.h"
#include "Storage/MetricStoreFactory.h"
#include <thread>

// SqliteMetricStore::Store()의 fdatasync가 네트워크 스레드를 블로킹하는 문제(1-5 실측 발견) 개선용
// - 저장 작업을 전용 워커 스레드로 넘기기 위한 JobQueue 서브시스템. 이 서브시스템은 여태 APM_Agent
// 어디서도 안 쓰였고, 자신의 pch(CorePch.h)에 기대는 방식이라 자기완결적이지 않음 - 그 pch가 쓰는
// 순서 그대로 나열해야 컴파일됨(2026-07-27 확인). CoreMacro.h의 PrintStackTrace()/CrashLog()가
// <fstream>/<execinfo.h>(Windows는 <dbghelp.h>)를 자기 스스로 include 안 해서 빌드 시도 중 추가 발견.
#include <fstream>
#ifdef _WIN32
#include <dbghelp.h>
#else
#include <execinfo.h>
#endif
#include "CoreMacro.h"      // WRITE_LOCK/USE_LOCK 매크로, GetCurrentTick()
#include "CoreGlobal.h"     // extern GThreadManager
#include "CoreTLS.h"        // thread_local LEndTickCount
#include "Lock.h"           // LockQueue가 쓰는 Lock 클래스
#include "ObjectPool.h"     // 전역 MakeShared<T>()
#include "LockQueue.h"      // JobQueue 내부 큐
#include "JobTimer.h"       // JobQueue가 참조
#include "JobQueue.h"       // JobQueue, JobQueueRef
#include "ThreadManager.h"  // GThreadManager->Launch(), DoGlobalQueueWork()
```

수정 후:
```cpp
#include "Protocol/Metric.pb.h"
#include "Storage/MetricStoreFactory.h"
#include <thread>

// SqliteMetricStore::Store()의 fdatasync가 네트워크 스레드를 블로킹하는 문제(1-5 실측 발견) 개선용.
// GW2_CrossPlatformCore/Thread/JobQueue는 재실측 중 Lock::WriteUnlock() 버그로 크로스 스레드
// 사용 시 크래시하는 게 확인돼(2026-07-27, 이 파일은 검증된 코드라 수정하지 않기로 결정)
// APM_Agent 자체 WorkerQueue(std::mutex/condition_variable만 사용)로 대체.
#include "WorkerQueue.h"
```

**`main()` 함수 전문**

수정 전(1-7 적용분, 현재 디스크 상태):
```cpp
int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");
        AesGcmCipher::Key webServerKey = LoadKeyFromHexFile("certs/webserver_aes.key");

        CollectorConfig config = LoadCollectorConfig("collector_config.json");

        auto store = CreateMetricStore(STORAGE_CONNECTION_INFO, config.metricsRetentionDays);

        // 1-7: SqliteMetricStore::Store()의 fdatasync가 네트워크 스레드를 막지 못하게, 저장 호출을
        // 전용 워커 스레드로 넘기는 큐. storePtr은 store(unique_ptr)가 main() 스코프 내내 살아있는
        // 것에 기대는 non-owning 포인터 - 워커 스레드도 main()이 끝나기 전까지만 존재하므로 안전.
        IMetricStore* storePtr = store.get();
        JobQueueRef metricStoreQueue = MakeShared<JobQueue>();

        // 전용 워커 스레드 1개 - SQLite는 어차피 단일 writer라 여러 개 띄워도 JobQueue 자체가
        // 직렬화함(늘릴 이유 없음). LEndTickCount를 루프마다 먼저 세팅해야 DoGlobalQueueWork()가
        // 즉시 break하지 않음(GW2 틱 서버 관례) - ThreadManager.cpp:63 참고.
        GThreadManager->Launch([]()
            {
                while (true)
                {
                    LEndTickCount = GetCurrentTick() + 100;
                    ThreadManager::DistributeReservedJobs();
                    ThreadManager::DoGlobalQueueWork();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });

        // Collector가 받은 뒤 아직 WebServer로 안 보낸 메트릭들 - 주기/CLI 트리거로 비워짐.
        std::vector<apm::Metric> pendingMetrics;

        asio::io_context ioContext;

        // Agent 접속을 받는 서버 역할 컨텍스트(기존)
        asio::ssl::context sslContext(asio::ssl::context::tls_server);
        sslContext.use_certificate_chain_file("certs/server.crt");
        sslContext.use_private_key_file("certs/server.key", asio::ssl::context::pem);

        // WebServer에 접속하는 클라이언트 역할 컨텍스트(신규) - Agent용과 모드가 달라 별도 필요.
        asio::ssl::context webServerSslContext(asio::ssl::context::tls_client);
        // 테스트용 자체 서명 인증서라 CA 검증 생략(Agent->Collector와 동일한 이유, 프로덕션 금지).
        webServerSslContext.set_verify_mode(asio::ssl::verify_none);

        ResilientSender webServerSender(ioContext, webServerSslContext,
            config.webServerHost, config.webServerPort,
            [webServerKey]() { return std::make_unique<AesGcmPayload>(webServerKey); });

        PacketHandler::Register<apm::Metric>(
            [storePtr, metricStoreQueue, &pendingMetrics](const apm::Metric& pkt)
            {
                // Collector 안에서 "트랜잭션"이라 부를 만한 지점 중 가장 자연스러운 곳 -
                // Agent가 보낸 메트릭 패킷 하나를 받아 저장하는 구간(2026-07-26 4순위 데모 계측).
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                // store->Store(pkt) 직접 호출(동기, fdatasync 블로킹 포함) 대신 워커 스레드로 위임.
                // pushOnly=true 필수 - 기본값(false)이면 호출 스레드가 다른 JobQueue::Execute() 안이
                // 아닐 때 그 자리에서 동기 실행해버려 아무 효과가 없어짐(JobQueue.cpp:18).
                // pkt은 값 복사로 캡처 - 비동기 실행 시점까지 살아있어야 함.
                metricStoreQueue->Push(MakeShared<Job>([storePtr, pkt]() { storePtr->Store(pkt); }), /*pushOnly=*/true);

                pendingMetrics.push_back(pkt);
                // 저장이 이제 비동기라 이 시점엔 아직 안 끝났을 수 있음 - "stored"는 부정확한 표현이라 정정.
                std::cout << "[Collector] metric received: cpu=" << pkt.cpu_usage_percent()
                    << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                    << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                    << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                    << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                    << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                    << ",cwnd:" << pkt.tcp_snd_cwnd()
                    << std::endl;
            });

        asio::ip::tcp::acceptor acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), PORT));

        // 포트바인딩 : 특권이 필요할 수 있는 유일한 단계, 완료 직후 권한 하향
        PrivilegeDrop::DropTo("nobody");

        std::function<void()> doAccept;
        doAccept = [&]()
        {
            acceptor.async_accept(
                [&](const asio::error_code &ec, asio::ip::tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::cout << "[Collector] connection accepted" << std::endl;
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::cerr << "[Collector] accept error : " << ec.message() << std::endl;
                    }
                    doAccept();
                });
        };
        doAccept();

        // pendingMetrics/SpanRecorder에 쌓인 걸 전부 WebServer로 보내고 비움 - 주기 타이머와
        // CLI 트리거 둘 다 이 함수 하나를 호출함(로직 중복 방지). span 전송을 여기 얹은 이유:
        // 이미 "주기적으로 WebServer에 밀어넣는" 책임을 지고 있는 함수라 새 타이머를 또
        // 만들 필요가 없음(2026-07-26 4순위 설계).
        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (!pendingMetrics.empty())
            {
                std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
                for (const auto& m : pendingMetrics)
                    webServerSender.Enqueue(m);

                pendingMetrics.clear();
            }

            auto spans = SpanRecorder::Instance().DrainAll();
            if (!spans.empty())
            {
                std::cout << "[Collector] WebServer로 span " << spans.size() << "건 전송 시도" << std::endl;
                for (const auto& s : spans)
                {
                    apm::TransactionSpan pkt;
                    pkt.set_operation_name(s.operationName);
                    pkt.set_duration_us(s.durationUs);
                    pkt.set_success(s.success);
                    webServerSender.Enqueue(pkt);
                }
            }
        };

        asio::steady_timer pushTimer(ioContext);
        std::function<void()> schedulePush;
        schedulePush = [&]()
        {
            pushTimer.expires_after(std::chrono::seconds(config.pushIntervalSeconds));
            pushTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        flushToWebServer();
                        schedulePush();
                    }
                });
        };
        schedulePush();

        // 로컬 저장소(store) 보존 정책 - pushTimer와 같은 패턴, 24시간 간격으로
        // 오래된 행 정리(TimescaleMetricStore는 내부적으로 no-op, SqliteMetricStore만 실제
        // DELETE 수행 - IMetricStore::Prune 문서 참고, 2026-07-26 3순위 설계).
        asio::steady_timer pruneTimer(ioContext);
        std::function<void()> schedulePrune;
        schedulePrune = [&]()
        {
            pruneTimer.expires_after(std::chrono::hours(24));
            pruneTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        store->Prune(config.metricsRetentionDays);
                        schedulePrune();
                    }
                });
        };
        schedulePrune();

        // Collector 콘솔에 "send"를 입력하면 즉시 전송. stdin 읽기는 블로킹이라 별도 스레드에서
        // 돌리고, 실제 전송(flushToWebServer)은 asio::post로 io_context 스레드에 넘김 -
        // pendingMetrics/webServerSender를 항상 단일 스레드에서만 건드리게 되어 락이 불필요함.
        std::thread cliThread(
            [&ioContext, &flushToWebServer]()
            {
                String line;
                while (std::getline(std::cin, line))
                {
                    if (line == "send")
                        asio::post(ioContext, flushToWebServer);
                }
            });
        cliThread.detach();

        std::cout << "Collector listening on port " << PORT << " (TLS)" << std::endl;
        std::cout << "[Collector] WebServer(" << config.webServerHost << ":" << config.webServerPort
            << ")로 " << config.pushIntervalSeconds << "초마다 전송 (콘솔에 'send' 입력 시 즉시 전송)" << std::endl;
        ioContext.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Collector] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

수정 후(제안, 변경된 부분만 발췌하지 않고 함수 전문 — 바뀐 곳은 선언부와 `PacketHandler::Register` 캡처/`Push` 호출부 두 곳뿐):
```cpp
int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");
        AesGcmCipher::Key webServerKey = LoadKeyFromHexFile("certs/webserver_aes.key");

        CollectorConfig config = LoadCollectorConfig("collector_config.json");

        auto store = CreateMetricStore(STORAGE_CONNECTION_INFO, config.metricsRetentionDays);

        // 1-7: SqliteMetricStore::Store()의 fdatasync가 네트워크 스레드를 막지 못하게, 저장 호출을
        // 전용 워커 스레드로 넘기는 큐. storePtr은 store(unique_ptr)가 main() 스코프 내내 살아있는
        // 것에 기대는 non-owning 포인터 - 워커 스레드도 main()이 끝나기 전까지만 존재하므로 안전.
        IMetricStore* storePtr = store.get();
        WorkerQueue metricStoreQueue;

        // Collector가 받은 뒤 아직 WebServer로 안 보낸 메트릭들 - 주기/CLI 트리거로 비워짐.
        std::vector<apm::Metric> pendingMetrics;

        asio::io_context ioContext;

        // Agent 접속을 받는 서버 역할 컨텍스트(기존)
        asio::ssl::context sslContext(asio::ssl::context::tls_server);
        sslContext.use_certificate_chain_file("certs/server.crt");
        sslContext.use_private_key_file("certs/server.key", asio::ssl::context::pem);

        // WebServer에 접속하는 클라이언트 역할 컨텍스트(신규) - Agent용과 모드가 달라 별도 필요.
        asio::ssl::context webServerSslContext(asio::ssl::context::tls_client);
        // 테스트용 자체 서명 인증서라 CA 검증 생략(Agent->Collector와 동일한 이유, 프로덕션 금지).
        webServerSslContext.set_verify_mode(asio::ssl::verify_none);

        ResilientSender webServerSender(ioContext, webServerSslContext,
            config.webServerHost, config.webServerPort,
            [webServerKey]() { return std::make_unique<AesGcmPayload>(webServerKey); });

        PacketHandler::Register<apm::Metric>(
            [storePtr, &metricStoreQueue, &pendingMetrics](const apm::Metric& pkt)
            {
                // Collector 안에서 "트랜잭션"이라 부를 만한 지점 중 가장 자연스러운 곳 -
                // Agent가 보낸 메트릭 패킷 하나를 받아 저장하는 구간(2026-07-26 4순위 데모 계측).
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                // store->Store(pkt) 직접 호출(동기, fdatasync 블로킹 포함) 대신 워커 스레드로 위임.
                // pkt은 값 복사로 캡처 - 비동기 실행 시점까지 살아있어야 함.
                metricStoreQueue.Push([storePtr, pkt]() { storePtr->Store(pkt); });

                pendingMetrics.push_back(pkt);
                // 저장이 이제 비동기라 이 시점엔 아직 안 끝났을 수 있음 - "stored"는 부정확한 표현이라 정정.
                std::cout << "[Collector] metric received: cpu=" << pkt.cpu_usage_percent()
                    << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                    << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                    << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                    << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                    << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                    << ",cwnd:" << pkt.tcp_snd_cwnd()
                    << std::endl;
            });

        asio::ip::tcp::acceptor acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), PORT));

        // 포트바인딩 : 특권이 필요할 수 있는 유일한 단계, 완료 직후 권한 하향
        PrivilegeDrop::DropTo("nobody");

        std::function<void()> doAccept;
        doAccept = [&]()
        {
            acceptor.async_accept(
                [&](const asio::error_code &ec, asio::ip::tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::cout << "[Collector] connection accepted" << std::endl;
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::cerr << "[Collector] accept error : " << ec.message() << std::endl;
                    }
                    doAccept();
                });
        };
        doAccept();

        // pendingMetrics/SpanRecorder에 쌓인 걸 전부 WebServer로 보내고 비움 - 주기 타이머와
        // CLI 트리거 둘 다 이 함수 하나를 호출함(로직 중복 방지). span 전송을 여기 얹은 이유:
        // 이미 "주기적으로 WebServer에 밀어넣는" 책임을 지고 있는 함수라 새 타이머를 또
        // 만들 필요가 없음(2026-07-26 4순위 설계).
        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (!pendingMetrics.empty())
            {
                std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
                for (const auto& m : pendingMetrics)
                    webServerSender.Enqueue(m);

                pendingMetrics.clear();
            }

            auto spans = SpanRecorder::Instance().DrainAll();
            if (!spans.empty())
            {
                std::cout << "[Collector] WebServer로 span " << spans.size() << "건 전송 시도" << std::endl;
                for (const auto& s : spans)
                {
                    apm::TransactionSpan pkt;
                    pkt.set_operation_name(s.operationName);
                    pkt.set_duration_us(s.durationUs);
                    pkt.set_success(s.success);
                    webServerSender.Enqueue(pkt);
                }
            }
        };

        asio::steady_timer pushTimer(ioContext);
        std::function<void()> schedulePush;
        schedulePush = [&]()
        {
            pushTimer.expires_after(std::chrono::seconds(config.pushIntervalSeconds));
            pushTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        flushToWebServer();
                        schedulePush();
                    }
                });
        };
        schedulePush();

        // 로컬 저장소(store) 보존 정책 - pushTimer와 같은 패턴, 24시간 간격으로
        // 오래된 행 정리(TimescaleMetricStore는 내부적으로 no-op, SqliteMetricStore만 실제
        // DELETE 수행 - IMetricStore::Prune 문서 참고, 2026-07-26 3순위 설계).
        asio::steady_timer pruneTimer(ioContext);
        std::function<void()> schedulePrune;
        schedulePrune = [&]()
        {
            pruneTimer.expires_after(std::chrono::hours(24));
            pruneTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        store->Prune(config.metricsRetentionDays);
                        schedulePrune();
                    }
                });
        };
        schedulePrune();

        // Collector 콘솔에 "send"를 입력하면 즉시 전송. stdin 읽기는 블로킹이라 별도 스레드에서
        // 돌리고, 실제 전송(flushToWebServer)은 asio::post로 io_context 스레드에 넘김 -
        // pendingMetrics/webServerSender를 항상 단일 스레드에서만 건드리게 되어 락이 불필요함.
        std::thread cliThread(
            [&ioContext, &flushToWebServer]()
            {
                String line;
                while (std::getline(std::cin, line))
                {
                    if (line == "send")
                        asio::post(ioContext, flushToWebServer);
                }
            });
        cliThread.detach();

        std::cout << "Collector listening on port " << PORT << " (TLS)" << std::endl;
        std::cout << "[Collector] WebServer(" << config.webServerHost << ":" << config.webServerPort
            << ")로 " << config.pushIntervalSeconds << "초마다 전송 (콘솔에 'send' 입력 시 즉시 전송)" << std::endl;
        ioContext.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Collector] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

### 결정 사항

- 아직 파일 생성/수정 전 — `CLAUDE.md` 규칙대로 제안 단계. 사용자가 "적용해줘" 하면 신규 2개(`Common/WorkerQueue.h`/`.cpp`) + 수정 2개(`Common/CMakeLists.txt`, `Collector/main.cpp`) 반영 예정.
- 적용 후 검증 계획: `cmake --build build` 성공 확인 → `run_load_test.sh` 6단계 매트릭스 재실행(이번엔 크래시 없이 끝까지 도는지가 1차 확인 사항) → 결과를 1-5 베이스라인과 비교.

## 2026-07-29 — 콘솔 로깅 병목(300-agent 한정, 1-7-b에서 발견) 설계 제안

### 배경

`WORK_STATUS.md` 1-7-b 절에서 300-agent 재실측 중 새로 발견해 "후속 과제"로만 기록해뒀던 항목. 처리량이 늘자(같은 60초간 처리 로그가 9,625줄→90,276줄, 9.4배 증가) `PacketHandler::Register` 핸들러의 `std::cout << ... << std::endl`(메트릭 1건마다 동기 flush)이 새 병목으로 드러남 — 300-agent 재실측에서 `write` syscall이 전체 시간의 92%를 차지. `queue_drop=0`(유실 없음)이라 심각도는 낮게 기록해뒀으나, 이번 세션에 착수 요청 받아 설계 제안.

### 원인 분석

- `std::endl`은 개행 삽입에 더해 **매번 강제로 스트림을 flush**함(`std::flush` 호출과 동일). 리다이렉트된 파일(예: `run_load_test.sh`가 만드는 `collector_stdout.log`)로 출력할 때 원래라면 비-tty 대상엔 fully-buffered(통상 libc `BUFSIZ`=4096바이트 단위)가 적용돼 `write` syscall이 버퍼가 찰 때만 발생해야 하는데, `std::endl`이 그 버퍼링 이점을 매 줄마다 스스로 무효화시키고 있음.
- 이 로그 콜백은 `PacketHandler::Dispatch`를 거쳐 **네트워크 스레드(단일 `io_context` 스레드)**에서 실행됨 — 1-5~1-7에서 고쳤던 "SQLite `fdatasync`가 네트워크 스레드를 블로킹하던 문제"와 **같은 스레드, 같은 카테고리의 문제**(다만 원인은 디스크 fsync가 아니라 강제 스트림 flush).
- SQLite 케이스와 달리 이번엔 "어느 스레드가 블로킹되는가"가 아니라 "**불필요하게 자주 flush를 부르는가**"가 문제라, `WorkerQueue`처럼 별도 워커 스레드로 옮기는 방식(큐잉)까지는 불필요 — `std::endl` → `'\n'` 교체만으로 근본 원인이 해소됨.

### 제안 — `Collector/main.cpp` 수정 (수정 전 / 수정 후, 함수 전문)

`PacketHandler::Register<apm::Metric>(...)`에 넘기는 콜백 람다 전체(다른 `std::cout` 호출부— 연결 수립/주기 전송/시작 배너 등—는 호출 빈도가 메트릭 수신 대비 훨씬 낮아 이번 수정 대상에서 제외):

수정 전:
```cpp
        PacketHandler::Register<apm::Metric>(
            [storePtr, &metricStoreQueue, &pendingMetrics](const apm::Metric& pkt)
            {
                // Collector 안에서 "트랜잭션"이라 부를 만한 지점 중 가장 자연스러운 곳 -
                // Agent가 보낸 메트릭 패킷 하나를 받아 저장하는 구간(2026-07-26 4순위 데모 계측).
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                // store->Store(pkt) 직접 호출(동기, fdatasync 블로킹 포함) 대신 워커 스레드로 위임.
                // pkt은 값 복사로 캡처 - 비동기 실행 시점까지 살아있어야 함.
                metricStoreQueue.Push([storePtr, pkt]() { storePtr->Store(pkt); });

                pendingMetrics.push_back(pkt);
                // 저장이 이제 비동기라 이 시점엔 아직 안 끝났을 수 있음 - "stored"는 부정확한 표현이라 정정.
                std::cout << "[Collector] metric received: cpu=" << pkt.cpu_usage_percent()
                    << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                    << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                    << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                    << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                    << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                    << ",cwnd:" << pkt.tcp_snd_cwnd()
                    << std::endl;
            });
```

수정 후:
```cpp
        PacketHandler::Register<apm::Metric>(
            [storePtr, &metricStoreQueue, &pendingMetrics](const apm::Metric& pkt)
            {
                // Collector 안에서 "트랜잭션"이라 부를 만한 지점 중 가장 자연스러운 곳 -
                // Agent가 보낸 메트릭 패킷 하나를 받아 저장하는 구간(2026-07-26 4순위 데모 계측).
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                // store->Store(pkt) 직접 호출(동기, fdatasync 블로킹 포함) 대신 워커 스레드로 위임.
                // pkt은 값 복사로 캡처 - 비동기 실행 시점까지 살아있어야 함.
                metricStoreQueue.Push([storePtr, pkt]() { storePtr->Store(pkt); });

                pendingMetrics.push_back(pkt);
                // 저장이 이제 비동기라 이 시점엔 아직 안 끝났을 수 있음 - "stored"는 부정확한 표현이라 정정.
                // std::endl -> '\n' : 매 메트릭마다 강제 flush하던 걸 제거(1-7-b 300-agent 재실측에서
                // write syscall이 전체 시간의 92%를 차지한 원인). 개행만 넣고 flush는 libc 버퍼링에 위임 -
                // 리다이렉트 대상(파일/파이프)에선 자연히 fully-buffered(통상 4KB 단위)로 동작해 write
                // 호출 빈도가 줄어듦. 콘솔(tty)로 직접 볼 때는 libc가 line-buffered로 동작하므로 체감 차이 없음.
                std::cout << "[Collector] metric received: cpu=" << pkt.cpu_usage_percent()
                    << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                    << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                    << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                    << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                    << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                    << ",cwnd:" << pkt.tcp_snd_cwnd()
                    << '\n';
            });
```

### 트레이드오프 — 짚어둘 것

- 파일/파이프로 리다이렉트된 상태에서 프로세스가 비정상 종료(crash, `kill -9`)되면, 아직 flush 안 된 마지막 버퍼(최대 libc `BUFSIZ` 수준, 통상 4KB)만큼의 콘솔 로그 줄이 유실될 수 있음. 다만 이 로그는 순수 진단용이고, 실제 메트릭 데이터는 이와 무관하게 `metricStoreQueue` → SQLite 경로로 별도 영속화되므로 데이터 유실은 아님. `Collector`가 애초에 정상 종료 경로 없이 `kill`로만 종료되는 기존 관례(1-7 설계 시 이미 짚어둔 사항)와 궤를 같이함 — 새로 생기는 리스크가 아니라 기존 트레이드오프의 연장선.
- `std::cerr`(라인 108/115의 접속/에러 로그)는 원래 unbuffered라 이번 변경과 무관 — 그대로 둠.
- 별도 워커 큐로 로깅 자체를 옮기는 방안도 고려했으나, 문제의 본질이 "블로킹 syscall이 다른 스레드를 막는가"가 아니라 "불필요한 flush 빈도"라 큐잉은 과한 해법으로 판단, 채택 안 함.

### 검증 계획(적용 시)

1. `cmake --build build` 성공 확인(회귀 없음).
2. `strace -f -c`로 300-agent 시나리오 재실측 → 적용 전(`write` 9,270~90,276줄 대비 92% 시간) 대비 `write` syscall 횟수/시간 비교(1-7-d WAL 검증과 동일한 방법론).
3. `connect_success`/지연시간(p95/p99)에 유의미한 개선이 있는지 확인 — 1-7-b 기록상 이 병목의 심각도가 낮게(`queue_drop=0`) 평가돼 있어, 실측으로 실제 효과 크기를 확인하는 게 목적.

### 결정 사항

- 아직 파일 수정 전 — `CLAUDE.md` 규칙대로 제안 단계. 사용자가 "적용해줘" 하면 `Collector/main.cpp` 1줄(`std::endl` → `'\n'`) 반영 예정, 이후 위 검증 계획대로 빌드 + 300-agent strace 재실측.

## 2026-07-29 — 콘솔 로깅 병목: 2순위(워커 스레드 위임)로 우선순위 변경 + 1순위 일부 결합 설계

### 배경

바로 위 항목(1순위: `std::endl` → `'\n'` + `sync_with_stdio(false)`)을 사용자에게 브리핑한 뒤, 사용자가 "2순위(로깅을 워커 스레드로 위임)를 먼저 적용하는 게 맞아 보인다"고 판단 — 근거로 든 "flush를 안 하면 C++ 특성상 메모리 버퍼에 문제가 생길 것 같다"는 우려에 대해 정정 후, 그 정정 과정에서 드러난 실제 리스크를 반영해 설계를 다시 잡음.

### 정정 — "flush 생략 = 메모리 버퍼 문제"는 사실이 아님

`std::cout`의 내부 `streambuf` 버퍼는 고정 크기(libc 기준 통상 4KB)로, `std::endl` 대신 `'\n'`을 쓰더라도 버퍼가 무한정 커지지 않음 — 버퍼가 차면 라이브러리가 자동으로 flush(`overflow()`/`sync()`)함. 부작용은 (1) 출력 시점이 버퍼가 찰 때/프로그램 정상 종료 시까지 늦어짐, (2) 비정상 종료(`kill -9`, 크래시) 시 마지막 버퍼분(최대 4KB 수준)만 유실 가능 — 둘 다 이미 위 1순위 항목에서 짚어둔 트레이드오프와 동일.

### 실제 리스크 — 2순위를 "그대로"만 적용(std::endl 유지) 시 큐 적체로 인한 메모리 증가

`WorkerQueue`(`Common/WorkerQueue.h`/`.cpp`)는 내부적으로 `std::queue<std::function<void()>>`를 쓰며 **크기 제한이 없음**. 로깅을 워커 스레드로 옮기되 `std::endl`(강제 flush)을 그대로 둔다면 로그 1건 처리 비용 자체는 줄지 않음(1-7-d WAL 검증에서 이미 확인한 교훈과 동일 — 스레드 이동은 "누가 블로킹되는가"만 바꾸고 "비용의 크기"는 안 바꿈). 300-agent 시나리오(초당 ~1,500건 유입)에서 워커 스레드의 flush-포함 처리 속도가 유입 속도를 못 따라가면 `Push`된 작업이 큐에 무한정 쌓여 **실제로 메모리가 계속 증가하는 문제**가 생길 수 있음 — 사용자가 우려한 "메모리 버퍼 문제"가 발생하는 지점은 맞지만, 원인은 flush 생략이 아니라 **flush를 유지한 채 다른 스레드로만 옮기는 것**.

### 결론 — 2순위 + 1순위(`'\n'`) 결합

로깅을 워커 스레드로 위임(2순위, 네트워크 스레드 블로킹 제거)하되, 옮겨진 워커 작업 안에서도 `std::endl` 대신 `'\n'`을 사용(1순위 일부 결합, 워커 스레드 처리 비용 자체를 낮춰 큐 적체 위험을 줄임). `sync_with_stdio(false)`(이 코드베이스는 `std::cout`만 쓰고 C `printf`는 안 써서 순서 꼬임 리스크 없음)도 별도 비용 없이 같이 반영.

### 제안 — `Collector/main.cpp` 수정 (수정 전 / 수정 후, 함수 전문 — `main()`)

수정 전(현재 git에 커밋된 상태, `2d44c72`까지 반영됨):
```cpp
int main()
{
#ifdef _WIN32
    // 소스가 UTF-8(/utf-8)로 컴파일되는데 Windows 콘솔 기본 코드페이지(한글 Windows는 949)는
    // 그와 달라서, std::cout으로 찍는 한글 문자열이 콘솔에서 깨져 보임 - 출력 코드페이지를
    // UTF-8로 맞춰서 해결. Linux는 기본이 UTF-8이라 이 문제 자체가 없음.
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        // Agent<->Collector, Collector<->WebServer 두 구간 모두 AES-256-GCM(AesGcmPayload) -
        // 구간별 키는 분리 유지(한 쪽이 유출돼도 다른 구간은 안전). 배경: Docs/ARIA_TO_AES_MIGRATION.md
        AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");
        AesGcmCipher::Key webServerKey = LoadKeyFromHexFile("certs/webserver_aes.key");

        CollectorConfig config = LoadCollectorConfig("collector_config.json");

        auto store = CreateMetricStore(STORAGE_CONNECTION_INFO, config.metricsRetentionDays);

        // 1-7: SqliteMetricStore::Store()의 fdatasync가 네트워크 스레드를 막지 못하게, 저장 호출을
        // 전용 워커 스레드로 넘기는 큐. storePtr은 store(unique_ptr)가 main() 스코프 내내 살아있는
        // 것에 기대는 non-owning 포인터 - 워커 스레드도 main()이 끝나기 전까지만 존재하므로 안전.
        IMetricStore* storePtr = store.get();
        WorkerQueue metricStoreQueue;

        // Collector가 받은 뒤 아직 WebServer로 안 보낸 메트릭들 - 주기/CLI 트리거로 비워짐.
        std::vector<apm::Metric> pendingMetrics;

        asio::io_context ioContext;

        // Agent 접속을 받는 서버 역할 컨텍스트(기존)
        asio::ssl::context sslContext(asio::ssl::context::tls_server);
        sslContext.use_certificate_chain_file("certs/server.crt");
        sslContext.use_private_key_file("certs/server.key", asio::ssl::context::pem);

        // WebServer에 접속하는 클라이언트 역할 컨텍스트(신규) - Agent용과 모드가 달라 별도 필요.
        asio::ssl::context webServerSslContext(asio::ssl::context::tls_client);
        // 테스트용 자체 서명 인증서라 CA 검증 생략(Agent->Collector와 동일한 이유, 프로덕션 금지).
        webServerSslContext.set_verify_mode(asio::ssl::verify_none);

        ResilientSender webServerSender(ioContext, webServerSslContext,
            config.webServerHost, config.webServerPort,
            [webServerKey]() { return std::make_unique<AesGcmPayload>(webServerKey); });

        PacketHandler::Register<apm::Metric>(
            [storePtr, &metricStoreQueue, &pendingMetrics](const apm::Metric& pkt)
            {
                // Collector 안에서 "트랜잭션"이라 부를 만한 지점 중 가장 자연스러운 곳 -
                // Agent가 보낸 메트릭 패킷 하나를 받아 저장하는 구간(2026-07-26 4순위 데모 계측).
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                // store->Store(pkt) 직접 호출(동기, fdatasync 블로킹 포함) 대신 워커 스레드로 위임.
                // pkt은 값 복사로 캡처 - 비동기 실행 시점까지 살아있어야 함.
                metricStoreQueue.Push([storePtr, pkt]() { storePtr->Store(pkt); });

                pendingMetrics.push_back(pkt);
                // 저장이 이제 비동기라 이 시점엔 아직 안 끝났을 수 있음 - "stored"는 부정확한 표현이라 정정.
                std::cout << "[Collector] metric received: cpu=" << pkt.cpu_usage_percent()
                    << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                    << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                    << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                    << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                    << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                    << ",cwnd:" << pkt.tcp_snd_cwnd()
                    << std::endl;
            });

        asio::ip::tcp::acceptor acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), PORT));

        // 포트바인딩 : 특권이 필요할 수 있는 유일한 단계, 완료 직후 권한 하향
        PrivilegeDrop::DropTo("nobody");

        std::function<void()> doAccept;
        doAccept = [&]()
        {
            acceptor.async_accept(
                [&](const asio::error_code &ec, asio::ip::tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::cout << "[Collector] connection accepted" << std::endl;
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::cerr << "[Collector] accept error : " << ec.message() << std::endl;
                    }
                    doAccept();
                });
        };
        doAccept();

        // pendingMetrics/SpanRecorder에 쌓인 걸 전부 WebServer로 보내고 비움 - 주기 타이머와
        // CLI 트리거 둘 다 이 함수 하나를 호출함(로직 중복 방지). span 전송을 여기 얹은 이유:
        // 이미 "주기적으로 WebServer에 밀어넣는" 책임을 지고 있는 함수라 새 타이머를 또
        // 만들 필요가 없음(2026-07-26 4순위 설계).
        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (!pendingMetrics.empty())
            {
                std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
                for (const auto& m : pendingMetrics)
                    webServerSender.Enqueue(m);

                pendingMetrics.clear();
            }

            auto spans = SpanRecorder::Instance().DrainAll();
            if (!spans.empty())
            {
                std::cout << "[Collector] WebServer로 span " << spans.size() << "건 전송 시도" << std::endl;
                for (const auto& s : spans)
                {
                    apm::TransactionSpan pkt;
                    pkt.set_operation_name(s.operationName);
                    pkt.set_duration_us(s.durationUs);
                    pkt.set_success(s.success);
                    webServerSender.Enqueue(pkt);
                }
            }
        };

        asio::steady_timer pushTimer(ioContext);
        std::function<void()> schedulePush;
        schedulePush = [&]()
        {
            pushTimer.expires_after(std::chrono::seconds(config.pushIntervalSeconds));
            pushTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        flushToWebServer();
                        schedulePush();
                    }
                });
        };
        schedulePush();

        // 로컬 저장소(store) 보존 정책 - pushTimer와 같은 패턴, 24시간 간격으로
        // 오래된 행 정리(TimescaleMetricStore는 내부적으로 no-op, SqliteMetricStore만 실제
        // DELETE 수행 - IMetricStore::Prune 문서 참고, 2026-07-26 3순위 설계).
        asio::steady_timer pruneTimer(ioContext);
        std::function<void()> schedulePrune;
        schedulePrune = [&]()
        {
            pruneTimer.expires_after(std::chrono::hours(24));
            pruneTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        store->Prune(config.metricsRetentionDays);
                        schedulePrune();
                    }
                });
        };
        schedulePrune();

        // Collector 콘솔에 "send"를 입력하면 즉시 전송. stdin 읽기는 블로킹이라 별도 스레드에서
        // 돌리고, 실제 전송(flushToWebServer)은 asio::post로 io_context 스레드에 넘김 -
        // pendingMetrics/webServerSender를 항상 단일 스레드에서만 건드리게 되어 락이 불필요함.
        std::thread cliThread(
            [&ioContext, &flushToWebServer]()
            {
                String line;
                while (std::getline(std::cin, line))
                {
                    if (line == "send")
                        asio::post(ioContext, flushToWebServer);
                }
            });
        cliThread.detach();

        std::cout << "Collector listening on port " << PORT << " (TLS)" << std::endl;
        std::cout << "[Collector] WebServer(" << config.webServerHost << ":" << config.webServerPort
            << ")로 " << config.pushIntervalSeconds << "초마다 전송 (콘솔에 'send' 입력 시 즉시 전송)" << std::endl;
        ioContext.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Collector] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

수정 후(변경 지점 4곳 — ① `sync_with_stdio(false)` 추가, ② `consoleLogQueue` 선언 추가, ③ 람다 캡처 목록에 `&consoleLogQueue` 추가, ④ 로그 블록을 `consoleLogQueue.Push(...)`로 위임 + `std::endl`→`'\n'`. 나머지는 변경 없음):
```cpp
int main()
{
    // std::cout이 C stdio(printf 등)와 동기화되지 않게 함 - 이 코드베이스는 std::cout만 쓰고
    // C stdio는 안 써서 순서 꼬임 리스크 없음, << 연산의 불필요한 동기화 오버헤드만 제거.
    std::ios::sync_with_stdio(false);

#ifdef _WIN32
    // 소스가 UTF-8(/utf-8)로 컴파일되는데 Windows 콘솔 기본 코드페이지(한글 Windows는 949)는
    // 그와 달라서, std::cout으로 찍는 한글 문자열이 콘솔에서 깨져 보임 - 출력 코드페이지를
    // UTF-8로 맞춰서 해결. Linux는 기본이 UTF-8이라 이 문제 자체가 없음.
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        // Agent<->Collector, Collector<->WebServer 두 구간 모두 AES-256-GCM(AesGcmPayload) -
        // 구간별 키는 분리 유지(한 쪽이 유출돼도 다른 구간은 안전). 배경: Docs/ARIA_TO_AES_MIGRATION.md
        AesGcmCipher::Key agentCollectorKey = LoadKeyFromHexFile("certs/agent_collector_aes.key");
        AesGcmCipher::Key webServerKey = LoadKeyFromHexFile("certs/webserver_aes.key");

        CollectorConfig config = LoadCollectorConfig("collector_config.json");

        auto store = CreateMetricStore(STORAGE_CONNECTION_INFO, config.metricsRetentionDays);

        // 1-7: SqliteMetricStore::Store()의 fdatasync가 네트워크 스레드를 막지 못하게, 저장 호출을
        // 전용 워커 스레드로 넘기는 큐. storePtr은 store(unique_ptr)가 main() 스코프 내내 살아있는
        // 것에 기대는 non-owning 포인터 - 워커 스레드도 main()이 끝나기 전까지만 존재하므로 안전.
        IMetricStore* storePtr = store.get();
        WorkerQueue metricStoreQueue;

        // 콘솔 로깅 병목(1-7-b 300-agent 재실측에서 발견) 개선용 - std::cout 조립/출력 자체를
        // 네트워크 스레드에서 떼어내는 전용 워커 큐. metricStoreQueue와 분리한 이유: 로그 flush
        // 지연이 메트릭 저장(또는 그 반대)을 밀리게 하지 않도록 책임을 나눔.
        WorkerQueue consoleLogQueue;

        // Collector가 받은 뒤 아직 WebServer로 안 보낸 메트릭들 - 주기/CLI 트리거로 비워짐.
        std::vector<apm::Metric> pendingMetrics;

        asio::io_context ioContext;

        // Agent 접속을 받는 서버 역할 컨텍스트(기존)
        asio::ssl::context sslContext(asio::ssl::context::tls_server);
        sslContext.use_certificate_chain_file("certs/server.crt");
        sslContext.use_private_key_file("certs/server.key", asio::ssl::context::pem);

        // WebServer에 접속하는 클라이언트 역할 컨텍스트(신규) - Agent용과 모드가 달라 별도 필요.
        asio::ssl::context webServerSslContext(asio::ssl::context::tls_client);
        // 테스트용 자체 서명 인증서라 CA 검증 생략(Agent->Collector와 동일한 이유, 프로덕션 금지).
        webServerSslContext.set_verify_mode(asio::ssl::verify_none);

        ResilientSender webServerSender(ioContext, webServerSslContext,
            config.webServerHost, config.webServerPort,
            [webServerKey]() { return std::make_unique<AesGcmPayload>(webServerKey); });

        PacketHandler::Register<apm::Metric>(
            [storePtr, &metricStoreQueue, &pendingMetrics, &consoleLogQueue](const apm::Metric& pkt)
            {
                // Collector 안에서 "트랜잭션"이라 부를 만한 지점 중 가장 자연스러운 곳 -
                // Agent가 보낸 메트릭 패킷 하나를 받아 저장하는 구간(2026-07-26 4순위 데모 계측).
                APM_TRACE_SCOPE("Collector.HandleMetricPacket");

                // store->Store(pkt) 직접 호출(동기, fdatasync 블로킹 포함) 대신 워커 스레드로 위임.
                // pkt은 값 복사로 캡처 - 비동기 실행 시점까지 살아있어야 함.
                metricStoreQueue.Push([storePtr, pkt]() { storePtr->Store(pkt); });

                pendingMetrics.push_back(pkt);

                // 콘솔 로그 조립+출력을 네트워크 스레드에서 떼어내 별도 워커로 위임(2순위 채택).
                // std::endl 대신 '\n' 사용 - 워커 스레드 안에서도 매번 강제 flush하면 로그 1건
                // 처리 비용 자체는 안 줄어, 유입 속도가 처리 속도를 앞지를 때 WorkerQueue 내부
                // std::queue(무제한)에 처리 못 한 작업이 계속 쌓여 메모리가 늘어나는 리스크가
                // 있음(1-7-d WAL 검증 교훈과 동일 - 스레드 이동은 "누가 블로킹되는가"만 바꿈).
                // '\n'으로 워커 처리 비용 자체를 낮춰 큐 적체 위험을 줄임.
                // 저장이 이제 비동기라 이 시점엔 아직 안 끝났을 수 있음 - "stored"는 부정확한 표현이라 정정.
                consoleLogQueue.Push([pkt]()
                {
                    std::cout << "[Collector] metric received: cpu=" << pkt.cpu_usage_percent()
                        << "% mem=" << pkt.mem_used_bytes() << "/" << pkt.mem_total_bytes()
                        << " disk=" << pkt.disk_used_bytes() << "/" << pkt.disk_total_bytes()
                        << " net=rx:" << pkt.net_rx_bytes_per_sec() << "B/s,tx:" << pkt.net_tx_bytes_per_sec() << "B/s"
                        << " tcp=rtt:" << pkt.tcp_rtt_us() << "us,var:" << pkt.tcp_rtt_var_us() << "us"
                        << ",retrans:" << pkt.tcp_retransmits() << "(total:" << pkt.tcp_total_retrans() << ")"
                        << ",cwnd:" << pkt.tcp_snd_cwnd()
                        << '\n';
                });
            });

        asio::ip::tcp::acceptor acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), PORT));

        // 포트바인딩 : 특권이 필요할 수 있는 유일한 단계, 완료 직후 권한 하향
        PrivilegeDrop::DropTo("nobody");

        std::function<void()> doAccept;
        doAccept = [&]()
        {
            acceptor.async_accept(
                [&](const asio::error_code &ec, asio::ip::tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::cout << "[Collector] connection accepted" << std::endl;
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::cerr << "[Collector] accept error : " << ec.message() << std::endl;
                    }
                    doAccept();
                });
        };
        doAccept();

        // pendingMetrics/SpanRecorder에 쌓인 걸 전부 WebServer로 보내고 비움 - 주기 타이머와
        // CLI 트리거 둘 다 이 함수 하나를 호출함(로직 중복 방지). span 전송을 여기 얹은 이유:
        // 이미 "주기적으로 WebServer에 밀어넣는" 책임을 지고 있는 함수라 새 타이머를 또
        // 만들 필요가 없음(2026-07-26 4순위 설계).
        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (!pendingMetrics.empty())
            {
                std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
                for (const auto& m : pendingMetrics)
                    webServerSender.Enqueue(m);

                pendingMetrics.clear();
            }

            auto spans = SpanRecorder::Instance().DrainAll();
            if (!spans.empty())
            {
                std::cout << "[Collector] WebServer로 span " << spans.size() << "건 전송 시도" << std::endl;
                for (const auto& s : spans)
                {
                    apm::TransactionSpan pkt;
                    pkt.set_operation_name(s.operationName);
                    pkt.set_duration_us(s.durationUs);
                    pkt.set_success(s.success);
                    webServerSender.Enqueue(pkt);
                }
            }
        };

        asio::steady_timer pushTimer(ioContext);
        std::function<void()> schedulePush;
        schedulePush = [&]()
        {
            pushTimer.expires_after(std::chrono::seconds(config.pushIntervalSeconds));
            pushTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        flushToWebServer();
                        schedulePush();
                    }
                });
        };
        schedulePush();

        // 로컬 저장소(store) 보존 정책 - pushTimer와 같은 패턴, 24시간 간격으로
        // 오래된 행 정리(TimescaleMetricStore는 내부적으로 no-op, SqliteMetricStore만 실제
        // DELETE 수행 - IMetricStore::Prune 문서 참고, 2026-07-26 3순위 설계).
        asio::steady_timer pruneTimer(ioContext);
        std::function<void()> schedulePrune;
        schedulePrune = [&]()
        {
            pruneTimer.expires_after(std::chrono::hours(24));
            pruneTimer.async_wait(
                [&](const asio::error_code& ec)
                {
                    if (!ec)
                    {
                        store->Prune(config.metricsRetentionDays);
                        schedulePrune();
                    }
                });
        };
        schedulePrune();

        // Collector 콘솔에 "send"를 입력하면 즉시 전송. stdin 읽기는 블로킹이라 별도 스레드에서
        // 돌리고, 실제 전송(flushToWebServer)은 asio::post로 io_context 스레드에 넘김 -
        // pendingMetrics/webServerSender를 항상 단일 스레드에서만 건드리게 되어 락이 불필요함.
        std::thread cliThread(
            [&ioContext, &flushToWebServer]()
            {
                String line;
                while (std::getline(std::cin, line))
                {
                    if (line == "send")
                        asio::post(ioContext, flushToWebServer);
                }
            });
        cliThread.detach();

        std::cout << "Collector listening on port " << PORT << " (TLS)" << std::endl;
        std::cout << "[Collector] WebServer(" << config.webServerHost << ":" << config.webServerPort
            << ")로 " << config.pushIntervalSeconds << "초마다 전송 (콘솔에 'send' 입력 시 즉시 전송)" << std::endl;
        ioContext.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Collector] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

### 트레이드오프 — 짚어둘 것

- `consoleLogQueue`도 `metricStoreQueue`와 마찬가지로 소멸 시 큐를 다 비우고 `join()`(1-7-b 설계와 동일 패턴) - 정상 종료 시 유실 없음. `Collector`가 애초에 정상 종료 경로 없이 `kill`로만 종료되는 기존 관례(1-7에서 이미 짚어둔 사항)라 이 특성 자체는 새로 생기는 리스크가 아님.
- 워커 스레드가 하나 늘어남(`metricStoreQueue`용 1개 + `consoleLogQueue`용 1개, 총 2개) - 리소스 비용은 스레드 1개 수준으로 낮음.
- `'\n'`로 바꿔도 큐 적체 위험이 "이론적으로 0"이 되는 건 아님(유입 속도가 극단적으로 치솟으면 여전히 쌓일 수 있음) - 다만 300-agent 실측 범위에선 강제 flush 제거만으로 처리 비용이 크게 줄 것으로 예상(1-7-d WAL 검증에서 `fdatasync` 제거가 -97% 시간 절감을 보인 것과 유사한 성격). 실측으로 큐 깊이/드롭 여부까지 확인 필요.

### 검증 계획(적용 시)

1. `cmake --build build` 성공 확인(회귀 없음).
2. `strace -f -c`로 300-agent 재실측 → `write` syscall 횟수/시간, `connect_success`/지연시간(p95/p99) 비교(1-7-d와 동일 방법론).
3. (신규) 로그 큐 적체 여부 확인 — 필요시 `consoleLogQueue`에 현재 대기 중인 작업 수를 노출하는 간단한 카운터를 임시로 추가해 300-agent 시나리오 동안 큐 깊이가 발산하지 않고 수렴하는지 확인(선택 사항, 실측에서 이상 징후 있을 때만).

### 결정 사항

- 아직 파일 수정 전 — `CLAUDE.md` 규칙대로 제안 단계. 사용자가 "적용해줘" 하면 `Collector/main.cpp` 1개 파일(위 4곳) 반영 예정, 이후 위 검증 계획대로 빌드 + 300-agent strace 재실측.

### 적용 완료 (2026-07-29, "적용하자"로 명시 확인)

`Collector/main.cpp` 위 4곳 실제 반영. `cmake --build build` 성공, `ctest` 9/9 통과. `strace -f -c` 300-agent(ramp-up 5s) 재실측 결과와 WAL 적용 후 대비 비교 표는 `WORK_STATUS.md` 신규 "1-7-e" 절 참고 — 요약: connect_success 250/300→**300/300(전원)**, latency p95/p99 소폭 개선(19,890/26,044ms → 19,322/24,970ms, 더 많은 트래픽 처리하면서도), 단 워커 스레드가 1개→2개로 늘며 **futex 경합이 새 지배적 비용(18.19s→49.26s, 36.5%→57.22%)**으로 떠올라 전체 syscall 시간은 오히려 증가(49.81s→86.09s, 메트릭당 정규화 기준 0.345ms→0.501ms) — 1-7-d WAL 트레이드오프와 같은 패턴("한 비용을 줄이면 다른 형태로 비용이 늘 수 있다") 재확인. `queue_drop=0` 유지, 크래시 없음. 실측 산출물: `loadtest_results/straceF_300_afterLogFix_20260729_001310/`. 커밋 완료(`83f3761`).

## 2026-07-29 — 100-agent 재검증 중 발견한 버그: `sync_with_stdio(false)` + 다중 스레드 `cout` 동시 쓰기 레이스

### 배경

사용자가 "Collector 1대 : Agent 100개"를 성능 기준으로 삼고 싶다며 WAL+로깅 개선 적용 후 100-agent 규모에서 실제로 개선됐는지 확인을 요청. 100-agent `run_load_test.sh` 재실측 결과, 메인 스레드 전용 `strace -c` 요약에서 `write`가 여전히 62.36%(7.5초, 102,809회)로 지배적이어서 — 1-7-e에서 콘솔 로그를 `consoleLogQueue`로 옮겼는데도 왜 아직 이렇게 큰지 확인하려고 `strace -f`로 스레드별 실제 실행 위치를 직접 계측(임시 진단 코드: 각 작업 람다 안에 `std::cerr << "[DIAG] ... thread=" << std::this_thread::get_id()` 삽입).

### 발견 — 진단 로그 자체가 스레드 간 레이스로 깨짐

20-agent 진단 실행 결과, `consoleLogQueue`/`metricStoreQueue`/네트워크 스레드가 실제로 서로 다른 3개의 OS 스레드(`std::this_thread::get_id()` 값이 전부 다름)라는 건 확인됐으나, 진단 로그 자체가 이렇게 깨져 나옴:
```
[DIAG] log thread=[DIAG] store thread=131728022697664
[DIAG] log thread=[DIAG] log thread=[DIAG] store thread=131728014304960131728014304960
```
서로 다른 스레드가 같은 `std::cout`/`std::cerr`에 동시에 쓰면서 문자 단위로 뒤섞인 것 — 이론적 우려가 아니라 실제로 재현된 데이터 레이스.

### 원인 분석

1-7-e에서 `consoleLogQueue`(메트릭 로그 전용 워커 스레드) 분리 자체는 정확히 동작했지만(위 진단으로 스레드 ID가 실제로 다름을 확인), **네트워크 스레드가 여전히 `connection accepted`/`accept error`/`WebServer로 N건 전송 시도`(메트릭·span 2곳)를 직접 `std::cout`/`std::cerr`로 찍고 있었음** — 즉 두 스레드가 같은 스트림에 동시 접근. 1-7-e에서 함께 넣은 `sync_with_stdio(false)`가 std::cout을 C stdio의 내부 락(스레드 안전장치)에서 분리시켜, 이 동시 접근이 실제 레이스로 이어져 버퍼가 깨짐.

**영향 범위**: 콘솔 로그 텍스트 가독성 문제일 뿐 — SQLite에 저장되는 실제 메트릭 데이터, `queue_drop`, 이미 측정한 syscall 레벨 지표(write 횟수/시간, connect_success, 지연시간)에는 영향 없음(로그 내용과 무관한 지표들). 다만 방치하면 운영 중 콘솔 로그를 신뢰할 수 없음.

### 수정 — `Collector/main.cpp` (수정 전 / 수정 후)

**① `doAccept` 콜백** (accept 완료 핸들러):

수정 전:
```cpp
                    if (!ec)
                    {
                        std::cout << "[Collector] connection accepted" << std::endl;
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::cerr << "[Collector] accept error : " << ec.message() << std::endl;
                    }
```

수정 후:
```cpp
                    if (!ec)
                    {
                        // 네트워크 스레드가 cout을 직접 건드리지 않게 consoleLogQueue로 위임 -
                        // sync_with_stdio(false) 상태에서 여러 스레드가 동시에 cout/cerr에 쓰면
                        // 내부 버퍼가 레이스로 깨질 수 있음(재실측 중 발견, 진단 로그가 실제로
                        // 스레드 간 뒤섞여 깨지는 걸 확인). consoleLogQueue 워커 스레드 하나만
                        // 런타임 중 스트림을 쓰도록 통일해 레이스를 원천 차단.
                        consoleLogQueue.Push([]() { std::cout << "[Collector] connection accepted\n"; });
                        auto session = std::make_shared<ApmSession>(std::move(socket), sslContext, SessionMode::Server,
                            std::make_unique<AesGcmPayload>(agentCollectorKey));
                        session->Start(nullptr, nullptr, &PacketHandler::Dispatch);
                    }
                    else
                    {
                        std::string errMsg = ec.message();
                        consoleLogQueue.Push([errMsg]() { std::cerr << "[Collector] accept error : " << errMsg << '\n'; });
                    }
```

**② `flushToWebServer`** (전체 함수):

수정 전:
```cpp
        auto flushToWebServer = [&pendingMetrics, &webServerSender]()
        {
            if (!pendingMetrics.empty())
            {
                std::cout << "[Collector] WebServer로 " << pendingMetrics.size() << "건 전송 시도" << std::endl;
                for (const auto& m : pendingMetrics)
                    webServerSender.Enqueue(m);

                pendingMetrics.clear();
            }

            auto spans = SpanRecorder::Instance().DrainAll();
            if (!spans.empty())
            {
                std::cout << "[Collector] WebServer로 span " << spans.size() << "건 전송 시도" << std::endl;
                for (const auto& s : spans)
                {
                    apm::TransactionSpan pkt;
                    pkt.set_operation_name(s.operationName);
                    pkt.set_duration_us(s.durationUs);
                    pkt.set_success(s.success);
                    webServerSender.Enqueue(pkt);
                }
            }
        };
```

수정 후:
```cpp
        auto flushToWebServer = [&pendingMetrics, &webServerSender, &consoleLogQueue]()
        {
            if (!pendingMetrics.empty())
            {
                // consoleLogQueue로 위임(위 accept 핸들러와 같은 이유) - pendingMetrics는 이 직후
                // clear()되므로 크기를 미리 값으로 캡처(워커 스레드 실행 시점엔 이미 비어있을 수 있음).
                size_t count = pendingMetrics.size();
                consoleLogQueue.Push([count]() { std::cout << "[Collector] WebServer로 " << count << "건 전송 시도\n"; });
                for (const auto& m : pendingMetrics)
                    webServerSender.Enqueue(m);

                pendingMetrics.clear();
            }

            auto spans = SpanRecorder::Instance().DrainAll();
            if (!spans.empty())
            {
                size_t spanCount = spans.size();
                consoleLogQueue.Push([spanCount]() { std::cout << "[Collector] WebServer로 span " << spanCount << "건 전송 시도\n"; });
                for (const auto& s : spans)
                {
                    apm::TransactionSpan pkt;
                    pkt.set_operation_name(s.operationName);
                    pkt.set_duration_us(s.durationUs);
                    pkt.set_success(s.success);
                    webServerSender.Enqueue(pkt);
                }
            }
        };
```

startup 배너 2줄(`"Collector listening on port..."`, `"[Collector] WebServer(...)..."`)과 최상위 `catch`의 `std::cerr << "[Collector] fatal: ..."`는 그대로 둠 — 전자는 `ioContext.run()` 시작 전(아직 다른 스레드가 스트림에 안 씀), 후자는 예외로 프로세스가 곧 종료되는 경로라 동시 접근 위험이 낮음.

### 검증 완료(WSL, 2026-07-29)

- `cmake --build build` 성공, `ctest` 9/9 통과.
- 100-agent(interval 50ms, ramp 2s, 10초) 스트레스로 레이스 재현 여부 확인: 수정 후 81,381줄 전부 정상 접두어로 시작 — "metric received" 포함 줄(17,803건)과 `^[Collector] metric received`로 시작하는 줄(17,803건)이 정확히 일치, 깨진 줄 0건.
- 100-agent 표준 재실측(`run_load_test.sh 100`): connect_success 100/100, p95/p99 0/2,053ms — 레이스 수정 전(2,049ms)과 사실상 동일. **레이스 수정이 성능 지표 자체를 바꾸지 않음을 확인**(스레드 배치만 정리했을 뿐 총 작업량은 그대로라 예상된 결과).
- 300-agent `strace -f -c` 재검증: futex 57.78%(51.10s), write 13.15%(11.63s), 전체 88.45s, connect_success 300/300, 로그 98,600줄 전부 정상. 직전 1-7-e 보고값(futex 57.22%/49.26s, write 13.29%/11.44s, 전체 86.09s)과 오차범위 내로 일치 — **1-7-e에 이미 기록한 300-agent 비교 표 수치는 그대로 유효, 갱신 불필요**.

### 100-agent 기준 질문에 대한 결론

WAL+로깅 개선은 100-agent 규모에선 1-7-b(WorkerQueue)에서 이미 해소된 하드 리밋(72→100)에 **추가 이득을 주지 않음** — 오히려 p99가 소폭 늘어남(1,010ms→2,053ms), 워커 스레드가 1개→3개로 늘며 생기는 동기화 오버헤드로 보임(300-agent futex 경합 증가와 같은 패턴, 규모만 작을 뿐). 이 개선의 실질 효과는 300-agent 같은 고부하 구간(접속 성공 250→300)에 있음 — **100-agent를 기준으로 삼는다면 "이번 라운드 개선은 이 규모에선 순효과가 거의 없거나 근소하게 손해"가 정확한 결론**.

### 결정 사항

사용자가 "진행하자"로 명시 확인, `Collector/main.cpp` 위 2곳(4개 호출) 실제 반영 완료. 임시 진단 코드(`std::this_thread::get_id()` 출력)는 원인 확인 후 즉시 되돌림(커밋 대상 아님). 커밋 완료(`d664153`).

## 2026-07-29 — 시각 검증(`/apm/alerts`, `/apm/traces`) 중 발견한 버그 2건: EF Core + SQLite가 `DateTimeOffset` 비교를 SQL로 못 옮김

### 배경

사용자가 "프로젝트 완료로 평가하겠다, 문서화와 시각 검증을 동시에 진행하겠다"고 확정 — 그동안 빌드/단위테스트만 검증되고 실제로 브라우저에서 열어본 적 없던 `/apm/alerts`(2순위)/`/apm/traces`(5순위)를 이번에 처음 실행. `run` 스킬을 통해 Collector + APM_Console을 실제로 함께 띄우고 LoadTester로 실 트래픽을 흘려보내 검증하는 과정에서 두 가지가 연달아 크래시/500 에러로 드러남 — "빌드 성공 ≠ 동작"이라는 이 프로젝트의 기존 원칙(`Docs/PROJECT_TECHNICAL_REVIEW.md` §9)이 그대로 재현된 사례.

### 사전 조치 — 이관 후 방치된 설정 경로 수정

`APM_Console/src/ApmConsole.Host/appsettings.json`의 `ConnectionString`/`WebServerAesKeyPath`/`ReceiverCertPath`/`ReceiverKeyPath`가 옛 모노레포 경로(`/home/shkim/dev/gw2-cross/...`)로 남아있던 것(2026-07-26 3순위 작업 때부터 알려져 있던 항목, `WORK_STATUS.md`에 "실행 시 문제되면 별도로 손봐야 함"으로 기록해뒀던 것)을 이번에 `/home/shkim/dev/APM/...`로 전부 수정. `APM_Console/certs/webserver.crt`/`.key`도 이 저장소엔 없던 상태라 `certs/generate_webserver_cert.sh`로 새로 생성.

### 버그 A — `RetentionService.PruneAsync()`가 시작 즉시 전체 호스트를 크래시시킴

**증상**: `dotnet run`으로 APM_Console을 처음 띄우자마자(요청 한 번 안 받고) `Unhandled exception`으로 프로세스 자체가 죽음.

**원인**: `db.Metrics.Where(m => m.Ts < metricsCutoff).ExecuteDeleteAsync()`에서 `InvalidOperationException: The LINQ expression ... could not be translated`. `Ts`가 `DateTimeOffset`인데, 이 EF Core+SQLite 프로바이더 조합이 `DateTimeOffset` 비교(`<`)를 SQL로 번역하지 못함 — `ExecuteDeleteAsync`뿐 아니라 일반 `Where(...).ToListAsync()`조차 똑같이 실패(아래에서 확인). `RetentionService`는 `BackgroundService`로 등록돼 앱 시작 직후 실행되므로, 이 한 줄이 호스트 전체의 기동을 막음.

**수정 전** (`Common/RetentionService.cs` — 전체 `PruneAsync()`):
```csharp
    private async Task PruneAsync(CancellationToken ct)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        var metricsCutoff = DateTimeOffset.UtcNow.AddDays(-_metricsRetentionDays);
        var metricsDeleted = await db.Metrics
            .Where(m => m.Ts < metricsCutoff)
            .ExecuteDeleteAsync(ct);

        // 진행 중인 알림(ClosedAt == null)은 기간과 무관하게 항상 보존 - 오래됐다고 지우면
        // 활성 알림 목록에서 사라지는 버그가 됨(AlertsController.Index의 active 조회 기준과 동일).
        var alertsCutoff = DateTimeOffset.UtcNow.AddDays(-_alertRetentionDays);
        var alertsDeleted = await db.AlertRecords
            .Where(a => a.ClosedAt != null && a.ClosedAt < alertsCutoff)
            .ExecuteDeleteAsync(ct);

        // TransactionSpans도 Metrics와 같은 고빈도 원본 데이터라 같은 보존 기간을 적용
        // (2026-07-26 4순위 설계 - 새 설정값을 따로 만들지 않고 기존 정책을 자연스럽게 확장).
        var spansDeleted = await db.TransactionSpans
            .Where(s => s.Ts < metricsCutoff)
            .ExecuteDeleteAsync(ct);

        if (metricsDeleted > 0 || alertsDeleted > 0 || spansDeleted > 0)
            Console.WriteLine($"[RetentionService] 정리 완료: metrics {metricsDeleted}건, alerts {alertsDeleted}건, spans {spansDeleted}건 삭제");
    }
```

**시행착오**: 1차로 "조건에 맞는 PK만 `Select(Id)`로 뽑고 그 PK 목록으로 `ExecuteDeleteAsync`" 2단계 우회를 시도했으나, PK를 뽑는 첫 `Where(...).ToListAsync()` 자체가 똑같은 `InvalidOperationException`으로 실패 — `DateTimeOffset` 비교는 `ExecuteDelete` 한정 문제가 아니라 이 프로바이더에서 LINQ 비교 자체가 안 되는 것이었음이 이때 확인됨. 최종적으로 LINQ 번역을 아예 거치지 않는 파라미터화 raw SQL(`ExecuteSqlInterpolatedAsync`, 값은 파라미터 바인딩이라 인젝션 안전)로 우회.

**수정 후**:
```csharp
    private async Task PruneAsync(CancellationToken ct)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

        // SQLite EF Core 프로바이더가 DateTimeOffset 비교를 LINQ로 SQL 번역하지 못함
        // (InvalidOperationException, 2026-07-29 첫 실행 시 발견 - ExecuteDeleteAsync는 물론
        // 일반 Where(...).ToListAsync()조차 번역 실패. 그동안 빌드/단위테스트만 검증했지
        // 실제 실행은 안 해봐서 못 잡았던 버그). LINQ 번역을 아예 거치지 않는 파라미터화
        // raw SQL DELETE(ExecuteSqlInterpolatedAsync, 값은 파라미터 바인딩이라 인젝션 안전)로 우회.
        var metricsCutoff = DateTimeOffset.UtcNow.AddDays(-_metricsRetentionDays);
        var metricsDeleted = await db.Database.ExecuteSqlInterpolatedAsync(
            $"DELETE FROM Metrics WHERE Ts < {metricsCutoff}", ct);

        // 진행 중인 알림(ClosedAt == null)은 기간과 무관하게 항상 보존 - 오래됐다고 지우면
        // 활성 알림 목록에서 사라지는 버그가 됨(AlertsController.Index의 active 조회 기준과 동일).
        var alertsCutoff = DateTimeOffset.UtcNow.AddDays(-_alertRetentionDays);
        var alertsDeleted = await db.Database.ExecuteSqlInterpolatedAsync(
            $"DELETE FROM AlertRecords WHERE ClosedAt IS NOT NULL AND ClosedAt < {alertsCutoff}", ct);

        // TransactionSpans도 Metrics와 같은 고빈도 원본 데이터라 같은 보존 기간을 적용
        // (2026-07-26 4순위 설계 - 새 설정값을 따로 만들지 않고 기존 정책을 자연스럽게 확장).
        var spansDeleted = await db.Database.ExecuteSqlInterpolatedAsync(
            $"DELETE FROM TransactionSpans WHERE Ts < {metricsCutoff}", ct);

        if (metricsDeleted > 0 || alertsDeleted > 0 || spansDeleted > 0)
            Console.WriteLine($"[RetentionService] 정리 완료: metrics {metricsDeleted}건, alerts {alertsDeleted}건, spans {spansDeleted}건 삭제");
    }
```

**검증**: 재빌드 후 `dotnet run` → 정상 기동, 로그에 `DELETE FROM Metrics WHERE Ts < @p0` 등 3개 DELETE문이 파라미터 바인딩된 채 정상 실행되는 것을 EF Core 커맨드 로그로 직접 확인.

### 버그 B — `/apm/traces`가 항상 HTTP 500

**증상**: `/apm/traces` 접속 시 500. 원인이 같은 `DateTimeOffset` 비교 문제였는지 확인.

**원인**: `TracesController.Index()`의 `_db.TransactionSpans.Where(s => s.Ts >= cutoff).Select(...).ToListAsync()` — 버그 A와 정확히 같은 근본 원인(`Ts >= cutoff` 비교가 SQL 번역 안 됨).

**수정 전** (`Controllers/TracesController.cs` — 해당 쿼리 부분):
```csharp
        var spans = await _db.TransactionSpans
            .AsNoTracking()
            .Where(s => s.Ts >= cutoff)
            .Select(s => new { s.OperationName, s.DurationUs, s.Success })
            .ToListAsync();
```

**수정 후**: `Ts` 비교를 SQL로 안 보내고, `Ts`까지 포함해 전부 메모리로 가져온 뒤 필터링하도록 이동(원래 설계 코멘트에 있던 "OperationName+Ts 복합 인덱스가 이 WHERE에 맞는다"는 전제가 실제로는 안 맞았던 것 — 인덱스 활용은 포기하지만, 이 프로젝트 규모의 30일 치 span 개수로는 감내 가능한 트레이드오프로 판단):
```csharp
        var spans = (await _db.TransactionSpans
            .AsNoTracking()
            .Select(s => new { s.Ts, s.OperationName, s.DurationUs, s.Success })
            .ToListAsync())
            .Where(s => s.Ts >= cutoff)
            .ToList();
```

**검증**: 재빌드 후 `curl http://localhost:5299/apm/traces?window=1h|24h|7d` 전부 HTTP 200, 1h 창에서 `Collector.HandleMetricPacket` 실 데이터(LoadTester 5-agent 30초 실행분) 확인. `AlertsController`/`AlertEvaluator`는 `ClosedAt == null`/`!= null` 같은 동등 비교만 써서 이 버그의 영향을 안 받는다는 것도 코드 검색으로 확인(`/apm/alerts`는 수정 없이 정상 200).

### 결정 사항

두 파일(`RetentionService.cs`, `TracesController.cs`) 실제 반영 완료 — 시각 검증을 진행하려면 이 두 크래시를 먼저 고치지 않고는 페이지 자체를 열 수 없었으므로, 발견 즉시 수정(이 프로젝트의 기존 관례 "적용 중 발견해 그 자리에서 고침"과 동일). 문서(`Docs/PROJECT_TECHNICAL_REVIEW.md` 신규 버그 9/10, `README.md`, `WORK_STATUS.md`) 반영은 뒤이어 진행. 커밋은 사용자 요청 시.

---

## 2026-08-05 — `GW2_CrossPlatformCore/Thread/Lock.cpp`의 `WriteUnlock()`/`ReadLock()` 진짜 원인 재진단 + 수정

### 배경

1-7 재실측 중 발견했던 `LOCK_TIMEOUT` 크래시(2026-07-27, `WorkerQueue`로 우회 완료됨 — 위 §1-7-b/§1-7 관련 항목 참고)의 근본 원인을 사용자가 코드 직접 분석 세션에서 재검토. 기존엔 "`Lock::WriteUnlock()`이 소유 스레드 비트를 안 지우는 로직 버그"로만 기록돼 있었는데, 사용자가 "이 버그가 처음부터 있었다면 이미 여러 Windows 기반 실시간 게임 서버에 쓰였던 `JobQueue`+`Lock` 조합이 10분도 안 돼 크래시했을 것"이라며 "포팅 과정에서 문제가 생겼을 것"이라는 가설을 제시. 사용자가 원 모노레포 경로(`../gw2` = `/home/shkim/dev/gw2/GW2_Server/`)를 알려줘서 `GW2_ServerCore/Lock.cpp`(원본, Windows 전용)와 `GW2_CrossPlatformCore/Lock.cpp`(이관본)를 직접 `diff`.

### 원인 — `WriteUnlock()`과 `ReadLock()`의 함수 본문이 이관 중 뒤바뀜

`diff`로 확인한 원본(`GW2_ServerCore/Lock.cpp`, 실전 검증된 버전)의 `WriteUnlock()`/`ReadLock()`:

```cpp
void Lock::WriteUnlock(const char* name)
{
#if _DEBUG
	GDeadLockProfiler->PopLock(name);
#endif

	// ReadLock 안 풀린 상태면 WriteUnlock 불가능
	if ((_lockFlag.load() & READ_COUNT_MASK) != 0)
		CRASH("INVALID_UNLOCK_ORDER");

	const int32 lockCount = --_writeCount;
	if (lockCount == 0)
		_lockFlag.store(EMPTY_FLAG);
}

void Lock::ReadLock(const char* name)
{
#if _DEBUG
	GDeadLockProfiler->PushLock(name);
#endif

	// 이미 소유한 스레드면 재진입(write 락 보유 중 read도 허용)
	const uint32 lockThreadId = (_lockFlag.load() & WRITE_THREAD_MASK) >> 16;
	if (LThreadId == lockThreadId)
	{
		_lockFlag.fetch_add(1);
		return;
	}

	// CAS로 READ_COUNT_MASK 증가
	const int64 beginTick = ::GetTickCount64();
	while (true)
	{
		for (uint32 spinCount = 0; spinCount < MAX_SPIN_COUNT; spinCount++)
		{
			uint32 expected = (_lockFlag.load() & READ_COUNT_MASK);
			if (_lockFlag.compare_exchange_strong(OUT expected, expected + 1))
				return;
		}
		if (::GetTickCount64() - beginTick >= ACQUIRE_TIMEOUT_TICK)
			CRASH("LOCK_TIMEOUT");
		this_thread::yield();
	}
}
```

이관본(`GW2_CrossPlatformCore/Lock.cpp`, 수정 전)엔 `ReadLock()` 함수 자체가 통째로 없고, `WriteUnlock()`이라는 이름 아래 원래 `ReadLock()`의 본문이 들어가 있었음 — `LThreadId`(원본이 쓰던 GW2 프레임워크 전역 스레드 ID)를 이식 가능한 `GetThisThreadId()`로 바꾸는 리팩터링 도중 두 함수의 본문이 뒤바뀌어 붙여진 것으로 추정(순수 복사·붙여넣기 실수, OS API 차이와는 무관). 원래 `WriteUnlock()`의 진짜 해제 로직(`INVALID_UNLOCK_ORDER` 검사 + `--_writeCount` + `_lockFlag.store(EMPTY_FLAG)`)은 이 과정에서 완전히 유실됨. `diff`로 APM 저장소의 사본이 원 모노레포의 `GW2_CrossPlatformCore/Lock.cpp`와 바이트 단위로 동일함도 확인 — 이 버그는 APM으로 추출되기 전 원 모노레포 단계에서 이미 있었음.

### 수정 (`GW2_CrossPlatformCore/Thread/Lock.cpp`, APM 저장소 사본만 — `../gw2` 원본은 미수정)

**수정 전** (`WriteUnlock()`만 있고, 실제로는 `ReadLock()`의 본문):
```cpp
void Lock::WriteUnlock(const char* name)
{
#ifdef _DEBUG
	GDeadLockProfiler->PushLock(name);
#endif

	const uint32 threadId = GetThisThreadId();
	const uint32 lockThreadId = (_lockFlag.load() & WRITE_THREAD_MASK) >> 16;

	if ((threadId & 0xFFFF) == lockThreadId)
	{
		_lockFlag.fetch_add(1);
		return;
	}

	const auto beginTick = GetCurrentTick();

	while (true)
	{
		for (uint32 spinCount = 0; spinCount < MAX_SPIN_COUNT; ++spinCount)
		{
			uint32 expected = (_lockFlag.load() & READ_COUNT_MASK);
			if (_lockFlag.compare_exchange_strong(expected, expected + 1))
			{
				return;
			}

			if (GetCurrentTick() - beginTick >= ACQUIRE_TIMEOUT_TICK)
			{
				CRASH("LOCK_TIMEOUT");
			}
			this_thread::yield();
		}
	}
}
```

**수정 후** (`WriteUnlock()` 복원 + `ReadLock()` 신규 복원, `GetThisThreadId()`/`GetCurrentTick()` 이식 규약은 그대로 유지):
```cpp
void Lock::WriteUnlock(const char* name)
{
#ifdef _DEBUG
	GDeadLockProfiler->PopLock(name);
#endif

	// ReadLock이 아직 안 풀린 상태면 WriteUnlock 불가능
	if ((_lockFlag.load() & READ_COUNT_MASK) != 0)
	{
		CRASH("INVALID_UNLOCK_ORDER");
	}

	const int32 lockCount = --_writeCount;
	if (lockCount == 0)
	{
		_lockFlag.store(EMPTY_FLAG);
	}
}

void Lock::ReadLock(const char* name)
{
#ifdef _DEBUG
	GDeadLockProfiler->PushLock(name);
#endif

	const uint32 threadId = GetThisThreadId();
	const uint32 lockThreadId = (_lockFlag.load() & WRITE_THREAD_MASK) >> 16;

	if ((threadId & 0xFFFF) == lockThreadId)
	{
		_lockFlag.fetch_add(1);
		return;
	}

	const auto beginTick = GetCurrentTick();

	while (true)
	{
		for (uint32 spinCount = 0; spinCount < MAX_SPIN_COUNT; ++spinCount)
		{
			uint32 expected = (_lockFlag.load() & READ_COUNT_MASK);
			if (_lockFlag.compare_exchange_strong(expected, expected + 1))
			{
				return;
			}

			if (GetCurrentTick() - beginTick >= ACQUIRE_TIMEOUT_TICK)
			{
				CRASH("LOCK_TIMEOUT");
			}
			this_thread::yield();
		}
	}
}
```

**변경 사유**: `WriteUnlock()`이 소유 스레드 비트(`WRITE_THREAD_MASK`)를 절대 안 지우던 버그(1-7-b `LOCK_TIMEOUT` 크래시의 근본 원인)를 원본 로직대로 복원 — `_writeCount`를 실제로 감소시키고 0이 될 때만 `_lockFlag`를 `EMPTY_FLAG`로 되돌림. 동시에 통째로 유실됐던 `ReadLock()`(헤더엔 선언만 있고 정의가 없어 지금까지 아무도 호출 안 해서 링크 에러 없이 숨어있던 부분)도 원본대로 복원.

### 검증

- `cmake --build build --target GW2_CrossPlatformCore Collector Agent` → 전부 빌드 성공(무관한 기존 `ASIO_STANDALONE` 재정의 경고 1건 외 에러/신규 경고 없음).
- `ctest --test-dir build` → 9/9 통과(회귀 없음).
- **참고**: 현재 `Collector/main.cpp`는 1-7-b 이후 `JobQueue`/`Lock`을 아예 안 쓰고 `WorkerQueue`(자체 `std::mutex`/`condition_variable` 구현)로 대체된 상태라, 이 수정은 **현재 런타임 동작에는 영향이 없음** — `Thread/JobQueue`를 이 저장소에서 다시 쓰게 될 경우를 위한 정합성 수정.

### 결정 사항

`GW2_CrossPlatformCore/Thread/Lock.cpp` 수정은 사용자가 "우선 APM 아래에 있는 내용만 수정하자"로 범위를 명시 확정 — `../gw2`(별도 저장소) 쪽 원본은 미수정. `CODE_ARCHITECTURE.md` 반영 여부와 커밋 여부는 사용자 확인 대기.

---

## 2026-09-06 — Qt/MFC 트랙 착수: `APM_QtDashboard/` §6 0~1단계(빈 프로젝트 + signal/slot) 코드 제안

### 배경

`Docs/QT_MFC_PORTFOLIO_PLAN.md` §9 진행 순서 1번("Qt 설치 및 0~2단계")에 따라 착수. 환경 확인 결과 이 WSL에는 Qt5만 설치돼 있었고 Qt6는 미설치 — 사용자가 직접 `sudo apt install qt6-base-dev qt6-charts-dev qt6-websockets-dev libqt6sql6-sqlite qtcreator`로 설치 완료(Qt 6.4.2, Qt Creator 13.0.0). WSLg로 GUI 실행 가능함도 확인(`DISPLAY=:0`, `/tmp/.X11-unix/X0` 존재).

**한 차례 범위 착오 있었음**: 사용자가 "0단계부터 제가 대신 진행"이라 요청한 걸 "코드까지 전부 작성"으로 확대 해석해 `MainWindow.h/.cpp`/`main.cpp`/`CMakeLists.txt`를 실제로 만들고 빌드까지 했다가, 사용자가 "디렉터리 구조만 대신 부탁한 것"이라고 정정 — CLAUDE.md rule 2(코드는 사용자가 직접 작성) 원칙에 맞춰 소스 파일 전부 삭제, `APM_QtDashboard/`는 빈 디렉토리로 되돌림. 이 사건은 메모리에도 별도 기록(`feedback_claude_md_rule2_scope`).

**이 항목의 성격**: 아래 코드는 Claude가 직접 적용한 게 아니라 rule 3/4에 따른 **제안**이다 — 신규 파일 4개라 "수정 전"은 없음(빈 디렉토리), "수정 후" 전문만 제시. 실제 생성/빌드는 사용자가 Qt Creator(또는 CMake CLI)로 직접 진행.

### 제안 — `APM_QtDashboard/CMakeLists.txt` (신규)

```cmake
cmake_minimum_required(VERSION 3.20)
project(APM_QtDashboard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Q_OBJECT가 붙은 클래스는 moc(Meta-Object Compiler)가 signal/slot 디스패치 코드를
# 별도로 생성해줘야 링크가 됨 - AUTOMOC이 그 호출을 빌드 시점에 자동으로 끼워 넣는다.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(APM_QtDashboard
    main.cpp
    MainWindow.cpp
    MainWindow.h
)

target_link_libraries(APM_QtDashboard PRIVATE Qt6::Widgets)
```

**변경 사유**: `APM_Agent/CMakeLists.txt`와 같은 관례(3.20 최소, C++20, `CMAKE_EXPORT_COMPILE_COMMANDS ON`)를 그대로 따르되, Qt 프로젝트에만 필요한 `AUTOMOC`/`AUTOUIC`/`AUTORCC`를 추가. `AUTOMOC`이 핵심 — `Q_OBJECT` 매크로가 붙은 클래스는 컴파일러가 처리 못 하고 Qt의 moc가 signal/slot 디스패치용 코드를 별도 생성해줘야 링크가 되는데, 이 옵션이 그 호출을 CMake 빌드 그래프에 자동으로 끼워 넣어준다(수동으로 `qt6_wrap_cpp` 호출할 필요 없음).

### 제안 — `APM_QtDashboard/MainWindow.h` (신규)

```cpp
#pragma once

#include <QMainWindow>

class QLabel;
class QPushButton;

// §6 0~1단계 검증용 최소 창 - QPushButton::clicked 신호를 이 클래스의 슬롯에 연결해
// signal/slot 배선이 실제로 동작하는지(및 moc/AUTOMOC 빌드 경로) 확인하는 용도.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void OnButtonClicked();

private:
    QLabel* _clickCountLabel;
    QPushButton* _clickButton;
    int _clickCount = 0;
};
```

**변경 사유**: `Q_OBJECT` 매크로 하나가 signal/slot을 쓰기 위한 전제조건이라는 걸 가장 작은 예시로 보여주기 위해 별도 클래스로 뺐다(위젯 자체의 내장 `clicked()` 시그널만 갖고는 "이 프로젝트가 Q_OBJECT/moc를 실제로 거쳤다"는 걸 보여줄 수 없음 — 커스텀 슬롯이 있어야 moc 산출물이 실제로 링크에 들어감). 전방 선언(`class QLabel;`/`class QPushButton;`)으로 헤더의 include를 최소화 — 기존 저장소 관례(`Collector/main.cpp` 등)와 같은 방향.

### 제안 — `APM_QtDashboard/MainWindow.cpp` (신규)

```cpp
#include "MainWindow.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("APM Qt Dashboard - step 0/1");

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    _clickCountLabel = new QLabel("Clicked 0 times", central);
    _clickButton = new QPushButton("Click me", central);

    layout->addWidget(_clickCountLabel);
    layout->addWidget(_clickButton);
    setCentralWidget(central);

    connect(_clickButton, &QPushButton::clicked, this, &MainWindow::OnButtonClicked);
}

void MainWindow::OnButtonClicked()
{
    ++_clickCount;
    _clickCountLabel->setText(QString("Clicked %1 times").arg(_clickCount));
}
```

**변경 사유**: `connect(...)`의 새 함수 포인터 문법(`&QPushButton::clicked`, `&MainWindow::OnButtonClicked`)을 사용 — Qt5의 옛 문자열 기반 `SIGNAL()`/`SLOT()` 매크로 대신 컴파일 타임에 타입이 검사되는 방식이라 오탈자가 런타임이 아니라 빌드 타임에 걸린다(Qt6에서는 이 방식이 표준). 위젯 소유권은 전부 `central`을 부모로 넘겨(`new QLabel(..., central)`) Qt의 부모-자식 트리가 소멸을 자동 처리하도록 함 — `delete`를 직접 호출할 필요 없음.

### 제안 — `APM_QtDashboard/main.cpp` (신규)

```cpp
#include <QApplication>

#include "MainWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    MainWindow window;
    window.resize(320, 120);
    window.show();

    return app.exec();
}
```

**변경 사유**: Qt Widgets 앱의 표준 진입점 형태 그대로 — `QApplication`이 이벤트 루프·플랫폼 통합(WSLg 등)을 초기화하고, `app.exec()`가 그 이벤트 루프를 돌며 signal/slot 디스패치를 실제로 처리한다(`§3-3 ② QThread`와 같은 이벤트 루프 개념의 출발점).

### 검증(삭제 전 1회 확인, 참고용)

- `cmake -S . -B build` → configure 성공(Qt6 6.4.2 감지).
- `cmake --build build` → AUTOMOC이 `mocs_compilation.cpp.o`를 생성하고 `APM_QtDashboard` 바이너리 링크까지 성공.
- **GUI 실행/버튼 클릭 동작은 미확인** — 이 시점에 사용자가 범위 정정을 요청해 파일을 삭제, 실행 검증 전에 중단됨.

### 결정 사항

`APM_QtDashboard/`는 빈 디렉토리로 유지 — 위 4개 파일은 사용자가 Qt Creator(또는 CMake CLI)로 직접 작성. 실제 작성/빌드 후 실행까지 확인되면 §6 0~1단계 완료로 `WORK_STATUS.md`에 반영 예정.

---

## 2026-09-06 — Qt/MFC 트랙 §6 2단계 설계·코드 제안: SQLite(`webserver_apm.db`) 초기 데이터 표시

### 배경

사용자가 "2단계부터 미리 설계·코드 제안 준비해줘"라고 요청 — 0~1단계는 아직 사용자가 실제로 작성/빌드하기 전이지만, `Docs/QT_MFC_PORTFOLIO_PLAN.md` §9 진행 순서 1번("Qt 설치 및 0~2단계")대로 미리 준비해두는 것. 이 항목도 rule 3/4에 따른 **제안**이며 Claude가 직접 적용하지 않음 — `APM_QtDashboard/`는 여전히 빈 디렉토리.

**어느 DB를 읽을지부터 확인**: 이 저장소에는 SQLite 파일이 두 개 있고 스키마가 다르다.
- `APM_Agent/apm_metrics.db`(Collector 전용, `APM_Agent/Storage/SqliteMetricStore.cpp`) — `metrics` 테이블(소문자 스네이크케이스, `id` 없는 keyless 테이블, `ts`는 Unix epoch INTEGER)만 있고 **알림 테이블 자체가 없음**.
- `APM_Console/webserver_apm.db`(Console 전용, EF Core `ApmDbContext.cs`) — `Metrics`/`AlertRecords`/`AlertThresholds`/`TransactionSpans` 테이블 전부 있음.

§3-1에서 요구하는 화면 요소("임계값 초과 알림 표시")를 채우려면 알림 이력이 있는 `webserver_apm.db` 쪽이어야 한다 — 그래서 이번 제안은 이 DB를 대상으로 함. 연결 문자열은 `APM_Console/src/ApmConsole.Host/appsettings.json`의 `Apm:ConnectionString`(`Data Source=/home/shkim/dev/APM/APM_Console/webserver_apm.db`, 이 체크아웃 기준 절대경로 하드코딩)과 실제 파일(`python3 -c "import sqlite3; ..."`로 직접 스키마/PRAGMA 확인)을 대조해 확정.

**`Ts` 컬럼 함정 확인**: 실제 파일을 열어보면 `Metrics.Ts`/`AlertRecords.OpenedAt`은 .NET `DateTimeOffset.ToString()` 그대로 저장된 TEXT라 소수점 자릿수가 행마다 다르다(`'2026-07-28 17:07:57.0215884+00:00'` vs `'...17:08:06.92077+00:00'`) — `WORK_STATUS.md`에 기록된 "EF Core가 SQLite에서 `DateTimeOffset` 비교를 SQL로 못 옮기는 버그"와 같은 원인이다. 그래서 이번 설계는 **정렬 기준으로 `Ts`를 아예 안 쓰고 `Id`(AUTOINCREMENT, 삽입 순서와 항상 일치)를 쓴다** — Console 쪽이 겪은 문제를 Qt 쪽에서 처음부터 피해가는 설계 판단이고, 그대로 면접 답변이 된다.

**`webserver_apm.db`의 WAL 여부도 직접 재확인**(계획서 §3-2의 "WAL이라 안전하다" 주장을 이 DB 자체에 대해 재검증): `PRAGMA journal_mode` → `wal`, `PRAGMA synchronous` → `2`(NORMAL). 확인됨 — Qt가 두 번째 리더로 붙어도 안전.

### 제안 — `APM_QtDashboard/CMakeLists.txt` (수정)

**수정 전**(0~1단계 제안, 미적용):
```cmake
cmake_minimum_required(VERSION 3.20)
project(APM_QtDashboard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(APM_QtDashboard
    main.cpp
    MainWindow.cpp
    MainWindow.h
)

target_link_libraries(APM_QtDashboard PRIVATE Qt6::Widgets)
```

**수정 후**:
```cmake
cmake_minimum_required(VERSION 3.20)
project(APM_QtDashboard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql)

add_executable(APM_QtDashboard
    main.cpp
    MainWindow.cpp
    MainWindow.h
    MetricsRepository.cpp
    MetricsRepository.h
)

target_link_libraries(APM_QtDashboard PRIVATE Qt6::Widgets Qt6::Sql)
```

**변경 사유**: SQLite 조회에는 `Qt6::Sql` 모듈(`QSqlDatabase`/`QSqlQuery`)이 필요해 `find_package` 컴포넌트와 링크 대상에 추가. 신규 파일 `MetricsRepository.h/.cpp`를 소스 목록에 등록 — `Qt6Sql` 자체는 `Q_OBJECT`를 쓰지 않는 순수 C++ 클래스라 moc 대상은 아니지만, `add_executable` 소스 목록엔 포함시켜야 컴파일된다. 실행 시 SQLite 드라이버(`libqt6sql6-sqlite`)가 필요한데, 이는 이미 0~1단계 배경에서 사용자가 apt로 함께 설치해둔 상태(§6 0~1단계 항목 참고).

### 제안 — `APM_QtDashboard/MetricsRepository.h` (신규)

```cpp
#pragma once

#include <QString>
#include <QVector>
#include <qglobal.h>

// Metrics 테이블 한 행. 컬럼명은 APM_Console의 MetricRecord.cs(EF Core 엔티티)와
// 1:1 대응 - Mem/Disk는 바이트 단위 사용량/총량으로만 저장돼 있어(퍼센트 컬럼 없음)
// 표시 시점에 직접 계산해야 한다(Console의 MetricsReceiverService.EvaluateAlertsAsync
// 가 알림 판정할 때 하는 계산과 동일).
struct MetricSample
{
    qlonglong id = 0;
    QString ts;
    double cpuUsagePercent = 0.0;
    qlonglong memUsedBytes = 0;
    qlonglong memTotalBytes = 0;
    qlonglong diskUsedBytes = 0;
    qlonglong diskTotalBytes = 0;
    qlonglong netRxBytesPerSec = 0;
    qlonglong netTxBytesPerSec = 0;
};

// AlertRecords 테이블 한 행. metricType 값(0=Cpu,1=Memory,2=Disk,3=TcpRttUs)은
// APM_Console의 AlertMetricType enum(AlertThreshold.cs) 순서를 그대로 저장한 것 -
// 두 프로젝트 사이의 암묵적 데이터 계약이라 Console 쪽 enum 순서가 바뀌면 이
// 값의 의미도 같이 깨진다. ClosedAt이 NULL인 행만 가져오므로(=열려 있는 알림)
// 이 구조체엔 ClosedAt 필드 자체가 없다.
struct AlertSample
{
    qlonglong id = 0;
    int metricType = 0;
    double thresholdValue = 0.0;
    double triggerValue = 0.0;
    QString openedAt;
};

// APM_Console(웹서버)이 쓰는 SQLite(webserver_apm.db)를 읽기 전용으로 직접 연다.
// 원칙 1(기존 코어 무수정) - Console 프로세스와 이 파일을 코드 수정 없이 동시에
// 읽는다. 실제로 WAL 모드(journal_mode=wal, synchronous=NORMAL)인 걸 직접 확인했으므로
// 두 번째 리더로 붙어도 안전하다.
class MetricsRepository
{
public:
    explicit MetricsRepository(const QString& dbPath);
    ~MetricsRepository();

    bool Open();
    bool IsOpen() const;

    QVector<MetricSample> FetchLatestMetrics(int limit) const;
    QVector<AlertSample> FetchOpenAlerts(int limit) const;

private:
    QString _dbPath;
    QString _connectionName;
};
```

**변경 사유**: `MetricSample`/`AlertSample`을 `Q_OBJECT` 없는 순수 데이터 구조체로 뺀 것은 — 이 값들은 신호를 보내거나 슬롯을 가질 필요가 없는 "데이터"일 뿐이라 Qt의 메타오브젝트 오버헤드가 불필요하기 때문(③ Model/View 단계에서 `QAbstractTableModel`이 이 구조체를 그대로 내부 저장소로 재사용할 예정이라 지금부터 순수 값 타입으로 설계). `_connectionName`을 별도로 둔 이유는 `QSqlDatabase::addDatabase()`를 이름 없이 호출하면 전역 "default connection"을 등록하는데, 나중에 리포지토리를 두 개 이상(예: Collector용 DB도 같이 열어야 하는 경우) 쓰게 되면 서로 덮어써 버리는 문제가 생긴다 — 지금은 인스턴스가 하나뿐이라도 이름을 명시해 그 문제를 미리 차단.

### 제안 — `APM_QtDashboard/MetricsRepository.cpp` (신규)

```cpp
#include "MetricsRepository.h"

#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace
{
constexpr const char* kConnectionName = "apm_console_ro";
}

MetricsRepository::MetricsRepository(const QString& dbPath)
    : _dbPath(dbPath)
    , _connectionName(kConnectionName)
{
}

MetricsRepository::~MetricsRepository()
{
    // QSqlDatabase는 이름으로 전역 레지스트리에 등록되므로, 이 객체가 죽을 때
    // 명시적으로 지워주지 않으면 다음 실행에서 같은 이름으로 재등록할 때
    // "already exists" 경고가 뜬다.
    if (QSqlDatabase::contains(_connectionName))
    {
        QSqlDatabase::removeDatabase(_connectionName);
    }
}

bool MetricsRepository::Open()
{
    auto db = QSqlDatabase::addDatabase("QSQLITE", _connectionName);
    db.setDatabaseName(_dbPath);
    // 원칙 1(기존 코어 무수정)을 드라이버 레벨에서 강제 - 실수로 INSERT/UPDATE
    // 코드를 넣어도 여기서 막힌다. 파일이 아직 없으면(Console을 한 번도 안
    // 띄워서 DB가 안 만들어진 경우) READONLY라 open() 자체가 실패하는데, 이건
    // "파일이 없으니 새로 만든다"는 잘못된 동작보다 낫다 - 원칙 2(실패 경로 처리)
    // 대상이 되는 조건이다.
    db.setConnectOptions("QSQLITE_OPEN_READONLY");

    if (!db.open())
    {
        qWarning() << "MetricsRepository: failed to open" << _dbPath << db.lastError().text();
        return false;
    }
    return true;
}

bool MetricsRepository::IsOpen() const
{
    return QSqlDatabase::database(_connectionName, false).isOpen();
}

QVector<MetricSample> MetricsRepository::FetchLatestMetrics(int limit) const
{
    QVector<MetricSample> result;

    QSqlQuery query(QSqlDatabase::database(_connectionName));
    // Ts는 .NET DateTimeOffset의 TEXT 직렬화라 소수점 자릿수가 행마다 달라
    // 문자열 정렬 기준으로 쓰면 같은 초 안에서 순서가 어긋날 수 있다(Console
    // 쪽 EF Core가 이 컬럼 비교 자체를 SQL로 못 옮겨 별도 우회를 뒀던 것과 같은
    // 원인 - WORK_STATUS.md 참고). Id는 AUTOINCREMENT라 삽입 순서와 항상
    // 일치하므로 정렬 기준으로 대신 쓴다.
    query.prepare(
        "SELECT Id, Ts, CpuUsagePercent, MemUsedBytes, MemTotalBytes, "
        "DiskUsedBytes, DiskTotalBytes, NetRxBytesPerSec, NetTxBytesPerSec "
        "FROM Metrics ORDER BY Id DESC LIMIT ?");
    query.addBindValue(limit);

    if (!query.exec())
    {
        qWarning() << "MetricsRepository::FetchLatestMetrics failed:" << query.lastError().text();
        return result;
    }

    while (query.next())
    {
        MetricSample sample;
        sample.id = query.value(0).toLongLong();
        sample.ts = query.value(1).toString();
        sample.cpuUsagePercent = query.value(2).toDouble();
        sample.memUsedBytes = query.value(3).toLongLong();
        sample.memTotalBytes = query.value(4).toLongLong();
        sample.diskUsedBytes = query.value(5).toLongLong();
        sample.diskTotalBytes = query.value(6).toLongLong();
        sample.netRxBytesPerSec = query.value(7).toLongLong();
        sample.netTxBytesPerSec = query.value(8).toLongLong();
        result.push_back(sample);
    }
    return result;
}

QVector<AlertSample> MetricsRepository::FetchOpenAlerts(int limit) const
{
    QVector<AlertSample> result;

    QSqlQuery query(QSqlDatabase::database(_connectionName));
    query.prepare(
        "SELECT Id, MetricType, ThresholdValue, TriggerValue, OpenedAt "
        "FROM AlertRecords WHERE ClosedAt IS NULL ORDER BY Id DESC LIMIT ?");
    query.addBindValue(limit);

    if (!query.exec())
    {
        qWarning() << "MetricsRepository::FetchOpenAlerts failed:" << query.lastError().text();
        return result;
    }

    while (query.next())
    {
        AlertSample sample;
        sample.id = query.value(0).toLongLong();
        sample.metricType = query.value(1).toInt();
        sample.thresholdValue = query.value(2).toDouble();
        sample.triggerValue = query.value(3).toDouble();
        sample.openedAt = query.value(4).toString();
        result.push_back(sample);
    }
    return result;
}
```

**변경 사유**: `WHERE ClosedAt IS NULL`로 "열려 있는 알림"만 가져오는 것은 Console의 `AlertRecord` 설계(별도 severity/isActive 컬럼 없이 `ClosedAt IS NULL` 여부로만 열림/닫힘을 구분)를 그대로 따른 것 — 이 컬럼 하나로 상태를 표현하는 설계 자체가 Console 쪽 코드(`ApmDbContext.cs`)를 실제로 읽어야만 알 수 있는 부분이라 별도로 짚어둠. `query.prepare()`+`addBindValue()`로 `limit`을 바인딩한 것은 SQL 인젝션을 막기 위한 습관 — 지금은 사용자 입력이 아니라 상수라 실질적 위험은 없지만, `APM_Console` 쪽이 `ExecuteSqlInterpolatedAsync`(파라미터화된 보간 문자열)로 인젝션을 막은 것과 같은 습관을 Qt 쪽에도 처음부터 들이는 것.

### 제안 — `APM_QtDashboard/MainWindow.h` (수정)

**수정 전**(0~1단계 제안, 미적용):
```cpp
#pragma once

#include <QMainWindow>

class QLabel;
class QPushButton;

// §6 0~1단계 검증용 최소 창 - QPushButton::clicked 신호를 이 클래스의 슬롯에 연결해
// signal/slot 배선이 실제로 동작하는지(및 moc/AUTOMOC 빌드 경로) 확인하는 용도.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void OnButtonClicked();

private:
    QLabel* _clickCountLabel;
    QPushButton* _clickButton;
    int _clickCount = 0;
};
```

**수정 후**:
```cpp
#pragma once

#include <QMainWindow>
#include <QVector>

#include "MetricsRepository.h"

class QPushButton;
class QTableWidget;

// §6 2단계 - SQLite(webserver_apm.db)에서 초기 데이터를 읽어 표시하는 최소 창.
// 아직 QThread로 분리하지 않았으므로(§6 3단계에서 분리 예정) 조회는 UI 스레드에서
// 동기로 실행된다 - 지금은 데이터가 늘어나면 화면이 잠깐 멈추는 것도 의도적으로
// 남겨두고, 다음 단계에서 QThread로 옮기면서 그 전/후 차이를 직접 보여줄 것이다.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void OnRefreshClicked();

private:
    void LoadData();
    void PopulateMetricsTable(const QVector<MetricSample>& samples);
    void PopulateAlertsTable(const QVector<AlertSample>& alerts);

    MetricsRepository _repository;
    QTableWidget* _metricsTable;
    QTableWidget* _alertsTable;
    QPushButton* _refreshButton;
};
```

**변경 사유**: 0~1단계의 클릭 카운터(`_clickCountLabel`/`_clickButton`/`_clickCount`)는 "signal/slot 배선 자체가 동작하는지"만 확인하는 더미였고, 그 목적은 이제 "새로고침" 버튼(실제로 DB를 다시 읽어 화면을 갱신하는 진짜 동작)이 대신하므로 제거 — 더미를 남겨둘 이유가 없다. `_repository`를 포인터가 아니라 값 멤버로 둔 것은 이 창이 살아있는 동안 리포지토리도 항상 같이 존재해야 하고 별도로 소유권을 옮기거나 null을 가질 이유가 없기 때문(불필요한 동적 할당 회피).

### 제안 — `APM_QtDashboard/MainWindow.cpp` (수정)

**수정 전**(0~1단계 제안, 미적용):
```cpp
#include "MainWindow.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("APM Qt Dashboard - step 0/1");

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    _clickCountLabel = new QLabel("Clicked 0 times", central);
    _clickButton = new QPushButton("Click me", central);

    layout->addWidget(_clickCountLabel);
    layout->addWidget(_clickButton);
    setCentralWidget(central);

    connect(_clickButton, &QPushButton::clicked, this, &MainWindow::OnButtonClicked);
}

void MainWindow::OnButtonClicked()
{
    ++_clickCount;
    _clickCountLabel->setText(QString("Clicked %1 times").arg(_clickCount));
}
```

**수정 후**:
```cpp
#include "MainWindow.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
// APM_Console의 appsettings.json Apm:ConnectionString과 같은 파일을 가리켜야 한다.
// 이 체크아웃 기준 절대경로를 하드코딩 - Console 쪽도 지금 절대경로 하드코딩
// 상태라 이식성 문제가 새로 생기는 건 아니다. 배포판을 만들 때(§6 8단계)
// 커맨드라인 인자나 설정 파일로 뺄 것.
const QString kConsoleDbPath = "/home/shkim/dev/APM/APM_Console/webserver_apm.db";

// AlertRecords.MetricType 값 순서는 APM_Console의 AlertMetricType enum과 반드시
// 일치해야 한다(AlertThreshold.cs) - 두 프로젝트 사이의 암묵적 데이터 계약.
QString MetricTypeToString(int metricType)
{
    switch (metricType)
    {
    case 0: return "CPU";
    case 1: return "Memory";
    case 2: return "Disk";
    case 3: return "TCP RTT";
    default: return QString("Unknown(%1)").arg(metricType);
    }
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , _repository(kConsoleDbPath)
{
    setWindowTitle("APM Qt Dashboard - step 2 (SQLite initial load)");

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    _refreshButton = new QPushButton("새로고침", central);

    _metricsTable = new QTableWidget(central);
    _metricsTable->setColumnCount(6);
    _metricsTable->setHorizontalHeaderLabels(
        {"Id", "Ts", "CPU %", "Mem %", "Disk %", "Net Rx/Tx (B/s)"});
    _metricsTable->horizontalHeader()->setStretchLastSection(true);
    _metricsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

    _alertsTable = new QTableWidget(central);
    _alertsTable->setColumnCount(5);
    _alertsTable->setHorizontalHeaderLabels(
        {"Id", "Metric", "Threshold", "Trigger", "Opened At"});
    _alertsTable->horizontalHeader()->setStretchLastSection(true);
    _alertsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

    layout->addWidget(_refreshButton);
    layout->addWidget(_metricsTable);
    layout->addWidget(_alertsTable);
    setCentralWidget(central);

    connect(_refreshButton, &QPushButton::clicked, this, &MainWindow::OnRefreshClicked);

    if (!_repository.Open())
    {
        setWindowTitle(windowTitle() + " - DB open failed (see stderr)");
        return;
    }
    LoadData();
}

void MainWindow::OnRefreshClicked()
{
    LoadData();
}

void MainWindow::LoadData()
{
    PopulateMetricsTable(_repository.FetchLatestMetrics(20));
    PopulateAlertsTable(_repository.FetchOpenAlerts(20));
}

void MainWindow::PopulateMetricsTable(const QVector<MetricSample>& samples)
{
    _metricsTable->setRowCount(samples.size());
    for (int row = 0; row < samples.size(); ++row)
    {
        const MetricSample& s = samples[row];
        const double memPercent = s.memTotalBytes > 0
            ? s.memUsedBytes * 100.0 / s.memTotalBytes
            : 0.0;
        const double diskPercent = s.diskTotalBytes > 0
            ? s.diskUsedBytes * 100.0 / s.diskTotalBytes
            : 0.0;

        _metricsTable->setItem(row, 0, new QTableWidgetItem(QString::number(s.id)));
        _metricsTable->setItem(row, 1, new QTableWidgetItem(s.ts));
        _metricsTable->setItem(row, 2, new QTableWidgetItem(QString::number(s.cpuUsagePercent, 'f', 1)));
        _metricsTable->setItem(row, 3, new QTableWidgetItem(QString::number(memPercent, 'f', 1)));
        _metricsTable->setItem(row, 4, new QTableWidgetItem(QString::number(diskPercent, 'f', 1)));
        _metricsTable->setItem(row, 5, new QTableWidgetItem(
            QString("%1 / %2").arg(s.netRxBytesPerSec).arg(s.netTxBytesPerSec)));
    }
}

void MainWindow::PopulateAlertsTable(const QVector<AlertSample>& alerts)
{
    _alertsTable->setRowCount(alerts.size());
    for (int row = 0; row < alerts.size(); ++row)
    {
        const AlertSample& a = alerts[row];
        _alertsTable->setItem(row, 0, new QTableWidgetItem(QString::number(a.id)));
        _alertsTable->setItem(row, 1, new QTableWidgetItem(MetricTypeToString(a.metricType)));
        _alertsTable->setItem(row, 2, new QTableWidgetItem(QString::number(a.thresholdValue, 'f', 1)));
        _alertsTable->setItem(row, 3, new QTableWidgetItem(QString::number(a.triggerValue, 'f', 1)));
        _alertsTable->setItem(row, 4, new QTableWidgetItem(a.openedAt));
    }
}
```

**변경 사유**: `Mem %`/`Disk %`는 DB에 퍼센트 컬럼이 없어(바이트 단위 사용량/총량만 있음) 표시 시점에 직접 계산 — Console의 `MetricsReceiverService.EvaluateAlertsAsync`가 알림 판정 때 하는 계산과 같은 방식이라 두 프로젝트의 계산 결과가 어긋나지 않는다. `setEditTriggers(QAbstractItemView::NoEditTriggers)`는 읽기 전용 원칙(원칙 1)을 화면에서도 지키기 위함 — 테이블 셀을 더블클릭해서 수정하는 게 DB에 반영되진 않지만(별도 `submit()`을 안 부르므로), 애초에 "고칠 수 있는 것처럼 보이는" UI를 안 만드는 게 맞다. `_repository.Open()` 실패 시 즉시 리턴하고 제목표시줄에 실패를 노출한 것은 원칙 2(실패 경로 처리) — MFC Viewer가 "DB 없으면 재시도 대기중"을 보여준 것과 같은 방향, 다만 이 단계에선 재시도 로직까지는 넣지 않고 실패를 눈에 보이게만 함(재시도는 §6 6단계 "연결 끊김 처리"에서 다룰 항목).

### 제안 — `APM_QtDashboard/main.cpp` (수정)

**수정 전**(0~1단계 제안, 미적용):
```cpp
#include <QApplication>

#include "MainWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    MainWindow window;
    window.resize(320, 120);
    window.show();

    return app.exec();
}
```

**수정 후**:
```cpp
#include <QApplication>

#include "MainWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    MainWindow window;
    window.resize(720, 480);
    window.show();

    return app.exec();
}
```

**변경 사유**: 창 크기를 320x120(버튼 하나짜리 데모)에서 720x480으로 키운 것뿐 — 테이블 두 개를 붙였으니 최소한의 가독성을 위해. 그 외 진입점 구조는 0~1단계와 동일(QApplication 이벤트 루프 초기화 → MainWindow 생성 → exec()).

### 검증

**미검증** — 0~1단계와 마찬가지로 코드만 제안한 상태이고 `APM_QtDashboard/`는 아직 빈 디렉토리라 컴파일해본 적 없음. 사용자가 0~1단계부터 실제로 작성/빌드/실행 확인을 마친 뒤, 이 2단계 코드를 이어 붙이고 다음을 확인해야 함:
- `find_package(Qt6 REQUIRED COMPONENTS Widgets Sql)`가 성공하는지(0~1단계 배경에서 `libqt6sql6-sqlite`까지 이미 설치했다고 기록돼 있어 드라이버 자체는 준비돼 있을 것).
- `APM_Console/webserver_apm.db`가 실제로 존재하는 상태에서(Console을 한 번이라도 띄워야 EF Core `EnsureCreated()`가 파일을 만듦) 실행해 테이블에 실제 행이 뜨는지.
- DB 파일이 없는 상태로 실행했을 때 `Open()`이 깨끗하게 실패하고 창 제목에 실패 문구가 뜨는지(원칙 2 검증).

### 결정 사항

이 제안도 적용하지 않고 문서로만 남김 — 사용자가 0~1단계를 먼저 실제로 만들고 빌드/실행까지 확인한 뒤, 이 2단계 코드를 참고해 직접 작성. 이어서 필요하면 3단계(QThread 워커 분리) 설계·코드 제안도 같은 방식으로 미리 준비 가능.

## 2026-09-07 — Qt/MFC 트랙 0~1단계: 사용자 작성 코드의 오타 수정 (컴파일 불가 → 성공)

### 배경

사용자가 `APM_QtDashboard/`에 0~1단계 코드(`main.cpp`, `MainWindow.h`, `MainWindow.cpp`, `CMakeLists.txt`)를 실제로 작성함. "오타 때문에 컴파일이 안 된다"며 오타만 수정해달라고 명시적으로 요청 — CLAUDE.md 원칙 2의 예외("수정해라"에 해당)로 판단해 코드 파일을 직접 수정.

### 수정 — MainWindow.h (클래스 선언 전체)

**수정 전**:
```cpp
#pragma once
#include <QMainWindow>

class QLabel;
class QPushButton;

// 검증용 최소 창 : QPushButton::clicked 신호를 이 클래스의 슬롯에 연결
class MainWindow : public QMainWinodw
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void OnButtonClicked();

private:
    QLabel* _clickCountLabel;
    QPushButton* _clickButton;
    int _clickCount = 0;
}
```

**수정 후**:
```cpp
#pragma once
#include <QMainWindow>

class QLabel;
class QPushButton;

// 검증용 최소 창 : QPushButton::clicked 신호를 이 클래스의 슬롯에 연결
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void OnButtonClicked();

private:
    QLabel* _clickCountLabel;
    QPushButton* _clickButton;
    int _clickCount = 0;
};
```

**변경 사유**: (1) `QMainWinodw`는 존재하지 않는 타입명(`QMainWindow`의 오타) — "does not name a type" 컴파일 에러 발생. (2) 클래스 정의 끝 `}`에 세미콜론이 빠져 있어, 뒤에 오는 다른 선언까지 문법 오류로 전파되는 전형적인 실패 패턴.

### 수정 — MainWindow.cpp (생성자 함수 전문)

**수정 전**:
```cpp
#include "MainWindow.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget* parent) : QMainWindw(parent)
{
    setWindowTitle("APM Qt Dashboard - step 0/1");

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    _clickCountLabel = new QLabel("Clicked 0 times", central);
    _clickButton = new QPushButton("Click me", central);

    layout->addWidget(_clickCountLabel);
    layout->addWidget(_clickButton);
    setCentralWidget(central);

    connect(_clickButton, &QPushButton::clickeds, this, &MainWindow::OnButtonClicked);
}

void MainWindow::OnButtonClicked()
{
    ++_clickCount;
    _clickCountLabel->setText(QString("Clicked %1 times").arg(_clickCount));
}
```

**수정 후**:
```cpp
#include "MainWindow.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle("APM Qt Dashboard - step 0/1");

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    _clickCountLabel = new QLabel("Clicked 0 times", central);
    _clickButton = new QPushButton("Click me", central);

    layout->addWidget(_clickCountLabel);
    layout->addWidget(_clickButton);
    setCentralWidget(central);

    connect(_clickButton, &QPushButton::clicked, this, &MainWindow::OnButtonClicked);
}

void MainWindow::OnButtonClicked()
{
    ++_clickCount;
    _clickCountLabel->setText(QString("Clicked %1 times").arg(_clickCount));
}
```

**변경 사유**: (1) 베이스 클래스 초기화 목록의 `QMainWindw`도 동일한 오타(`QMainWindow`). (2) `&QPushButton::clickeds`는 존재하지 않는 시그널이라 `connect()`가 `&QPushButton::clicked` 오버로드를 찾지 못해 컴파일 에러.

### 검증 — 실제 빌드 확인

`CMakeLists.txt`는 오타 없이 정상 상태였음(`find_package(Qt6 REQUIRED COMPONENTS Widgets)`, `CMAKE_AUTOMOC ON` 등).

```
$ cd APM_QtDashboard/build && cmake --build .
[  0%] Built target APM_QtDashboard_autogen_timestamp_deps
[ 20%] Automatic MOC and UIC for target APM_QtDashboard
[ 40%] Building CXX object CMakeFiles/APM_QtDashboard.dir/APM_QtDashboard_autogen/mocs_compilation.cpp.o
[ 60%] Building CXX object CMakeFiles/APM_QtDashboard.dir/main.cpp.o
[ 80%] Building CXX object CMakeFiles/APM_QtDashboard.dir/MainWindow.cpp.o
[100%] Linking CXX executable APM_QtDashboard
[100%] Built target APM_QtDashboard
```

빌드 성공을 직접 확인함. 실행 파일 실행(WSLg 환경에서 GUI 창 표시)까지는 사용자에게 안내만 하고 이 세션에서 직접 실행하지는 않음:
```bash
cd /home/shkim/dev/APM/APM_QtDashboard/build
./APM_QtDashboard
```
"Click me" 버튼을 누를 때마다 라벨이 "Clicked N times"로 증가하는지가 1단계(Signal/Slot) 검증 기준.

### 결정 사항

오타 수정은 CLAUDE.md 원칙 2 예외(명시적 "수정해달라" 요청)에 해당해 직접 편집함. 빌드까지는 이 세션에서 확인 완료, 실제 GUI 실행/클릭 동작 확인은 사용자 몫으로 남김. 실행까지 문제없이 확인되면 `WORK_STATUS.md`의 Qt/MFC 트랙 상태를 "코딩 미시작" → "0~1단계 완료"로 갱신 필요.

## 2026-09-09 — Qt/MFC 트랙 2단계 진행 상황 점검: `MetricsRepository.cpp`에서 발견된 컴파일 차단 버그 (분석만, 미수정)

### 배경

사용자가 "남은 작업 브리핑" 요청. `APM_QtDashboard/`를 디스크에서 다시 읽어 2단계 진행 상황을 점검(원칙 7). `MainWindow.h`/`MetricsRepository.h`는 사용자가 직접 2단계 형태로 작성 중이고, `MetricsRepository.cpp`도 사용자가 직접 작성 완료한 상태. 리뷰 결과 이대로는 컴파일이 안 되는 오타/버그 5건을 발견 — CLAUDE.md 원칙 2에 따라 **직접 수정하지 않고 분석만 기록**.

### 발견 1 — `#include` 오타 (파일을 못 찾음)

**현재 코드** (`MetricsRepository.cpp` 1번째 줄):
```cpp
#include "MetricsfRepository.h"
```

**문제**: 실제 헤더 파일명은 `MetricsRepository.h`인데 `MetricsfRepository.h`로 "f"가 하나 더 들어가 있음. `#include` 처리 시 해당 이름의 파일을 못 찾아 "No such file or directory"로 컴파일 자체가 시작도 못 함(가장 먼저 걸리는 오류).

**수정 방향**: `#include "MetricsRepository.h"`로 정정.

### 발견 2 — `Open()` 함수: `setDatanaseName` 오타 + `qWarning` 괄호 누락

**현재 코드** (`MetricsRepository.cpp`, `Open()` 함수 전문):
```cpp
bool MetricsRepository::Open()
{
    auto db = QSqlDatabase::addDatabase("QSQLITE", _connectionName);
    db.setDatanaseName(_dbPath);

    // 기존 코어 무수정 원칙 구현 : 여기서 Insert/Update 쿼리를 막아버린다
    // 필요없는 체계의 훼손을 막는다
    db.setConnectionOptions("QSQLITE_OPEN_READONLY");

    if(!db.open())
    {
        qWarning << "MetricsRepository : failed to Open" << _dbPath << db.lastError().text();
        return false;
    }

    return true;
}
```

**문제**:
1. `db.setDatanaseName(_dbPath);` — `QSqlDatabase`에 그런 멤버 함수는 없음. 실제 함수는 `setDatabaseName`("Datanase"가 "Database"의 오타). "no member named 'setDatanaseName'" 컴파일 에러.
2. `qWarning << ...` — `qWarning`은 `QDebug`를 반환하는 **함수**인데 괄호 없이 함수 이름 자체에 `<<`를 시도함(함수 포인터에 `operator<<`를 적용하려는 꼴이라 매칭되는 연산자가 없어 컴파일 에러). 65번째 줄과 100번째 줄에서는 `qWarning()`으로 올바르게 괄호를 붙였는데 이 자리만 빠짐.

**수정 방향**:
```cpp
bool MetricsRepository::Open()
{
    auto db = QSqlDatabase::addDatabase("QSQLITE", _connectionName);
    db.setDatabaseName(_dbPath);

    // 기존 코어 무수정 원칙 구현 : 여기서 Insert/Update 쿼리를 막아버린다
    // 필요없는 체계의 훼손을 막는다
    db.setConnectionOptions("QSQLITE_OPEN_READONLY");

    if(!db.open())
    {
        qWarning() << "MetricsRepository : failed to Open" << _dbPath << db.lastError().text();
        return false;
    }

    return true;
}
```

### 발견 3 — `IsOpen()` 함수: `.IsOpen()` 대소문자 오타

**현재 코드** (`MetricsRepository.cpp`, `IsOpen()` 함수 전문):
```cpp
bool MetricsRepository::IsOpen() const
{
    return QSqlDatabase::database(_connectionName, false).IsOpen();
}
```

**문제**: `QSqlDatabase`의 실제 멤버 함수는 첫 글자가 소문자인 `isOpen()`(Qt 자체 API는 camelCase, 첫 글자 소문자 — 이 프로젝트 자체 클래스의 `MetricsRepository::IsOpen()`처럼 PascalCase로 만든 건 이 저장소 관례이지만, Qt 내장 클래스의 함수명까지 그 관례를 따라 대문자로 바꿔 부를 수는 없음). "no member named 'IsOpen' in 'QSqlDatabase'" 컴파일 에러.

**수정 방향**:
```cpp
bool MetricsRepository::IsOpen() const
{
    return QSqlDatabase::database(_connectionName, false).isOpen();
}
```

### 발견 4 — `FetchLatestMetrics()` 함수: `MetricSample` 구조체명 오타

**현재 코드** (`MetricsRepository.cpp`, `FetchLatestMetrics()` 함수 전문):
```cpp
QVector<MetricsSample> MetricsRepository::FetchLatestMetrics(int limit) const
{
    QVector<MetricsSample> result;

    QSqlQuery query(QSqlDatabase::database(_connectionName));
    // Ts는 .NET DataTimeOffset의 TEXT 직렬화라 소숫점 자리가 행마다 다르다.
    // 따라서 문자열 정렬 기준으로 쓰면 같은 초 안에서 순서가 어긋날 수 있다.

    query.prepare(
        "SELECT Id, Ts, CpuUsagePercent, MemUsedBytes, MemTotalBytes, "
        "DiskUsedBytes, DiskTotalBytes, NetRxBytesPerSec, NetTxBytesPerSec "
        "FROM Metrics ORDER BY Id DESC LIMIT ?");
    query.addBindValue(limit);

    if(!query.exec())
    {
        qWarning() << "MetricsRepository::FetchLatestMetrics failed : " << query.lastError().text();
        return result;
    }

    // 리눅스에서 터미널 명령어를 이용해 결과 메시지를 받아올 때랑 같은 이유로 while을 사용
    // query.next()에서 가져온 결과 메시지의 다음줄이 있는지를 확인 있으면 반복하는 것
    while (query.next())
    {
        MetricSample sample;
        sample.id = query.value(0).toLongLong();
        sample.ts = query.value(1).toString();
        sample.cpuUsagePercent = query.value(2).toDouble();
        sample.memUsedBytes = query.value(3).toLongLong();
        sample.memTotalBytes = query.value(4).toLongLong();
        sample.diskUsedBytes = query.value(5).toLongLong();
        sample.diskTotalBytes = query.value(6).toLongLong();
        sample.netRxBytesPerSec = query.value(7).toLongLong();
        sample.netTxBytesPerSec = query.value(8).toLongLong();
        result.push_back(sample);
    }
    return result;
}
```

**문제**: 함수 시그니처/`result` 선언은 `MetricsSample`(맞는 이름, `MetricsRepository.h`의 실제 구조체명)을 쓰는데, 반복문 안 지역 변수 선언만 `MetricSample`(s 빠짐)로 돼 있음. 그런 타입은 어디에도 선언돼 있지 않으므로 "unknown type name 'MetricSample'" 컴파일 에러.

**수정 방향**: `MetricSample sample;` → `MetricsSample sample;`로 정정(그 아래 필드 접근 코드는 전부 정확해서 이 한 줄만 고치면 됨).

### 발견 5 — 헤더/구현부 함수 이름 불일치: `FetchOpenAlerts` vs `FetchOpenAlert`

**현재 코드** (`MetricsRepository.h`, 해당 선언):
```cpp
QVector<AlertSample> FetchOpenAlerts(int limit) const;
```

**현재 코드** (`MetricsRepository.cpp`, 구현부 함수 전문):
```cpp
QVector<AlertSample> MetricsRepository::FetchOpenAlert(int limit) const
{
    QVector<AlertSample> result;

    QSqlQuery query(QSqlDatabase::database(_connectionName));
    query.prepare(
        "SELECT Id, MetricType, ThresholdValue, TriggerValue, OpenedAt "
        "FROM AlertRecords WHERE ClosedAt IS NULL ORDER BY Id DESC LIMIT ?");
    query.addBindValue(limit);

    if(!query.exec())
    {
        qWarning() << "MetricsRepository::FetchOpenAlerts failed : " << query.lastError().text();
        return result;
    }

    while(query.next())
    {
        AlertSample sample;
        sample.id = query.value(0).toLongLong();
        sample.metricType = query.value(1).toInt();
        sample.thresholdValue = query.value(2).toDouble();
        sample.triggerValue = query.value(3).toDouble();
        sample.openedAt = query.value(4).toString();
        result.push_back(sample);
    }
    return result;
}
```

**문제**: 헤더는 `FetchOpenAlerts`(복수형, s로 끝남)로 선언했는데 구현부는 `FetchOpenAlert`(단수형)로 정의함. C++ 입장에서는 클래스에 선언되지 않은 새 함수를 정의하려는 것으로 보여 "out-of-line definition does not match any declaration" 컴파일 에러가 나고, 동시에 헤더가 선언한 `FetchOpenAlerts`는 정의가 없는 상태로 남음(나중에 `MainWindow`에서 호출하면 그건 그것대로 링크 에러).

**수정 방향**: 둘 중 하나로 통일 — 구현부를 헤더와 맞춰 `MetricsRepository::FetchOpenAlerts(int limit) const`로 정정하는 쪽을 권장(2단계 제안 문서 원안이 복수형).

### 검증

**미검증** — 5건 모두 코드 읽기로 발견한 정적 분석 결과이고 실제 빌드는 시도하지 않음(원칙 2 — 코드 직접 수정 안 함). 사용자가 위 5곳을 직접 고친 뒤 `cmake --build .`로 재확인 필요. 이 5건을 전부 고쳐도 여전히 남아있는 별도 이슈(직전 답변에서 이미 안내함): `MainWindow.h`의 include 누락/`QTableWidget` forward decl 누락/`_repository` 멤버 초기화, `CMakeLists.txt`의 `find_package`에 `Sql` 컴포넌트 누락, `MainWindow.cpp`가 아직 0~1단계 코드 그대로라 2단계 UI로 재작성 필요, `main.cpp`의 `resize()`가 아직 720x480으로 안 바뀜.

### 결정 사항

이번에도 분석만 하고 코드는 건드리지 않음 — 사용자가 직접 고치는 게 원칙(CLAUDE.md 원칙 2). 다음 세션에서 "현황 파악"하면 이 5건이 고쳐졌는지 디스크에서 다시 확인.

## 2026-09-09 — 2단계 CMake 에러 재현: `Qt6::Sql` 타겟 없음 (위 "참고" 항목이 실제로 발생)

### 배경

사용자가 `cmake build` 실행 시 아래 에러를 받아 공유:
```
CMake Error at CMakeLists.txt:24 (target_link_libraries):
  Target "APM_QtDashboard" links to:

    Qt6::Sql

  but the target was not found.
```
바로 위 항목("검증" 절)에서 이미 예견했던 문제 그대로 — `find_package`에 `Sql` 컴포넌트를 안 넣었는데 `target_link_libraries`가 `Qt6::Sql`을 요구해서 발생.

### 제안 — CMakeLists.txt (수정)

**수정 전**:
```cmake
find_package(Qt6 REQUIRED COMPONENTS Widgets)
```

**수정 후**:
```cmake
find_package(Qt6 REQUIRED COMPONENTS Widgets Sql)
```

**변경 사유**: `find_package(... COMPONENTS ...)`에 나열된 모듈만 그 모듈의 CMake 임포트 타겟(`Qt6::위젯이름`)이 생성된다. `Sql`을 안 넣었으니 `Qt6::Sql`이라는 타겟 자체가 존재하지 않는 상태였고, `target_link_libraries`가 그 존재하지 않는 타겟을 요구해서 "target was not found" 에러가 남.

### 참고 — 명령어 자체 오타

`cmake build`가 아니라 `cmake --build build`(빌드 디렉터리 안에서는 `cmake --build .`)가 맞음. `cmake build`는 "build"라는 이름의 소스 디렉터리를 구성(configure)하라는 뜻이라 지금 로그가 사실은 재구성(configure) 단계 출력임 — 우연히 `build/` 안에 이미 있던 `CMakeCache.txt`를 그대로 재사용하면서 에러만 표시된 것으로 보임.

### 검증

미검증 — 사용자가 위 한 줄을 고친 뒤 `cmake --build build`(또는 `build/`로 이동 후 `cmake --build .`)로 재확인 필요.

### 결정 사항

분석만 하고 코드는 건드리지 않음(원칙 2). 이 한 줄을 고쳐도 지난 항목의 나머지 미해결 사항(`MetricsRepository.cpp` 오타 5건, `MainWindow.h`/`.cpp`/`main.cpp` 관련 사항)이 남아있으므로 순차적으로 처리 필요.

