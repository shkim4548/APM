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
