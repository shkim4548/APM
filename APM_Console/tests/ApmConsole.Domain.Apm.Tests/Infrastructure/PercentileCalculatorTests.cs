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
