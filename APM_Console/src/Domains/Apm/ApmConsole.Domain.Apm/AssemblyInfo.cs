using System.Runtime.CompilerServices;

// 테스트 프로젝트가 internal 멤버(MetricsReceiverService.ReadExactAsync/DecryptAndParse)에
// 접근할 수 있도록 허용 - 이 둘은 private으로 완전히 숨기기보다 테스트 대상으로 삼는 게
// 가치가 커서 internal로 열어두고 여기서만 가시성을 확장함(공개 API 표면은 그대로 유지).
[assembly: InternalsVisibleTo("ApmConsole.Domain.Apm.Tests")]
