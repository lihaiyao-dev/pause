<#
测试项目：首轮 TSan 压测证据汇总
性质: 读取历史日志与二进制, 不重新运行压力测试.
项目: 核对 sender、integration、hook、lifetime、batch、extreme、concurrency 七组退出码,
拒绝 TSan 诊断, 按各自 PASS 格式检查完整场景数并记录二进制 SHA-256.
预期: 场景计数符合固定表, 输出 summary.json; 另外记录阳性对照退出码和两组源文件稳定性.
运行: powershell -File tests/archive/pause-audit-20260925/tsan/summarize.ps1.
边界: 此版仅记录阳性对照/哈希稳定字段, 未将它们全部作为 throw 条件; 不能以脚本退出码代替复核.
#>
$ErrorActionPreference = 'Stop'
$auditDir = Join-Path (Resolve-Path -LiteralPath "$PSScriptRoot/../../../..").Path 'build/pause-audit-20260925/tsan'
$configurations = @(
    @{ Suite='sender'; Dir='sender-debug'; Test='pause_sender_test'; Expected=43000; Pattern='PASS: (\d+) real Pause sender/scheduler/lifetime cases x (\d+) iterations' },
    @{ Suite='integration'; Dir='integration-debug'; Test='pause_integration_test'; Expected=20004; Pattern='real Pause public integration: (\d+) cases passed' },
    @{ Suite='hook'; Dir='hook-debug'; Test='pause_sender_hook_test'; Expected=7000; Pattern='PASS: (\d+) instrumented real Pause callback/lifetime windows x (\d+) iterations' },
    @{ Suite='lifetime'; Dir='lifetime-debug'; Test='pause_task_lifetime_test'; Expected=3000; Pattern='PASS: (\d+) real Pause task lifetime/optional-host cancellation cases x (\d+) iterations' },
    @{ Suite='batch'; Dir='batch-release'; Test='pause_batch_extreme_test'; Expected=52000; Pattern='PASS: suite=.* iterations=(\d+)' },
    @{ Suite='extreme'; Dir='extreme-release'; Test='pause_extreme_test'; Expected=25000; Pattern='PASS shape=.* scopes=(\d+)' },
    @{ Suite='concurrency'; Dir='concurrency-debug'; Test='pause_concurrency_stress_test'; Expected=7000; Pattern='real Pause concurrency: (\d+) complete scopes' }
)
$records = foreach ($configuration in $configurations) {
    $log = Get-Content -LiteralPath (Join-Path $auditDir "$($configuration.Dir)/$($configuration.Test)-run.log") -Raw
    $exitCode = [int](Get-Content -LiteralPath (Join-Path $auditDir "$($configuration.Dir)/$($configuration.Test)-exit.txt") -Raw)
    if ($exitCode -ne 0) { throw "Failed suite: $($configuration.Suite) exit $exitCode" }
    if ($log -match 'WARNING: ThreadSanitizer|FATAL: ThreadSanitizer|SUMMARY: ThreadSanitizer') { throw "TSan finding: $($configuration.Suite)" }
    $successes = [regex]::Matches($log, $configuration.Pattern)
    $count = 0
    foreach ($success in $successes) {
        $value = [int]$success.Groups[1].Value
        if ($success.Groups.Count -eq 3) { $value *= [int]$success.Groups[2].Value }
        $count += $value
    }
    if ($count -ne $configuration.Expected) { throw "Unexpected scenario count: $($configuration.Suite) $count" }
    [pscustomobject]@{
        suite = $configuration.Suite
        complete_scenarios = $count
        exit = $exitCode
        log = "$($configuration.Dir)/$($configuration.Test)-run.log"
        binary_sha256 = (Get-FileHash -LiteralPath (Join-Path $auditDir "$($configuration.Dir)/$($configuration.Test)") -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$summary = [pscustomobject]@{
    primary_header = 'pause.hpp (direct coop_pause production header, no POSIX shim)'
    total_complete_scenarios = ($records | Measure-Object -Property complete_scenarios -Sum).Sum
    suites = @($records)
    tsan_options = 'halt_on_error=1:exitcode=66'
    positive_control_expected_exit = [int](Get-Content -LiteralPath (Join-Path $auditDir 'positive-control-exit.txt') -Raw)
    source_hashes_stable = -not [bool](Select-String -LiteralPath (Join-Path $auditDir 'source-sha256-after-check.txt') -Pattern 'FAILED')
    concurrency_source_hashes_stable = -not [bool](Select-String -LiteralPath (Join-Path $auditDir 'concurrency-debug/source-sha256-after-check.txt') -Pattern 'FAILED')
    limits = 'Finite schedules and tested toolchain only; no proof that all races/lifetime bugs are absent. Positive control is excluded from passing scenario counts.'
}
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $auditDir 'summary.json') -Encoding UTF8
$summary | Select-Object total_complete_scenarios, positive_control_expected_exit, source_hashes_stable
