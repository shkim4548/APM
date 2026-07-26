using ApmConsole.Domain.Apm.Infrastructure.Persistence;

namespace ApmConsole.Domain.Apm.Models;

public record AlertsViewModel(
    List<AlertThreshold> Thresholds,
    List<AlertRecord> ActiveAlerts,
    List<AlertRecord> RecentHistory);
