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
