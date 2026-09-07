# CODE_ARCHITECTURE.md — 코드 분석 노트

> 사용자가 직접 코드를 분석하기 위한 질의응답 기록. 실제 디스크의 코드(2026-08-03 기준)를
> 직접 읽어 확인한 내용만 담는다 — 추측/일반론 금지.

---

## 1. 직접 분석할 때 추천 순서

의존성 방향을 따라가는 게 헷갈리지 않는다.

1. **공유 계약** — 세 프로세스(Agent/Collector/Console)가 공통으로 전제하는 것부터:
   - `APM_Agent/Protocol/Metric.proto` — 메시지 정의, 필드
   - `APM_Agent/Common/ApmSession.h`의 `struct PacketHeader` — 프레이밍(size, id)
   - `APM_Agent/Common/AesGcmPayload.h` — 페이로드 암호화 와이어 포맷
2. **가장 단순한 진입점** — `APM_Agent/Agent/main.cpp`(약 70줄) + `Common/ResourceCollector.{h,cpp}`
   (실제 OS 지표 수집이 일어나는 곳) + `Common/MetricScheduler.{h,cpp}`(주기 실행)
3. **가장 복잡한 진입점** — `APM_Agent/Collector/main.cpp` + `Common/WorkerQueue.{h,cpp}` +
   `Common/PacketHandler.{h,cpp}` + `Common/ApmSession.cpp` + `Common/ResilientSender.{h,cpp}`
4. **소비자 관점 재확인용** — `APM_Agent/LoadTester/` (Agent인 척 트래픽을 만드는 코드라
   프로토콜을 다른 각도에서 다시 검증하기 좋음)
5. **.NET 수신 측** — `APM_Console/.../ApmConsole.Host/Program.cs`(플러그인 로더) →
   `Infrastructure/MetricsReceiverService.cs`(Collector의 `PacketHandler::Dispatch`에 대응) →
   `Controllers/`

---

## 2. Collector — 프로세스 내 생명 유지 방식

**먼저 정정할 오해**: "성능 수집"(CPU/메모리/디스크/네트워크 실측) 자체는 **Collector가 아니라
Agent가 한다**(`Common/ResourceCollector.cpp`, Linux는 `/proc` 파싱, Windows는 WinAPI).
Collector는 그 결과물(패킷)을 **수신 → 로컬 SQLite 저장 → Console로 중계**하는 역할이다.
이름 때문에 "Collector가 수집한다"고 오해하기 쉬운 부분.

### 스레드 구성 (`Collector/main.cpp` 기준, 총 4개)

```
[메인 스레드]      ioContext.run()  — accept, TLS 핸드셰이크, recv, 타이머 콜백 전부 처리
[워커 스레드 1]    metricStoreQueue — storePtr->Store(pkt) 만 순차 실행 (SQLite, fdatasync 포함)
[워커 스레드 2]    consoleLogQueue  — std::cout/cerr 조립+출력만 실행
[cliThread]        std::cin 블로킹 read — "send" 입력 감시
```

### 왜 죽지 않고 유지되는가 — 4개 스레드 전부 "자연 종료 불가" 상태

`io_context::run()`은 내부적으로 미결(outstanding) 비동기 작업 수를 세다가 그게 0이 되면
반환하는데, Collector 코드는 이 수가 절대 0이 안 되게 짜여 있다:

```cpp
// main.cpp — accept 콜백이 성공/실패 상관없이 항상 자기 자신을 재호출
std::function<void()> doAccept;
doAccept = [&]()
{
    acceptor.async_accept(
        [&](const asio::error_code &ec, asio::ip::tcp::socket socket)
        {
            if (!ec) { /* 세션 생성 */ }
            else { /* 에러 로그 */ }
            doAccept();   // ← 여기, 무조건 재호출
        });
};
```

```cpp
// ApmSession.cpp — recv도 성공 시 자기 자신을 재호출, self가 세션을 계속 살려둠
void ApmSession::RegisterRecv()
{
    auto self = shared_from_this();
    _sslStream.async_read_some(asio::buffer(_recvBuffer),
        [this, self](const asio::error_code& ec, size_t bytes)
        {
            if (ec) { NotifyDisconnected(); return; }   // 에러 시에만 재호출 안 함
            _accumulated.insert(...); ProcessAccumulated();
            RegisterRecv();   // ← 여기, 성공하면 무조건 재호출
        });
}
```

`pushTimer`/`pruneTimer`도 콜백 끝에서 `schedulePush()`/`schedulePrune()`을 스스로 재예약한다.
그 결과 미결 작업 수가 항상 최소 3건(accept 1 + timer 2) 이상 유지되고, 접속 중인 세션마다
1건씩 더 늘어난다 — `run()`의 반환 조건이 정상 흐름에서 절대 성립하지 않는다.

워커 스레드 2개는 io_context와 무관하게 각자:
```cpp
// WorkerQueue.cpp
void WorkerQueue::WorkerLoop()
{
    while (true)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait(lock, [this]() { return _stop || !_jobs.empty(); });
        if (_jobs.empty()) return;   // _stop이어도 남은 job은 마저 처리
        job = std::move(_jobs.front()); _jobs.pop();
        // ... job() 실행
    }
}
```
에 블로킹돼 있고, `_stop`은 오직 `WorkerQueue` 소멸자에서만 true가 된다 — 그 소멸자는
`main()`이 끝나야(=`ioContext.run()` 반환 후) 호출되므로 메인 스레드가 안 빠져나오는 한
영원히 대기. `cliThread`는 `std::getline(std::cin, line)` 블로킹 read에 갇혀 있다.

**4개 스레드 전부 "다시 걸어놓는 콜백" 또는 "조건 안 풀리는 블로킹 대기"뿐이라, 프로세스가
살아있는 한 하나도 자연 종료되지 않는다.**

### 실제로 프로세스가 끝나는 3가지 경로

1. **외부 kill**(`SIGINT`/`SIGTERM`/`SIGKILL`) — `signal`/`SIGINT`/`SIGTERM` 핸들러가 코드
   어디에도 없어(grep 확인) OS 기본 동작으로 즉시 종료. `WorkerQueue` 소멸자(`join()`으로
   큐 flush)조차 안 거치고 죽어 미처리 작업(저장 안 된 메트릭, 안 찍힌 로그) 유실 가능.
2. **`main()`의 `try`/`catch (const std::exception&)`까지 예외가 올라오는 경우** —
   `"[Collector] fatal: ..."` 출력 후 `return 1`. 연결/패킷 처리 예외는 `ApmSession` 내부에서
   이미 잡히므로(`ProcessAccumulated`의 try/catch), 실질적으론 초기화 단계(키/설정 파일 로드
   실패) 예외 정도만 이 경로를 탄다.
3. **크래시** — 과거 `JobQueue`+`GW2_CrossPlatformCore/Thread/Lock.cpp`의 `Lock::WriteUnlock()`
   버그로 인한 `LOCK_TIMEOUT` 크래시가 있었으나(교차 스레드 사용 시 재현), `WorkerQueue`(자체
   `std::mutex`/`condition_variable` 구현, 위 코드)로 교체하며 해소됨. 현재 코드 경로상 알려진
   크래시 요인 없음.

---

## 3. 패킷 구성과 전송 (Agent → Collector 기준)

### 3-1. 값 채우기 (`Agent/main.cpp`)

```cpp
MetricScheduler scheduler(ioContext, std::chrono::seconds(5),
    [&sender](const SystemMetrics& metrics)
    {
        apm::Metric pkt;
        pkt.set_cpu_usage_percent(metrics.cpuUsagePercent);
        pkt.set_mem_used_bytes(metrics.memUsedBytes);
        pkt.set_mem_total_bytes(metrics.memTotalBytes);
        pkt.set_disk_used_bytes(metrics.diskUsedBytes);
        pkt.set_disk_total_bytes(metrics.diskTotalBytes);
        pkt.set_net_rx_bytes_per_sec(metrics.netRxBytesPerSec);
        pkt.set_net_tx_bytes_per_sec(metrics.netTxBytesPerSec);

        TcpConnectionInfo tcpInfo = sender.GetConnectionInfo();   // getsockopt(TCP_INFO)
        pkt.set_tcp_rtt_us(tcpInfo.rttMicros);
        pkt.set_tcp_rtt_var_us(tcpInfo.rttVarMicros);
        pkt.set_tcp_retransmits(tcpInfo.retransmits);
        pkt.set_tcp_total_retrans(tcpInfo.totalRetrans);
        pkt.set_tcp_snd_cwnd(tcpInfo.sndCwnd);

        sender.Enqueue(pkt);   // ResilientSender 큐에 적재
    });
```
`SystemMetrics`(CPU/메모리/디스크/네트워크)는 `ResourceCollector::Collect()`가 채운다 —
CPU/네트워크는 두 시점 사이의 누적값 델타로 계산하므로 최초 1회 호출은 유효값이 안 나온다
(`_hasPrevSample`/`_hasPrevNetSample` 플래그로 처리).

### 3-2. 직렬화 → 암호화 → 프레이밍 → 전송 (`ApmSession::SendPacket`/`Send`)

```cpp
// ApmSession.h — 고수준 전송 진입점
template<typename PacketType>
void SendPacket(const PacketType& pkt, SendCallback onComplete = nullptr)
{
    String payload;
    if (!pkt.SerializeToString(&payload))
        throw std::runtime_error("ApmSession::SendPacket - serialization failed");

    uint16 id = static_cast<uint16>(PacketType::descriptor()->index());   // .proto 선언 순서
    Send(id, payload, std::move(onComplete));
}
```

```cpp
// ApmSession.cpp — 저수준 전송
void ApmSession::Send(uint16 id, const String& payload, SendCallback onComplete)
{
    auto self = shared_from_this();
    std::vector<BYTE> sealed = _payloadSealer->Seal(payload);   // AES-256-GCM 암호화+인증 태그

    if (sealed.size() + HEADER_SIZE > 0xFFFF)
        throw std::runtime_error("ApmSession::Send - sealed payload too large for uint16 size field");

    uint16 totalSize = static_cast<uint16>(HEADER_SIZE + sealed.size());
    auto buffer = std::make_shared<std::vector<BYTE>>(totalSize);

    PacketHeader header{ totalSize, id };
    std::memcpy(buffer->data(), &header, HEADER_SIZE);
    std::memcpy(buffer->data() + HEADER_SIZE, sealed.data(), sealed.size());

    asio::async_write(_sslStream, asio::buffer(*buffer),
        [this, self, buffer, onComplete](const asio::error_code& ec, size_t)
        {
            if (ec) { NotifyDisconnected(); if (onComplete) onComplete(false); return; }
            if (onComplete) onComplete(true);
        });
}
```

**단계별 정리**:
1. `pkt.SerializeToString(&payload)` — Protobuf 직렬화
2. `id = PacketType::descriptor()->index()` — `.proto` 안 선언 순서로 자동 결정되는 타입 ID
   (`Metric`=0, `TransactionSpan`=1). 손으로 배정하지 않음 — 새 메시지는 반드시 기존 메시지
   뒤에 이어 붙여야 ID가 안 어긋남.
3. `_payloadSealer->Seal(payload)`(`AesGcmPayload::Seal`) — 와이어 포맷
   `[Nonce(12B)][ciphertext(가변)][Tag(16B)]`. GCM이 암호화+인증을 한 번에 처리(별도 MAC
   조합 불필요).
4. `PacketHeader{ size, id }`(4바이트, `size`=헤더 포함 전체 크기)를 암호화된 페이로드
   앞에 붙임.
5. `asio::async_write`로 TLS 스트림에 그대로 전송(TLS가 이미 전송 계층 암호화를 한 번 더
   감쌈 — 2-3의 페이로드 암호화보다 바깥 계층).

이 과정은 Agent→Collector, Collector→Console(중계) 양쪽에서 동일 — `ResilientSender`가
`SendPacket`을 감싸 재연결/큐잉만 얹은 것이라 프레이밍/암호화 로직 자체는 공유된다.

### 3-3. 수신 측 재조립 (`ApmSession::ProcessAccumulated`)

TCP는 스트림이라 메시지 경계가 없다 — 수신 측은 `header.size`만큼 버퍼에 다 모일 때까지
누적하다가, 다 모이면 그 구간만 잘라 `Open()`으로 복호화/무결성 검증 후 `PacketHandler::
Dispatch(id, payload)`로 넘긴다. MAC 불일치(변조/키 불일치)나 잘못된 헤더는 그 세션만
끊고(`NotifyDisconnected()`) 프로세스 전체엔 영향 없음.

---

## 4. 패킷 구성과 전송 (Collector → Console 기준)

3장과 프레이밍/암호화 로직 자체는 같지만(`ApmSession::Send`, `PacketHeader{size,id}`,
AES-256-GCM), **"언제 조립하는가"와 "무엇을 조립하는가"가 다르다** — Agent는 매 5초 새
`apm::Metric`을 처음부터 채우지만, Collector는 **Agent에게서 이미 받아 파싱까지 끝난
패킷을 그대로 재사용**해서 재직렬화한다(값을 다시 채우는 과정이 없음). 키도 구간별로
분리(`agent_collector_aes.key` vs `webserver_aes.key`) — 한쪽이 유출돼도 다른 구간은 안전.

### 4-1. 조립 시점 — 주기 타이머 또는 CLI 트리거 (`Collector/main.cpp`)

```cpp
// pendingMetrics/SpanRecorder에 쌓인 걸 전부 WebServer로 보내고 비움 - 주기 타이머와
// CLI 트리거 둘 다 이 함수 하나를 호출함(로직 중복 방지).
auto flushToWebServer = [&pendingMetrics, &webServerSender, &consoleLogQueue]()
{
    if (!pendingMetrics.empty())
    {
        size_t count = pendingMetrics.size();
        consoleLogQueue.Push([count]() { std::cout << "[Collector] WebServer로 " << count << "건 전송 시도\n"; });
        for (const auto& m : pendingMetrics)
            webServerSender.Enqueue(m);   // 이미 갖고 있던 apm::Metric을 그대로 재사용

        pendingMetrics.clear();
    }

    auto spans = SpanRecorder::Instance().DrainAll();
    if (!spans.empty())
    {
        for (const auto& s : spans)
        {
            apm::TransactionSpan pkt;   // 이건 새로 채움 - Collector 내부 계측(트레이스) 결과라 원본이 없음
            pkt.set_operation_name(s.operationName);
            pkt.set_duration_us(s.durationUs);
            pkt.set_success(s.success);
            webServerSender.Enqueue(pkt);
        }
    }
};
```

`pendingMetrics`는 `PacketHandler::Register<apm::Metric>` 핸들러(2번 섹션 참고)에서 Agent
패킷을 받을 때마다 `push_back`되는 누적 버퍼 — SQLite 저장/콘솔 로그와 나란히 세 번째로
하는 일이다. `flushToWebServer()`를 부르는 트리거는 두 가지:

1. **주기 타이머**(`config.pushIntervalSeconds`, `collector_config.json` 설정값) —
   `pushTimer`가 콜백 끝에서 스스로 재예약(2번 섹션의 "자연 종료 불가 콜백" 패턴과 동일).
2. **콘솔 CLI 입력** — `cliThread`가 `std::cin`에서 `"send"`를 읽으면
   `asio::post(ioContext, flushToWebServer)`로 io_context 스레드에 넘김. `pendingMetrics`/
   `webServerSender`를 항상 이 하나의 스레드에서만 건드리게 만들어 락 없이 스레드 안전성을
   확보하는 설계(주석에 명시).

### 4-2. 직렬화 → 큐잉 → 전송 (`ResilientSender::Enqueue`/`EnqueueRaw`/`FlushNext`)

```cpp
// ResilientSender.h — 타입 안전 진입점
template<typename PacketType>
void Enqueue(const PacketType& pkt, SendCallback onComplete = nullptr)
{
    String payload;
    if (!pkt.SerializeToString(&payload)) { ... return; }

    uint16 id = static_cast<uint16>(PacketType::descriptor()->index());
    EnqueueRaw(id, std::move(payload), std::move(onComplete));
}
```

```cpp
// ResilientSender.cpp — 큐잉 + 즉시 전송 시도
void ResilientSender::EnqueueRaw(uint16 id, String payload, SendCallback onComplete)
{
    if (_queue.size() >= _maxQueueSize)   // 기본 100
    {
        if (_queue.front().onComplete)
            _queue.front().onComplete(false);   // 드롭도 콜백으로 통지
        _queue.pop_front();
    }
    _queue.push_back(QueuedPacket{ id, std::move(payload), std::move(onComplete) });

    if (_connected && !_sending)
        FlushNext();
}

void ResilientSender::FlushNext()
{
    if (_queue.empty() || !_connected || _sending || !_session) return;
    _sending = true;
    QueuedPacket packet = _queue.front();

    _session->Send(packet.id, packet.payload,   // SendPacket이 아니라 저수준 Send() 직접 호출
        [this, onComplete = packet.onComplete](bool success)
        {
            _sending = false;
            if (success)
            {
                if (onComplete) onComplete(true);
                if (!_queue.empty()) _queue.pop_front();
                FlushNext();   // 큐에 남은 다음 항목 이어서 전송
            }
            // 실패 시 pop 안 함 - OnSessionDisconnected가 재연결 예약, 재연결 후
            // FlushNext()가 이 항목부터 재시도(여기선 아직 "최종 실패" 통지 안 함)
        });
}
```

**3장(Agent→Collector)과의 차이점**:
- 3-2의 `SendPacket<T>()`는 `Send()`를 즉시 호출하지만, 여기 `Enqueue<T>()`는 **먼저 직렬화만
  해서 `_queue`에 넣어두고**, 연결돼 있고 유휴 상태(`_connected && !_sending`)일 때만
  `FlushNext()`가 실제로 `Send()`를 호출 — 연결이 끊겨 있어도 Collector 쪽 로직(`pendingMetrics`
  누적)은 계속 진행되고, 데이터는 메모리 큐에 쌓였다가 재연결되면 순서대로 나간다(단, 프로세스
  자체가 죽으면 큐 내용도 유실 — 디스크 영속화는 없음).
- 큐가 가득 차면(기본 100건) 가장 오래된 항목부터 버림 — "최신 데이터가 더 중요하다"는 가정.
- `id = PacketType::descriptor()->index()`(`.proto` 선언 순서, `Metric`=0/`TransactionSpan`=1)는
  3-2와 동일한 규칙 — Collector→Console도 같은 ID 체계를 그대로 씀.

### 4-3. 수신 측 — Console(.NET) `MetricsReceiverService`가 C++ 프레이밍을 직접 재구현

C# 쪽엔 `PacketHandler` 같은 공용 디스패처가 없다 — **와이어 포맷을 C#으로 손으로 다시
맞춰 구현**했다(`MetricsReceiverService.cs`, 파일 상단 주석에 "APM_Agent의
ApmSession.cpp/AesGcmPayload.cpp 참고"라고 명시돼 있음).

```csharp
// 헤더(4바이트, size+id, 리틀엔디안)만 읽고 id/암호화된 payload를 그대로 반환
private async Task<(ushort Id, byte[] SealedPayload)?> ReadOnePacketAsync(Stream stream, CancellationToken ct)
{
    var header = await ReadExactAsync(stream, 4, ct);
    if (header == null) return null;

    ushort totalSize = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(0, 2));
    ushort id = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(2, 2));

    var sealedPayload = await ReadExactAsync(stream, totalSize - 4, ct);
    return sealedPayload == null ? null : (id, sealedPayload);
}
```

```csharp
// HandleClientAsync 안 - id로 메시지 타입 분기(C++ PacketHandler::Dispatch에 대응하는 자리)
if (id == (ushort)Metric.Descriptor.Index)
    await StoreAndBroadcastAsync(DecryptAndParse(sealedPayload, _aesKey));
else if (id == (ushort)TransactionSpan.Descriptor.Index)
    await StoreSpanAsync(DecryptAndParseSpan(sealedPayload, _aesKey));
else
    Console.WriteLine($"[MetricsReceiverService] 알 수 없는 패킷 id={id}, 무시");
```

```csharp
// Unseal - AesGcmPayload::Seal()과 같은 와이어 포맷: [Nonce(12B)][ciphertext][Tag(16B)]
private static byte[] Unseal(byte[] sealedPayload, byte[] aesKey)
{
    var nonce = sealedPayload.AsSpan(0, NonceSize);
    var tag = sealedPayload.AsSpan(sealedPayload.Length - TagSize, TagSize);
    var ciphertext = sealedPayload.AsSpan(NonceSize, sealedPayload.Length - NonceSize - TagSize);

    var plaintext = new byte[ciphertext.Length];
    using var aesGcm = new AesGcm(aesKey, TagSize);
    aesGcm.Decrypt(nonce, ciphertext, tag, plaintext);
    return plaintext;
}
```

복호화 후 `Metric.Parser.ParseFrom(plaintext)`(Protobuf가 자동 생성한 파서, C++ 쪽
`descriptor()`와 짝을 이루는 `.proto` 기반 코드)로 역직렬화 → `MetricRecord`(EF Core 엔티티)로
매핑해 `ApmDbContext`에 저장 → `_hubContext.Clients.All.SendAsync("NewMetric", record)`로
SignalR 브로드캐스트(대시보드가 폴링 없이 실시간 갱신) → 이어서 `EvaluateAlertsAsync`로
임계치 평가. `TransactionSpan`은 저장만 하고 브로드캐스트는 안 함(대시보드 실시간 갱신은
범위 밖으로 명시돼 있음).

**정리하면 Collector→Console 흐름 한 줄 요약**: `PacketHandler`가 Agent 패킷을 받아
`pendingMetrics`에 쌓음 → 타이머/CLI가 `flushToWebServer()` 실행 → `ResilientSender::Enqueue`가
재직렬화해 자체 큐에 넣고 연결 상태면 바로 전송, 아니면 재연결까지 버퍼링 → Console이 같은
프레이밍/암호화 규칙을 C#으로 재구현한 코드로 읽어 EF Core 저장 + SignalR 푸시.

---

## 5. 프로세스 유지 방식이 왜 이 형태인가 — 대안과의 비교

전제: 2번 섹션에서 확인한 "콜백이 성공 시 자기 자신을 재호출 → `io_context::run()`의
미결(outstanding) 작업 수가 절대 0이 안 됨" 방식. 크로스플랫폼 이유(asio가 Linux
epoll/Windows IOCP/macOS kqueue를 API 하나로 추상화 — GW2_ServerCore 이식 목적상
예측 가능한 선택)는 이미 예상 가능한 부분이라 상세 설명 생략.

### 5-1. `while(true)` 수동 폴링 방식과 비교해서 얻는 이득

수동으로 짠다면 대략 이런 모양이 된다:

```cpp
while (running)
{
    ioContext.poll_one();   // 또는 poll() - 논블로킹, 즉시 반환
    // 어떤 소켓이 accept 대기 중인지, 어떤 세션이 recv 대기 중인지,
    // 어떤 타이머가 얼마나 남았는지를 직접 자료구조로 추적/확인...
}
```

- **바쁜 대기(busy-wait) 문제**: `poll_one()`/`poll()`은 즉시 반환하는 논블로킹 함수라, 할 일이
  없으면 루프가 빈 채로 계속 돌아 CPU 코어 하나를 100% 점유한다. 반대로 `run()`은 내부적으로
  `epoll_wait`(Linux)/`GetQueuedCompletionStatus`(Windows) 같은 커널의 블로킹 대기 syscall을
  직접 호출해, 할 일이 없을 때 스레드가 실제로 잠들어(sleep) CPU를 거의 안 쓴다. 타협안으로
  루프에 `sleep(N)`을 넣으면 이번엔 이벤트 발생 후 최대 N만큼의 응답 지연이 생김 — `run()`은
  이 트레이드오프 자체가 없다(대기도 즉시 처리도 둘 다 얻음).
- **연결·타이머가 늘수록 수동 관리 코드가 커짐**: while 루프 방식이면 사실상 reactor를 손으로
  재구현해야 한다(어느 소켓이 준비됐는지, 어떤 세션이 어느 상태인지 루프 바깥에 별도 테이블로
  추적). 콜백 재등록 방식은 "이 작업이 끝나면 다음에 뭘 할지"를 콜백 지역 범위 안에 적어두는
  것으로 끝 — `doAccept()`는 세션을 만든 뒤 자기 자신을 다시 등록하면 그걸로 완결, 루프
  바깥에 상태를 별도로 안 만들어도 된다.
- **멀티스레드 확장이 자연스러움**: 여러 스레드가 같은 `io_context`에 대해 `run()`을 호출하는
  것만으로 워커 스레드 풀처럼 동작(asio가 내부적으로 핸들러를 안전하게 분배) — 수동 while
  루프로 이런 병렬 처리를 하려면 락/큐를 직접 설계해야 한다(참고: Collector가 이미 SQLite
  저장/콘솔 로그를 위해 직접 만든 `WorkerQueue`가 바로 이 "직접 설계"의 예 — `io_context`가
  담당하는 네트워크 이벤트 쪽은 이 수고를 안 해도 된다는 대비가 됨).
- **종료 조건이 선언적**: `run()`의 반환 조건("미결 작업 수 == 0")은 각 비동기 작업이 자기
  생명주기를 스스로 신고하는 방식이라, "이제 그만 돌아도 되는가"라는 판단이 코드 전체에
  흩어지지 않고 asio 내부 카운터 하나로 수렴된다. while 루프였다면 그 판단(예: `bool running`
  플래그)을 모든 진입점에서 참조/갱신해야 하고, 한 곳이라도 놓치면 루프가 못 끝나거나 너무
  일찍 끝나는 버그가 생기기 쉽다.

### 5-2. 셸 기반 데몬 프로세스 방식과 비교

셸 데몬화(`nohup ./Collector &`, 이중 fork+`setsid`로 세션 분리, systemd 유닛의
`Restart=always` 등)와는 애초에 **풀려는 문제 자체가 다르다** — 같은 층위에서 비교하면
오해하기 쉽다.

- **데몬화가 실제로 해결하는 것**: 터미널 종료(SIGHUP)에도 프로세스가 살아남게 하는 것,
  제어 터미널/세션에서 분리하는 것, stdout/stderr를 로그 파일로 리다이렉트하는 것.
  **이 중 어느 것도 "여러 클라이언트 연결을 동시에 어떻게 처리할 것인가"에는 답하지 않는다.**
  `nohup`으로 감싸도 원래 코드가 accept 한 번에 블로킹되는 구조였다면 여전히 한 번에 한
  연결만 처리한다 — 데몬화는 동시성 문제를 전혀 풀어주지 않는다. 콜백 재등록 방식이 주는
  이득(다중 연결 처리, CPU 효율)은 셸 데몬화로는 애초에 얻을 수 없는 종류의 이득이다.
- **프로세스 내부 상태 보존**: `pendingMetrics`/`WorkerQueue`에 쌓인 미처리 작업, 살아있는
  `ApmSession`들은 전부 "이 프로세스 인스턴스가 계속 같은 채로 유지된다"는 전제에 기대고
  있다. systemd `Restart=always`처럼 "죽으면 다시 띄운다"는 방식은 크래시 복원력을 주지만,
  그 대가로 재시작 시점에 메모리에 있던 이 상태들이 통째로 사라진다 — 2번 섹션에서 확인한
  "kill 시 `WorkerQueue`가 flush 없이 유실"과 근본적으로 같은 성격의 손실이 재시작마다
  반복되는 셈. 지금 코드는 "재시작 없이 하나의 프로세스가 계속 버틴다"는 걸 전제로 짜여
  있어서, 셸/systemd 레벨의 자동 재시작을 얹어도 그게 내부 이벤트 루프 설계를 대신해주지
  않는다 — 오히려 재시작 시 데이터 유실 범위만 커진다.
- **신호 처리 공백은 그대로 남음**: 2번 섹션에서 이미 확인했듯 `SIGINT`/`SIGTERM` 핸들러가
  코드 어디에도 없다. systemd가 종료 시 보내는 `SIGTERM`을 셸 데몬화만으로 우아하게 처리하게
  만들어주지 않는다 — 그건 여전히 프로세스 내부에 핸들러를 추가해야 하는 별개의 작업이다.
  즉 셸 데몬화는 "실행 방식(어떻게 백그라운드에 띄우는가)"의 문제고, 콜백 재등록/그레이스풀
  종료는 "코드 내부 설계(무엇이 프로세스를 계속 일하게 하는가)"의 문제 — 계층이 다르다.

---

## 6. `Common/` — 실제로 정적 라이브러리로 공유됨 (`APM_Common`)

폴더로 소스를 묶어놓은 것뿐이 아니라, 실제로 정적 라이브러리 타겟으로 빌드되어 여러 실행
파일이 링크한다.

```cmake
# APM_Agent/Common/CMakeLists.txt
add_library(APM_Common STATIC
    ApmSession.cpp AriaCipher.cpp AesGcmCipher.cpp AesGcmPayload.cpp
    HmacUtil.cpp SecurePayload.cpp KeyLoader.cpp PrivilegeDrop.cpp
    ResourceCollector.cpp MetricScheduler.cpp ResilientSender.cpp
    PacketHandler.cpp SpanRecorder.cpp ScopedSpan.cpp WorkerQueue.cpp
    ../Protocol/Metric.pb.cc
)
target_link_libraries(APM_Common PUBLIC GW2_CrossPlatformCore protobuf::libprotobuf)
```

`APM_Agent/CMakeLists.txt`에서 4곳이 이 라이브러리를 링크:

```cmake
target_link_libraries(Collector PRIVATE APM_Common APM_Storage)
target_link_libraries(Agent PRIVATE APM_Common)
target_link_libraries(LoadTester PRIVATE APM_Common)
```
```cmake
# tests/CMakeLists.txt
target_link_libraries(APM_Common_Tests PRIVATE APM_Common GTest::gtest GTest::gtest_main)
```

**의미**: `ApmSession`(세션/프레이밍), `AesGcmPayload`(암호화), `PacketHandler`(디스패치),
`ResilientSender`(재연결 큐잉), `Metric.pb.cc`(Protobuf 생성 코드) 같은 프로토콜·네트워킹
핵심 코드가 **한 번 컴파일되어 Collector/Agent/LoadTester/테스트 4개 바이너리에 동일하게
링크**된다. LoadTester가 "Agent인 척" 프로토콜을 그대로 재현할 수 있는 것도 별도로
재구현한 게 아니라 같은 `APM_Common`을 링크하기 때문.

---

## 7. `Agent/main.cpp` — `sender`/`scheduler` 초기화에 람다를 쓰는 이유

포인터로 미리 만든 객체를 넘기지 않고 람다(`std::function`)를 넘기는 두 지점이 있는데,
성격이 다르다 — 하나는 **팩토리**, 하나는 **콜백**.

```cpp
ResilientSender sender(ioContext, sslContext, COLLECTOR_HOST, COLLECTOR_PORT,
    [agentCollectorKey]()
    {
        return std::make_unique<AesGcmPayload>(agentCollectorKey);
    });

MetricScheduler scheduler(ioContext, std::chrono::seconds(5),
    [&sender](const SystemMetrics& metrics)
    {
        apm::Metric pkt;
        pkt.set_cpu_usage_percent(metrics.cpuUsagePercent);
        // ... (7개 필드 set_*, 3-1절 참고)
        sender.Enqueue(pkt);
    });
```

**`sender`의 `SealerFactory`** (`ResilientSender.h`: `using SealerFactory = std::function<std::unique_ptr<IPayloadSealer>()>;`):
- 재연결마다 `ApmSession`을 통째로 새로 만드는데, 그때마다 새 세션에 물릴 `IPayloadSealer`도
  새 인스턴스여야 함(재연결마다 깨끗한 상태로 시작한다는 설계). 포인터 하나를 넘기면 "하나의
  인스턴스를 계속 재사용"하는 그림이 돼서 이 의도와 어긋남 — 팩토리 람다는 "호출할 때마다
  새 인스턴스"라는 의미 자체를 타입으로 표현.
- `ResilientSender`가 구체 타입(`SecurePayload` vs `AesGcmPayload`)을 몰라도 되게 함 —
  포인터 인자였다면 `ResilientSender`가 어떤 서브클래스를 만들지 알아야 하거나 별도 추상
  팩토리 클래스 계층이 필요했을 것.

**`scheduler`의 `MetricCallback`** (`MetricScheduler.h`는 `ResourceCollector.h`만 include,
`ResilientSender.h`는 모름):
- `ResilientSender*`를 직접 받았다면 `MetricScheduler`가 네트워크 계층 헤더를 알아야 하고
  "수집 주기 관리"에 "어떻게 전송하는가"라는 무관한 책임이 섞임. 지금 구조는
  `MetricScheduler`가 "5초마다 `ResourceCollector::Collect()`를 불러 콜백에 넘긴다"까지만
  책임지고, 패킷 조립(TCP 정보 포함)과 전송은 호출부(`main.cpp`)의 람다 본문에 남김 — 관심사 분리.
- `[&sender]` 참조 캡처가 안전한 이유: `sender`/`scheduler` 둘 다 `main()`의 지역 변수이고
  `sender`가 먼저 선언돼 나중에 소멸 — `ioContext.run()`이 도는 동안 둘 다 스택에 살아있어
  댕글링 참조 위험 없음.

두 경우 다 "인터페이스 추상클래스 + 구체 서브클래스 포인터" 대신 `std::function`으로 동작
자체를 주입하는 경량 Strategy/DI — 이 규모에서 클래스 계층 없이 지역 상태를 캡처한 즉석
클로저로 끝낼 수 있다는 이점.

---

## 8. `WorkerQueue`(APM_Agent 자체 구현) vs `JobQueue`(GW2_CrossPlatformCore)

`Common/WorkerQueue.h`/`.cpp`(35+27줄, 표준 라이브러리만 사용)가 실제 위치. `Collector/main.cpp`가
`metricStoreQueue`/`consoleLogQueue` 두 인스턴스로 사용 중인 유일한 소비처(`APM_Common`을 링크하는
다른 타겟도 쓸 수 있는 상태지만 아직 안 씀).

```cpp
// WorkerQueue.cpp — 전용 스레드 1개를 직접 소유, 소멸자가 join까지 책임
WorkerQueue::WorkerQueue() : _worker([this]() { WorkerLoop(); }) {}
WorkerQueue::~WorkerQueue()
{
    { std::lock_guard<std::mutex> guard(_mutex); _stop = true; }
    _cv.notify_one();
    _worker.join();
}
void WorkerQueue::Push(std::function<void()> job)
{
    { std::lock_guard<std::mutex> guard(_mutex); _jobs.push(std::move(job)); }
    _cv.notify_one();
}
```

**`JobQueue`와의 구조적 차이**(`GW2_CrossPlatformCore/Thread/JobQueue.h/.cpp`):

| 축 | `WorkerQueue` | `JobQueue` |
|---|---|---|
| 스레드 소유 | 인스턴스당 전용 스레드 1개 | 자기 스레드 없음 — `GThreadManager`의 공유 풀이 `GlobalQueue`에서 꺼내 실행(매번 다른 스레드일 수 있음) |
| `Push()` 동작 | 항상 큐에 넣고 깨움, 호출자가 직접 실행하는 경로 없음 | `pushOnly=false`(기본값)이고 호출 스레드가 다른 `JobQueue::Execute()` 안이 아니면 **그 자리에서 동기 실행**하는 함정 존재 |
| 락 구현 | 표준 `std::mutex`/`condition_variable` | 자체 `Lock`(스핀락, 소유 스레드 ID 비트 기록) — §9의 버그가 여기 있었음 |
| 기동 인프라 | 생성자가 즉시 스레드 기동, 의존성 없음 | `GThreadManager->Launch(...)` 필요, 워커 루프가 스레드 로컬 `LEndTickCount`(틱 예산) 세팅을 전제 |
| 종료 | 소멸자가 `_stop` + `join()`으로 결정적 종료 | 공유 풀 구조라 "이 큐 하나만 멈추고 스레드 회수" 개념 없음 |

**선택 배경**(2026-07-27 결정, 상세: `WORK_STATUS.md` 1-7-b): 1-7에서 `JobQueue`를 먼저
채택했다가 부하 재실측 중 `LOCK_TIMEOUT` 크래시로 재현된 `Lock::WriteUnlock()` 버그(§9) 발견.
`GW2_CrossPlatformCore`는 여러 프로젝트가 공유하는 코어라 그쪽을 고치는 대신, `APM_Agent`
범위 안에서 표준 라이브러리로 최소 구현(`WorkerQueue`)을 새로 만드는 쪽으로 확정.

---

## 9. `GW2_CrossPlatformCore/Thread/Lock.cpp` — 이관 중 유실된 `ReadLock()`

§8의 `Lock::WriteUnlock()` 버그를 실제 원본(`GW2_ServerCore`, 다른 저장소 `../gw2/GW2_Server/`)과
`diff`로 대조해 근본 원인을 확정, 2026-08-05 수정 완료. **원인은 OS API 차이가 아니라
`GW2_ServerCore` → `GW2_CrossPlatformCore` 포팅 중 `WriteUnlock()`과 `ReadLock()`의 함수
본문이 뒤바뀐 복사·붙여넣기 실수** — `LThreadId`(원본의 GW2 프레임워크 전역 스레드 ID)를
이식 가능한 `GetThisThreadId()`로 바꾸는 리팩터링 도중 발생한 것으로 추정.

- **수정 전**: `WriteUnlock()`이라는 이름 아래 원래 `ReadLock()`의 본문(`_lockFlag.fetch_add(1)`로
  하위 비트만 건드림)이 들어있었고, 원래 `WriteUnlock()`의 해제 로직(`--_writeCount` + 0이면
  `_lockFlag.store(EMPTY_FLAG)`)은 완전히 유실 — 소유 스레드 비트(`WRITE_THREAD_MASK`)가
  영원히 안 지워짐. `ReadLock()` 자체는 헤더에 선언만 남고 `.cpp`엔 정의가 없어(아무도 안
  불러 링크 에러 없이 숨어있었음) 사실상 삭제된 상태였음.
- **수정 후**: `WriteUnlock()`을 원본 로직대로 복원, `ReadLock()`을 원본대로 되살림. 전체
  diff·전/후 코드 전문은 `Docs/SESSION_LOG.md` 2026-08-05 항목 참고.
- **검증**: `cmake --build build --target GW2_CrossPlatformCore Collector Agent` 빌드 성공,
  `ctest` 9/9 통과.
- **현재 런타임엔 영향 없음** — `Collector/main.cpp`는 1-7-b 이후 `JobQueue`/`Lock`을 안 쓰고
  `WorkerQueue`로 이미 대체된 상태(§8). 향후 이 저장소에서 `Thread/JobQueue`를 다시 쓸 경우를
  위한 정합성 수정.
- **수정 범위**: APM 저장소의 `GW2_CrossPlatformCore/Thread/Lock.cpp` 사본만 — `../gw2`
  원본(별도 저장소)은 미수정.

---

## 10. 순서도 — 수집 항목별 분석 착수 전 전체 흐름 정리

> 아래 두 순서도는 직접 분석(수집 항목별 기능 분석 → LoadTester 분석 순서)에 들어가기 전
> 전체 그림을 잡기 위한 것. 각 단계 옆 괄호 안은 실제 함수/파일 위치.

### 10-1. Agent — 항목별 수집 → Collector 발송

```
MetricScheduler 타이머(5초 주기, Common/MetricScheduler.cpp)
        │
        ▼
ResourceCollector::Collect()  (Common/ResourceCollector.cpp)
        │
        ├─ ComputeCpuUsage()          Linux: /proc/stat 두 시점 델타
        │                             Windows: GetSystemTimes()
        ├─ GetMemTotal()/GetMemUsed() Linux: /proc/meminfo(MemTotal/MemAvailable)
        │                             Windows: GlobalMemoryStatusEx()
        ├─ GetDiskUsage()             Linux: statvfs("/")
        │                             Windows: GetDiskFreeSpaceExA("C:\\")
        └─ ComputeNetworkUsage()      Linux: /proc/net/dev 두 시점 델타(lo 제외)
                                      Windows: GetIfTable2()(루프백/비활성 제외)
        │
        ▼
SystemMetrics{cpu, mem*2, disk*2, net*2} 리턴  (최초 1회는 CPU/Net 델타 기준값 없어 0)
        │
        ▼
Agent/main.cpp — apm::Metric pkt 채움 (7개 필드 set_*)
        │
        ▼
sender.GetConnectionInfo()  →  ApmSession::GetConnectionInfo()  (Common/ApmSession.cpp)
        │  getsockopt(TCP_INFO) — ResourceCollector와 무관한 별도 경로(소켓 자체에서 직접 조회)
        ▼
pkt에 tcp_rtt_us/tcp_rtt_var_us/tcp_retransmits/tcp_total_retrans/tcp_snd_cwnd 5개 필드 추가 채움
        │
        ▼
sender.Enqueue(pkt)  (Common/ResilientSender.cpp)
        │
        ├─ SerializeToString (Protobuf 직렬화)
        ├─ id = PacketType::descriptor()->index()  (.proto 선언 순서, Metric=0)
        ├─ _queue.push_back (최대 100건, 초과 시 가장 오래된 항목 드롭)
        └─ (_connected && !_sending) → FlushNext() → ApmSession::Send()
                ├─ AesGcmPayload::Seal()  — AES-256-GCM, agent_collector_aes.key
                ├─ PacketHeader{size,id} 부착
                └─ asio::async_write (TLS 스트림)
        │
        ▼
Collector로 전송 완료
```

### 10-2. Collector — 수신 → Console 발송

```
ApmSession::RegisterRecv()  async_read_some 콜백  (Common/ApmSession.cpp)
        │
        ▼
누적 버퍼 → ProcessAccumulated()  — header.size만큼 모이면 잘라냄
        │
        ├─ Open() — 복호화 + MAC 무결성 검증 (agent_collector_aes.key)
        ▼
PacketHandler::Dispatch(id, payload)  →  Register<apm::Metric> 핸들러  (Collector/main.cpp)
        │  이 핸들러 하나가 아래 3가지를 "동시에"(순서대로 큐잉) 수행:
        │
        ├─① metricStoreQueue.Push(...) ─(워커 스레드1)→ SqliteMetricStore::Store()
        ├─② consoleLogQueue.Push(...)  ─(워커 스레드2)→ std::cout "metric received" 로그
        └─③ pendingMetrics.push_back(pkt)   (io_context 스레드, 다음 flush까지 누적만)
        │
        ▼
발송 트리거 (둘 중 하나)
        ├─ pushTimer 주기 재예약 (collector_config.json의 pushIntervalSeconds)
        └─ cliThread가 stdin "send" 수신 → asio::post(ioContext, flushToWebServer)
        │
        ▼
flushToWebServer()  (Collector/main.cpp)
        ├─ pendingMetrics 순회 → webServerSender.Enqueue(m)  (파싱 끝난 pkt 재사용, 재직렬화만)
        ├─ SpanRecorder::DrainAll() → apm::TransactionSpan 신규 채움 → webServerSender.Enqueue(pkt)
        └─ pendingMetrics.clear()
        │
        ▼
ResilientSender::Enqueue → EnqueueRaw → (연결+유휴 시) FlushNext() → ApmSession::Send()
        ├─ AesGcmPayload::Seal()  — AES-256-GCM, webserver_aes.key (Agent↔Collector 키와 별개)
        ├─ PacketHeader{size,id} 부착
        └─ asio::async_write (TLS 스트림)
        │
        ▼
Console(.NET) MetricsReceiverService — 헤더 4B 파싱 → Unseal → Parser.ParseFrom → EF Core 저장
        + SignalR "NewMetric" 브로드캐스트 + EvaluateAlertsAsync  (4-3절 상세)
```

### 10-3. 다음 분석 순서 메모

1. 위 6-1의 각 수집 항목(CPU/메모리/디스크/네트워크/TCP)을 항목별로 나눠 기능 분석 —
   Linux `/proc` 파싱 방식과 Windows WinAPI 방식을 짝지어 비교하면 "왜 이 값을 이렇게
   계산하는가"(예: CPU/네트워크는 왜 델타 계산이 필요한가)가 드러남.
2. 그다음 `APM_Agent/LoadTester/`로 넘어가 "Agent인 척하는 코드가 실제 프로토콜/큐잉
   로직을 어떻게 재사용하는지" 확인 — 1번 문서 추천 순서의 4번 항목과 동일한 이유
   (프로토콜을 소비자 관점에서 재검증).

## 11. Console — 패킷 수신 후 처리 상세 (`MetricsReceiverService.cs`)

4-3절은 프레이밍/복호화(와이어 포맷을 C#으로 재구현한 부분)까지만 다뤘다. 여기서는 그
바깥쪽 — TCP 리스너가 어떻게 뜨고, 연결 하나를 어떻게 계속 처리하고, 저장 뒤에 무슨 일이
일어나는지 — 를 정리한다.

### 11-1. 수신 계층 — `TcpListener` + TLS(`SslStream`)

`MetricsReceiverService : BackgroundService`. `ExecuteAsync`가 진입점이고, ASP.NET Core의
제네릭 호스트가 앱 부팅 시 자동으로 백그라운드 Task로 실행한다(수동 스레드 생성 없음 —
`ApmModule.RegisterServices`의 `services.AddHostedService<MetricsReceiverService>()` 한 줄로
등록 끝. Collector가 §2에서처럼 스레드를 직접 만들어 살려두는 것과 대비되는 부분 — .NET
쪽엔 "호스트가 백그라운드 서비스 생명주기를 관리해주는" 표준 인프라가 있어서 수동 관리가
필요 없다).

```csharp
var listener = new TcpListener(IPAddress.Any, _port);
listener.Start();
while (!stoppingToken.IsCancellationRequested)
{
    var client = await listener.AcceptTcpClientAsync(stoppingToken);
    _ = HandleClientAsync(client, stoppingToken);   // await 안 함 — fire-and-forget
}
```

`_ = HandleClientAsync(...)`로 **await하지 않고** 던진다 — accept 루프가 한 연결 처리가
끝날 때까지 기다리지 않고 바로 다음 `AcceptTcpClientAsync`로 돌아가야 여러 Collector(또는
재연결)를 동시에 받을 수 있기 때문. Collector 쪽은 세션 하나(Console 하나에만 붙는
`ResilientSender`)라 실제로 동시 연결이 여러 개 생길 일은 거의 없지만, 구조상 다중 연결을
막지 않는다.

연결을 받으면 바로 `SslStream`으로 감싸 `AuthenticateAsServerAsync(_serverCert,
clientCertificateRequired: false, checkCertificateRevocation: false)`를 수행한다 — Agent↔
Collector 구간(자체 AEAD 프레이밍만 있고 TLS는 없음)과 달리, Collector↔Console 구간은
**TLS 위에 AES-GCM 페이로드 암호화가 다시 얹힌 이중 암호화** 구조다. `clientCertificateRequired:
false`이므로 서버(Console)만 인증서로 자신을 증명하고 Collector는 TLS 레벨에서 인증되지
않는다 — Collector의 신원 보장은 TLS가 아니라 페이로드를 열 수 있는 AES 키를 갖고 있다는
사실 자체에 의존한다.

### 11-2. 연결당 처리 루프 — "정확한 크기만큼 읽기" 방식 (C++ 재조립 방식과의 차이)

```csharp
while (!stoppingToken.IsCancellationRequested)
{
    var packet = await ReadOnePacketAsync(sslStream, stoppingToken);
    if (packet == null) break;   // 연결 종료
    ...
}
```

`ReadExactAsync`가 핵심이다.

```csharp
internal static async Task<byte[]?> ReadExactAsync(Stream stream, int length, CancellationToken ct)
{
    var buffer = new byte[length];
    var offset = 0;
    while (offset < length)
    {
        var read = await stream.ReadAsync(buffer.AsMemory(offset, length - offset), ct);
        if (read == 0) return null;   // 연결 종료
        offset += read;
    }
    return buffer;
}
```

TCP는 스트림이라 한 번의 `ReadAsync`가 요청한 길이를 다 채운다는 보장이 없다 — 그래서
"모자란 만큼 다시 읽기"를 offset 루프로 직접 구현했다(짧은 read 방어, C++의 `recv`가 요청
바이트 수보다 적게 반환할 수 있는 것과 같은 이유).

이 지점이 C++ `ApmSession`과 **구조적으로 다른 부분**이다.

- **C++ 쪽(`ProcessAccumulated`)**: `async_read_some`이 들어오는 대로 아무 크기나 누적
  버퍼(`_accumulated`)에 쌓고, 그 버퍼에서 "완전한 패킷이 나올 때까지" while 루프로 반복
  파싱 — 한 번의 recv에 패킷이 여러 개 섞여 들어오거나 헤더/바디가 쪼개져 들어오는 모든
  경우를 버퍼 재조립 로직으로 흡수한다.
- **C# 쪽(`ReadExactAsync`)**: "헤더는 정확히 4바이트, 바디는 정확히 `totalSize-4`바이트"라는
  걸 미리 알고 그만큼만 목표로 반복 `ReadAsync`를 건다 — 별도의 누적 버퍼가 필요 없다.
  대신 매 패킷마다 최소 2번(헤더용, 바디용)의 "필요한 만큼 채워질 때까지 기다리는" 구간이
  생긴다.

즉 같은 "TCP 스트림 재조립" 문제를 C++은 **범용 누적 버퍼 + 파싱 루프**로, C#은 **매번
정확한 길이를 요청하는 반복 읽기**로 푼다 — 후자가 코드는 더 짧지만, 한 패킷을 다 읽기
전까지는 다음 패킷의 바이트가 이미 도착해 있어도 그냥 커널 버퍼에 남아있다가 다음
`ReadOnePacketAsync` 호출 때 소비된다(반응형이 아니라 필요한 만큼 당겨오는 방식).

**왜 서로 다른 전략을 택했는가 — 진짜 이유는 스레드 모델 차이다.**

C++ `RegisterRecv()`를 다시 보면:

```cpp
void ApmSession::RegisterRecv()
{
    auto self = shared_from_this();
    _sslStream.async_read_some(asio::buffer(_recvBuffer),   // _recvBuffer: std::array<BYTE, 4096>
        [this, self](const asio::error_code& ec, size_t bytes)
        {
            ...
            _accumulated.insert(_accumulated.end(), _recvBuffer.begin(), _recvBuffer.begin() + bytes);
            ProcessAccumulated();
            RegisterRecv();
        });
}
```

`_recvBuffer`는 고정 4096바이트고, `async_read_some`은 "그 순간 커널에 도착해 있는 만큼"을
한 번에 받아온다. `apm::Metric`/`apm::TransactionSpan`은 암호화 후에도 수십~백여 바이트
수준이라, **한 번의 read 완료에 이미 완성된 패킷이 여러 개 들어 있는 경우가 흔하다** —
`ProcessAccumulated()`의 `while(true)` 루프는 바로 이걸 한 번에 다 뽑아내기 위해 존재한다.

이게 왜 중요하냐면, Collector의 io_context는 **스레드 하나가 여러 세션의 I/O를 전부
처리**하는 리액터 모델(§10-3)이기 때문이다. 만약 "정확히 헤더 4바이트, 정확히 바디
N바이트"를 기다리는 방식(`async_read` 같은 exact-read 조합 연산)을 썼다면, 패킷 하나당
최소 2번의 완료 콜백(→ ready queue 재진입)이 필요해진다 — 그 세션의 다음 패킷을 처리하기
전에 매번 "이 스레드가 다른 세션들 콜백도 처리하고 다시 내 차례가 올 때까지" 기다려야
할 수 있다는 뜻이다. `async_read_some` + 누적 버퍼 방식은 한 번의 완료로 이미 도착한
패킷을 몰아서 다 처리하므로, 같은 스레드를 공유하는 다른 세션들의 대기 시간을 줄인다.

C# `MetricsReceiverService`는 이 제약이 없다. `HandleClientAsync`가 연결마다 **독립된
Task**로 떠 있고(11-1절), `await stream.ReadAsync(...)`가 멈춰도 그건 그 Task 하나만
멈추는 것이지 다른 연결을 처리할 스레드를 붙잡아두는 게 아니다(.NET 스레드 풀이 알아서
다른 작업에 그 스레드를 돌려씀). 게다가 Console에 동시 연결되는 Collector 수도
사실상 1개 수준이라, "한 번의 I/O 이벤트로 여러 패킷을 몰아 처리해서 다른 세션의 대기를
줄인다"는 이득 자체가 발생할 상황이 없다 — 그래서 굳이 범용 누적 버퍼를 만들 이유 없이,
헤더가 알려주는 정확한 크기만큼만 그때그때 받는 더 단순한 구현을 택할 수 있었던 것이다.

즉 두 구현의 차이는 "C#이 더 게을러서"가 아니라, **한 스레드가 여러 연결을 다중화하는
리액터 구조(C++)냐, 연결마다 독립된 비동기 Task가 도는 구조(C#)냐**라는 동시성 모델
차이가 그대로 프레이밍 전략 차이로 이어진 것이다.

### 11-3. id 분기 → 복호화/파싱 → 저장 → SignalR 브로드캐스트

id 분기와 `Unseal()`은 4-3절과 동일하므로 반복하지 않는다. 저장 이후 경로만 보면:

```csharp
private async Task StoreAndBroadcastAsync(Metric metric)
{
    using var scope = _scopeFactory.CreateScope();
    var db = scope.ServiceProvider.GetRequiredService<ApmDbContext>();

    var record = new MetricRecord { /* Metric 필드 → EF Core 엔티티 필드 매핑 */ };
    db.Metrics.Add(record);
    await db.SaveChangesAsync();

    await _hubContext.Clients.All.SendAsync("NewMetric", record);
    await EvaluateAlertsAsync(db, record);
}
```

`_scopeFactory.CreateScope()`를 저장할 때마다 새로 만드는 이유: `MetricsReceiverService`
자체는 앱 전체에 하나뿐인 **싱글톤**(`BackgroundService`) 인데, `ApmDbContext`는 EF Core
관례상 **스코프드(Scoped)** 서비스라 싱글톤 안에서 그대로 주입받을 수 없다(요청/스코프
경계가 없으면 DbContext 하나를 여러 비동기 작업이 동시에 공유하게 되어 스레드 안전성이
깨짐) — 그래서 저장 시점마다 짧은 수명의 스코프를 직접 만들어 그 안에서만 DbContext를
꺼내 쓰고 버린다. ASP.NET Core에서 싱글톤 백그라운드 서비스가 스코프드 서비스를 쓸 때의
표준 패턴이다.

`_hubContext.Clients.All.SendAsync("NewMetric", record)` — SignalR로 **연결된 모든 브라우저
클라이언트**에 즉시 push(폴링 없음). `MetricsHub`는 빈 클래스인데, 서버→클라이언트 단방향
push 전용이라 클라이언트가 호출할 서버 메서드가 없어서다. 허브는 `/apm/hub/metrics`
경로로 노출된다(`ApmModule.MapEndpoints`의 `endpoints.MapHub<MetricsHub>(...)`) — 브라우저
JS가 이 경로로 SignalR 연결을 맺어 push를 받는다.

`TransactionSpan`(`StoreSpanAsync`)은 저장만 하고 브로드캐스트하지 않는다 — 대시보드
실시간 갱신은 설계 시점에 범위 밖으로 명시돼 있다(코드 주석: "5순위에서 집계 뷰를 만들 때
같이 고려").

**이 구간이 "웹소켓"인가 — 정확히는 구간별로 다르다.**

- **Agent↔Collector, Collector↔Console**: 전부 순수 TCP(+TLS) 위에 `PacketHeader` 직접
  프레이밍 — HTTP 업그레이드 핸드셰이크도, RFC 6455 WebSocket 프레임 포맷도 없다.
  "영속 연결을 맺어두고 서버가 원할 때 밀어넣는다"는 **개념**은 WebSocket과 같지만,
  프로토콜 자체는 커스텀이다.
- **Console↔브라우저(SignalR)**: 이쪽은 **실제로 WebSocket을 쓴다.** `ApmModule.cs`엔
  전송 방식을 강제하는 설정이 없어 SignalR 기본 협상 로직대로 동작 — 가능하면 WebSocket,
  안 되면 SSE/롱폴링으로 폴백. 즉 이 파이프라인 전체에서 "진짜 WebSocket 프로토콜"이
  쓰이는 지점은 Console↔브라우저 구간뿐이다(12절에서 상세).

### 11-4. 알림 평가 — 레벨 트리거가 아닌 엣지 트리거 상태 기계

```csharp
public static AlertTransition Evaluate(double currentValue, double threshold, bool currentlyOpen)
{
    bool isBreaching = currentValue >= threshold;

    if (isBreaching && !currentlyOpen) return AlertTransition.Opened;
    if (!isBreaching && currentlyOpen) return AlertTransition.Resolved;
    return AlertTransition.None;
}
```

`EvaluateAlertsAsync`는 저장이 끝난 메트릭 1건마다 활성화된 `AlertThreshold`를 전부 순회
하면서, 해당 지표에 대해 현재 열려 있는(`ClosedAt == null`) `AlertRecord`가 있는지를 먼저
조회한 뒤 위 순수 함수(`AlertEvaluator.Evaluate`, DB I/O 없음, 별도 테스트 대상)로 상태
전이를 판정한다.

핵심은 **"임계치를 넘을 때마다"가 아니라 "상태가 바뀌는 순간에만"** 알린다는 것 — 매
샘플마다 반복 알림(alert flapping)을 내지 않는, Zabbix/Nagios/Alertmanager와 동일한
엣지 트리거 패턴이다. `Opened`면 새 `AlertRecord`를 만들고 `"AlertOpened"`를 브로드캐스트,
`Resolved`면 기존 레코드에 `ResolvedValue`/`ClosedAt`을 채우고 `"AlertResolved"`를
브로드캐스트한다.

### 11-5. 요약 — 수신 파이프라인 한 줄 정리

```
TcpListener.AcceptTcpClientAsync (연결마다 fire-and-forget Task)
  → SslStream.AuthenticateAsServerAsync (TLS, 서버 인증서만)
  → while: ReadOnePacketAsync (ReadExactAsync 반복으로 헤더4B/바디N B 정확히 읽기)
  → id 분기 → Unseal(AES-256-GCM) → Protobuf ParseFrom
  → (스코프 생성) EF Core 저장 → SignalR "NewMetric"/"AlertOpened"/"AlertResolved" 브로드캐스트
  → EvaluateAlertsAsync (엣지 트리거 상태기계)
```

## 12. Console ↔ 브라우저 — 초기 로드(서버 렌더링) + 실시간 갱신(SignalR/WebSocket)

11절이 "Collector가 보낸 걸 Console이 어떻게 받아 저장하는가"였다면, 이번엔 그 다음
구간 — **저장된 데이터가 브라우저 화면까지 어떻게 도달하는가**다. 이 경로는 완전히
다른 두 메커니즘이 이어붙어 있다.

**왜 하필 이 구간만 WebSocket인가 — 브라우저 샌드박스 제약이 근본 원인.**
WebSocket(RFC 6455, 2011)은 애초에 "브라우저가 서버와 영속적인 양방향 채널을 맺을 수
있게 하려는" 목적으로 만들어진 프로토콜이다. 브라우저 안 JS는 보안 샌드박스 때문에
OS 소켓 API에 직접 접근해 raw TCP를 열 수 없고, HTTP는 원래 "요청 하나 → 응답 하나"
구조라 서버가 먼저 데이터를 밀어넣을 방법이 없었다 — 그 간극을 메우려고 만들어진
게 WebSocket이다(핸드셰이크가 굳이 일반 HTTP 요청 `Upgrade: websocket`으로 시작하는
것도, 브라우저/방화벽/프록시가 이미 다루는 HTTP 포트·인프라를 그대로 타고 들어가려는
이유). 이게 11절에서 정리한 프로토콜 선택과 정확히 맞아떨어진다 — Collector는
브라우저가 아니라 네이티브 C++ 프로세스라 이 제약 자체가 없고, 그래서 순수 TCP +
`PacketHeader` 커스텀 프레이밍으로 충분했다. 반면 Dashboard는 브라우저 샌드박스 안에
갇혀 있으니 SignalR이 WebSocket(안 되면 SSE/롱폴링)을 골라 쓴다 — **구현 스타일의
차이가 아니라, 클라이언트가 브라우저냐 아니냐라는 환경 제약의 차이**다.

### 12-1. 최초 화면 — 평범한 서버 사이드 렌더링(MVC), SignalR과 무관

브라우저가 `/apm/dashboard`로 접속하면 아직 SignalR은 등장하지 않는다. 그냥 일반적인
ASP.NET Core MVC 요청-응답이다.

```csharp
// DashboardController.cs
[HttpGet]
public async Task<IActionResult> Index()
{
    // Id(자동증가 정수, 삽입 순서와 항상 일치)로 정렬 - SQLite가 DateTimeOffset을
    // ORDER BY에서 직접 비교 못 해 NotSupportedException을 던지는 문제를 피함
    var recent = await _db.Metrics.AsNoTracking()
        .OrderByDescending(m => m.Id).Take(20).ToListAsync();
    return View(recent);
}
```

`Index.cshtml`(Razor)이 이 20건을 서버에서 HTML로 직접 찍어낸다 — `<table>` 행뿐 아니라
Chart.js 초기 데이터(`initialPoints` JS 배열)까지 Razor `@foreach`로 서버가 문자열로
구워서 `<script>` 안에 박아 넣는다. 즉 **페이지가 처음 열릴 때 보이는 값들은 SignalR로
받은 게 아니라, DB에서 막 조회해 HTML에 통째로 구운 값**이다. 새로고침하면 이 20건은
그 시점 DB 상태로 다시 구워진다.

> 코드 주석으로 남아있는 실제 버그 이력: Razor 코드 표현식(`@m.Ts...`)은 항상 HTML
> 인코딩을 거치는데, ISO 문자열로 시간을 내려보내면 시간대 기호가 인코딩되면서 JS
> `Date` 파싱이 깨졌다 — 그래서 `t: @m.Ts.ToUnixTimeMilliseconds()`처럼 순수 숫자(인코딩할
> 특수문자가 없음)로 내려보내도록 고쳐져 있다. 실제로 렌더링해보고 나서야 발견된 문제라고
> 명시돼 있다.

### 12-2. 실시간 갱신 시작 — SignalR 연결 수립 (여기서부터 WebSocket)

페이지 로드가 끝난 뒤, `<script>` 안의 이 세 줄이 별도의 연결을 새로 연다.

```javascript
const connection = new signalR.HubConnectionBuilder()
    .withUrl("/apm/hub/metrics")
    .withAutomaticReconnect()
    .build();

connection.on("NewMetric", (m) => { /* ... */ });

connection.start()
    .then(() => { statusEl.textContent = "실시간 연결됨"; })
    .catch(err => { statusEl.textContent = "실시간 연결 실패 - 새로고침으로만 갱신됩니다"; });
```

`connection.start()`가 실행하는 일(SignalR JS 클라이언트 내부 동작):

1. `/apm/hub/metrics/negotiate`로 일반 HTTP POST — 서버와 어떤 전송 방식을 쓸지
   협상(negotiation). 서버가 WebSocket을 지원하면 그걸 쓰기로 응답.
2. 그 응답을 바탕으로 실제 연결을 WebSocket으로 업그레이드(11-3의 답변에서 확인한 대로,
   `ApmModule.cs`에 전송 방식 강제 설정이 없어 기본 협상대로 감).
3. 이후로는 이 WebSocket 연결 하나가 계속 열려있는 상태로 유지된다 — 브라우저 탭이
   열려있는 동안 계속.

`connection.on("NewMetric", handler)`은 **호출이 아니라 등록**이다 — "서버가 나중에
`"NewMetric"`이라는 이름으로 뭔가 보내면 이 함수를 실행해줘"라고 미리 콜백을 걸어두는
것. `connection.start()`를 부르기 전에 미리 등록해두는 순서가 중요하다(연결이 열리자마자
푸시가 올 수도 있으므로).

`withAutomaticReconnect()` — 네트워크가 끊기거나 서버가 재시작돼도 클라이언트 라이브러리가
자동으로 재연결을 시도(지수 백오프). Collector가 `ResilientSender`로 직접 재연결 로직을
짠 것과 대응되는 지점이지만, 여긴 SignalR 클라이언트 라이브러리가 대신 해준다 — 직접
구현할 필요가 없다.

### 12-3. 서버 → 브라우저 푸시 발생 시점과 데이터 흐름

11-3에서 본 것처럼, `MetricsReceiverService.StoreAndBroadcastAsync`가 DB 저장 직후
`_hubContext.Clients.All.SendAsync("NewMetric", record)`를 부르는 순간 — 그 `/apm/hub/metrics`에
연결된 **모든** 브라우저 탭(누구든, 몇 명이든)에 동시에 `record`가 직렬화되어 전송된다.
`Clients.All`이라 대상 필터링이 없다 — 로그인/세션별 구분 없이 이 페이지를 열어둔 모든
클라이언트가 동일하게 받는다.

브라우저 쪽 핸들러:

```javascript
connection.on("NewMetric", (m) => {
    // 표 맨 위에 새 행 삽입, 20행 넘으면 마지막 행 삭제
    const row = document.createElement("tr");
    row.innerHTML = `<td>${new Date(m.ts).toLocaleString()}</td><td>${m.cpuUsagePercent.toFixed(1)}</td>...`;
    tbody.insertBefore(row, tbody.firstChild);
    while (tbody.rows.length > 20) tbody.deleteRow(tbody.rows.length - 1);

    // 5개 Chart.js 차트에 새 포인트 push, 20개 넘으면 앞에서부터 제거(pushToChart 헬퍼)
    pushToChart(cpuChart, label, [m.cpuUsagePercent]);
    // ...
});
```

한 가지 눈에 띄는 부분: C# 쪽 엔티티 프로퍼티는 `CpuUsagePercent`(PascalCase)인데 JS에서는
`m.cpuUsagePercent`(camelCase)로 접근한다 — 오타가 아니라, ASP.NET Core SignalR의
JSON 허브 프로토콜이 **기본적으로 camelCase로 직렬화**하기 때문이다(JS 관례에 맞춘
프레임워크 기본값). `ApmModule.cs`에서 `AddJsonProtocol`로 추가한 커스터마이징은
enum을 문자열로 내려주는 것 하나뿐이고, 이 camelCase 변환 자체는 건드리지 않은 기본
동작이다.

### 12-4. `Alerts/Index.cshtml`도 동일한 패턴을 독립적으로 반복

```javascript
connection.on("AlertOpened", upsertActiveRow);
connection.on("AlertResolved", (alert) => removeActiveRow(alert.id));
```

대시보드와 완전히 별개의 `HubConnectionBuilder` 인스턴스가 **같은 허브 경로**
(`/apm/hub/metrics`)에 다시 연결해, 자신이 관심 있는 이벤트 이름만 구독한다 — 허브
하나가 여러 종류의 이벤트("NewMetric"/"AlertOpened"/"AlertResolved")를 실어 나르고,
각 페이지는 자기가 필요한 이벤트만 `on(...)`으로 골라 듣는 구조다. 서버 쪽엔 이벤트별
채널 분리 같은 개념이 없다 — 그냥 문자열 이름으로 구분되는 메시지일 뿐이다.

### 12-5. 요약 — Console↔브라우저 한 줄 정리

```
[최초 로드] GET /apm/dashboard → DashboardController.Index (DB 조회 20건)
    → Razor가 HTML+초기 JS 배열을 서버에서 완성해 응답 (SignalR 아직 없음)

[실시간 갱신] 브라우저 JS: HubConnectionBuilder → /negotiate(HTTP) → WebSocket 업그레이드
    → connection.start() 로 상시 연결 유지
    → 서버가 DB 저장 직후 Clients.All.SendAsync("NewMetric"/"AlertOpened"/"AlertResolved")
    → connection.on(...) 콜백이 DOM(표 행)/Chart.js 갱신 (camelCase JSON, 폴링 없음)
```

---

## 13. 외부(비-브라우저) 클라이언트가 실시간 데이터를 받으려면 — Qt 데스크톱 클라이언트 연결 지점 조사

Qt 대시보드 포트폴리오 계획(`Docs/QT_MFC_PORTFOLIO_PLAN.md`) 검토 중 "Collector에
TCP로 직접 붙을지" 질문이 나와, Collector/Console 양쪽에서 실제로 열려 있는 지점을 확인.

**Collector 쪽 — 열려 있는 포트는 Agent 전용 하나뿐.**
`Collector/main.cpp:114`의 `acceptor`가 여는 유일한 리스너는 3절/11절에서 다룬
`PacketHeader{size,id}` 프레이밍 + `AesGcmPayload` 암호화(3-2) 위의 protobuf(`Metric.proto`)
메시지를 주고받는 Agent 전용 프로토콜이다. 조회용 평문 포트나 별도 리스너는 없다 — 여기에
Qt가 직접 붙으려면 Qt가 사실상 "Agent 흉내"(AES-256-GCM 핸드셰이크+프레이밍 재구현)를 해야
하고, 이는 계획서의 3~4일 견적에 안 잡혀 있는 별도 작업이다.

**Console 쪽 — HTTP는 서버 렌더링 HTML뿐(JSON API 없음), 실시간 채널은 SignalR/WebSocket.**
`DashboardController`/`AlertsController`(12-1)는 `View(recent)`로 Razor HTML을 반환할 뿐
JSON을 반환하는 `[ApiController]`가 아니다 — REST로 긁을 JSON 엔드포인트는 현재 없다.
반면 12-2~12-4에서 확인한 `/apm/hub/metrics` SignalR 허브는 **표준 SignalR JSON
프로토콜**(`negotiate` → WebSocket 업그레이드, camelCase JSON, `"NewMetric"`/`"AlertOpened"`/
`"AlertResolved"` 이벤트)로 동작한다 — 이건 브라우저 전용 메커니즘이 아니라 문서화된
프로토콜이라, Qt에서 `QWebSocket`으로 같은 handshake+JSON 포맷을 구현하면 **Console 쪽
코드를 한 줄도 안 건드리고** 웹 대시보드와 동등한 실시간 이벤트를 받을 수 있다.

**포트폴리오 계획서의 "A안/B안" 구도 자체가 부정확함 — 실제로는 3가지 경로:**

| 경로 | 구현 비용 | 코어 수정 필요 여부 |
|---|---|---|
| Collector에 직접 TCP(계획서 B안) | 높음 — AES-GCM+protobuf 재구현 | 없음(리스너는 그대로), 단 사실상 Agent 프로토콜 스택 재구현 |
| SQLite 폴링(계획서 A안) | 낮음 — MFC Viewer와 동일 | 없음 |
| **Console SignalR 구독(신규, 미검토였음)** | 중간 — `QWebSocket`으로 SignalR JSON 프로토콜만 구현, AES-GCM 불필요 | 없음(기존 허브 그대로 사용) |

**초기 화면(과거 데이터) 문제는 SignalR만으론 안 풀림** — 허브는 "이후 이벤트"만 push하고,
12-1처럼 최초 20건은 서버가 HTML로 구워 내려준다(JSON 아님). 따라서 Qt가 SignalR 경로를
쓰더라도 **시작 시 과거 데이터는 SQLite 직접 조회로 채우고, 이후 갱신만 SignalR로 받는
하이브리드**가 되어야 함 — "A안이냐 B안이냐" 양자택일이 아니라 "SQLite(초기 적재) +
SignalR(실시간 갱신)" 조합이 셋 중 가장 낮은 구현비용으로 셋 중 가장 신선한 이야깃거리
(웹이 쓰는 프로토콜을 네이티브 클라이언트가 같이 구독)를 준다.

> 이 발견은 포트폴리오 계획서의 "확인 필요" 항목("Collector가 외부 조회용 포트를 여는
> 구조인지")에 대한 실제 답이기도 함 — 아니오, 열지 않음. 대신 Console이 이미 읽기 전용
> 실시간 채널(SignalR)을 열어두고 있다는 게 이번에 새로 확인된 사실.
