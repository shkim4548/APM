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
