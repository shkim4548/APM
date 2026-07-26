namespace ApmConsole.Domain.Apm.Models;

public record OperationStatsRow(
    string OperationName,
    int Count,
    double P50Ms,
    double P95Ms,
    double P99Ms,
    double SuccessRatePercent);

public record TracesViewModel(string Window, List<OperationStatsRow> Rows);
