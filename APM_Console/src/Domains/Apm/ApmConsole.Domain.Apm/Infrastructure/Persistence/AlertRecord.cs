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
