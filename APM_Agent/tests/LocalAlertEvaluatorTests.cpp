#include <gtest/gtest.h>
#include <cstdio>
#include <unistd.h>

#include "../Agent/LocalAlertEvaluator.h"
#include "../Agent/ThresholdSet.h"
#include "../Storage/AgentAlertStore.h"

namespace
{
    // AgentAlertStore는 실제 SQLite 파일을 열어야 하므로, 테스트마다 프로세스 PID+테스트
    // 이름으로 고유한 임시 파일을 만들고 소멸자에서 정리한다(WAL 모드라 -wal/-shm 사이드카도 같이 지움).
    class TempDbFile
    {
    public:
        explicit TempDbFile(const String& suffix)
            : _path("test_agent_alerts_" + std::to_string(::getpid()) + "_" + suffix + ".db")
        {
        }
        ~TempDbFile()
        {
            std::remove(_path.c_str());
            std::remove((_path + "-wal").c_str());
            std::remove((_path + "-shm").c_str());
        }
        const String& Path() const { return _path; }

    private:
        String _path;
    };

    apm::Metric MakeMetric(double cpu, long long memUsed, long long memTotal)
    {
        apm::Metric m;
        m.set_cpu_usage_percent(cpu);
        m.set_mem_used_bytes(memUsed);
        m.set_mem_total_bytes(memTotal);
        m.set_disk_used_bytes(0);
        m.set_disk_total_bytes(0);
        m.set_tcp_rtt_us(0);
        return m;
    }
}

// ------------------------
//   EvaluateTransition (순수 함수) - Console AlertEvaluator.Evaluate()를 그대로 이식,
//   AlertEvaluatorTests.cs와 동일한 5개 케이스를 그대로 포팅해 판정 동등성을 보장한다.
// ------------------------

TEST(EvaluateTransition, 임계치를_처음_넘으면_Opened를_반환한다)
{
    EXPECT_EQ(EvaluateTransition(95, 90, false), AlertTransition::Opened);
}

TEST(EvaluateTransition, 이미_열려있는_상태에서_계속_넘으면_None을_반환한다)
{
    // 스팸 방지 핵심 - 매 수치마다 다시 Opened가 나오면 안 됨.
    EXPECT_EQ(EvaluateTransition(95, 90, true), AlertTransition::None);
}

TEST(EvaluateTransition, 열려있던_상태에서_임계치_아래로_내려가면_Resolved를_반환한다)
{
    EXPECT_EQ(EvaluateTransition(80, 90, true), AlertTransition::Resolved);
}

TEST(EvaluateTransition, 원래도_정상이었고_계속_정상이면_None을_반환한다)
{
    EXPECT_EQ(EvaluateTransition(50, 90, false), AlertTransition::None);
}

TEST(EvaluateTransition, 임계치와_정확히_같으면_초과로_간주한다)
{
    // ">=" 채택 - 경계값이 "아직 안전"으로 새는 쪽보다 "이미 위험"으로 잡는 쪽이 알림 목적에 맞음.
    EXPECT_EQ(EvaluateTransition(90, 90, false), AlertTransition::Opened);
}

// ------------------------
//   LocalAlertEvaluator::OnNewMetric (AgentAlertStore 연동 - 임시 SQLite 파일 사용)
// ------------------------

TEST(LocalAlertEvaluator, CPU가_임계치를_넘으면_로컬_DB에_알림을_연다)
{
    TempDbFile db("open");
    ThresholdSet thresholds; // CPU 기본 임계치 90
    AgentAlertStore store(db.Path());
    LocalAlertEvaluator evaluator(thresholds, store);

    evaluator.OnNewMetric(MakeMetric(/*cpu=*/95.0, /*memUsed=*/0, /*memTotal=*/0));

    auto open = store.FindOpen(AlertMetricType::CpuPercent);
    ASSERT_TRUE(open.has_value());
    EXPECT_DOUBLE_EQ(open->triggerValue, 95.0);
}

TEST(LocalAlertEvaluator, 다시_정상으로_돌아오면_알림이_닫힌다)
{
    TempDbFile db("resolve");
    ThresholdSet thresholds;
    AgentAlertStore store(db.Path());
    LocalAlertEvaluator evaluator(thresholds, store);

    evaluator.OnNewMetric(MakeMetric(95.0, 0, 0)); // 열림
    evaluator.OnNewMetric(MakeMetric(50.0, 0, 0)); // 해소

    EXPECT_FALSE(store.FindOpen(AlertMetricType::CpuPercent).has_value());
}

TEST(LocalAlertEvaluator, 메모리_총량이_0이면_0퍼센트로_처리해_알림을_안_연다)
{
    // OnNewMetric의 division-by-zero 가드 - mem_total_bytes()==0일 때 memPercent를
    // 그대로 나누면 안 되므로 0.0으로 방어하는데, 그 가드가 실제로 동작하는지 확인.
    TempDbFile db("divzero");
    ThresholdSet thresholds;
    AgentAlertStore store(db.Path());
    LocalAlertEvaluator evaluator(thresholds, store);

    evaluator.OnNewMetric(MakeMetric(0.0, 100, 0)); // memTotal=0

    EXPECT_FALSE(store.FindOpen(AlertMetricType::MemoryPercent).has_value());
}

TEST(LocalAlertEvaluator, 같은_수치가_반복돼도_알림을_중복으로_열지_않는다)
{
    TempDbFile db("noduplicate");
    ThresholdSet thresholds;
    AgentAlertStore store(db.Path());
    LocalAlertEvaluator evaluator(thresholds, store);

    evaluator.OnNewMetric(MakeMetric(95.0, 0, 0));
    evaluator.OnNewMetric(MakeMetric(96.0, 0, 0));
    evaluator.OnNewMetric(MakeMetric(97.0, 0, 0));

    auto open = store.FindOpen(AlertMetricType::CpuPercent);
    ASSERT_TRUE(open.has_value());
    EXPECT_DOUBLE_EQ(open->triggerValue, 95.0); // 최초 오픈 시점 값 유지, 갱신되지 않음
}
