<#
测试项目：十三套件契约 TSan 证据验收汇总
性质: 只检查既有日志和二进制, 不重新运行压力.
项目: 核对测试/驱动/哈希三个退出码, 拒绝 TSan 诊断, 校验各套件固定场景数,
integration 四组 32/64 深度模式各 10000 轮, notification 六个 ID 各 10000 轮.
预期: 十三套件计数满足固定表, 阳性对照退出 66 且含 data race, 输出 summary.json.
运行: powershell -File tests/archive/pause-contract-audit-20260925/tsan/summarize.ps1.
边界: 依赖完整历史输出; 有限调度通过不证明不存在所有竞态, 阳性对照不计入通过数.
#>
$ErrorActionPreference = 'Stop'
$auditDir = Join-Path (Resolve-Path -LiteralPath "$PSScriptRoot/../../../..").Path 'build/pause-contract-audit-20260925/tsan'
$configurations = @(
    @{ Suite='sender'; Config='debug'; Test='pause_sender_test'; Expected=430000; Pattern='PASS: (\d+) real Pause sender/scheduler/lifetime cases x (\d+) iterations' },
    @{ Suite='integration'; Config='release'; Test='pause_integration_test'; Expected=240000; Pattern='real Pause public integration: (\d+) cases passed' },
    @{ Suite='hook'; Config='debug'; Test='pause_sender_hook_test'; Expected=70000; Pattern='PASS: (\d+) instrumented real Pause callback/lifetime windows x (\d+) iterations' },
    @{ Suite='lifetime'; Config='debug'; Test='pause_task_lifetime_test'; Expected=30000; Pattern='PASS: (\d+) real Pause task lifetime/optional-host cancellation cases x (\d+) iterations' },
    @{ Suite='batch'; Config='release'; Test='pause_batch_extreme_test'; Expected=520000; Pattern='PASS: suite=.* iterations=(\d+)' },
    @{ Suite='extreme'; Config='release'; Test='pause_extreme_test'; Expected=250000; Pattern='PASS shape=.* scopes=(\d+)' },
    @{ Suite='concurrency'; Config='debug'; Test='pause_concurrency_stress_test'; Expected=70000; Pattern='real Pause concurrency: (\d+) complete scopes' },
    @{ Suite='resource'; Config='debug'; Test='pause_resource_test'; Expected=250000; Pattern='Pause resource rollback and no-allocation paths: (\d+) cases passed' },
    @{ Suite='transition'; Config='debug'; Test='pause_transition_extreme_test'; Expected=110000; Pattern='real Pause directed transitions: (\d+) complete scopes' },
    @{ Suite='exception'; Config='debug'; Test='pause_exception_matrix_test'; Expected=270000; Pattern='real Pause exception matrix: ordinary=\d+ concurrent=\d+ complete_scopes=(\d+)' },
    @{ Suite='notification'; Config='debug'; Test='pause_notification_matrix_test'; Expected=60000; Pattern='real Pause notification matrix: (\d+) complete scopes' },
    @{ Suite='outer'; Config='debug'; Test='pause_outer_lifetime_test'; Expected=60000; Pattern='real Pause outer lifetime: (\d+) complete scopes' },
    @{ Suite='host'; Config='debug'; Test='pause_host_failure_test'; Expected=20000; Pattern='Pause host failure: (\d+) complete scopes' }
)
$records = foreach ($configuration in $configurations) {
    $directory = "$($configuration.Suite)-$($configuration.Config)-10000"
    $path = Join-Path $auditDir $directory
    $log = Get-Content -LiteralPath (Join-Path $path "$($configuration.Test)-run.log") -Raw
    $exitCode = [int](Get-Content -LiteralPath (Join-Path $path "$($configuration.Test)-exit.txt") -Raw)
    $driverExit = [int](Get-Content -LiteralPath (Join-Path $path 'driver-exit.txt') -Raw)
    $hashExit = [int](Get-Content -LiteralPath (Join-Path $path 'hash-exit.txt') -Raw)
    if ($exitCode -ne 0 -or $driverExit -ne 0 -or $hashExit -ne 0) { throw "Suite failure: $directory" }
    if ($log -match 'WARNING: ThreadSanitizer|FATAL: ThreadSanitizer|SUMMARY: ThreadSanitizer') { throw "TSan finding: $directory" }
    $successes = [regex]::Matches($log, $configuration.Pattern)
    $count = 0
    foreach ($success in $successes) {
        $value = [int]$success.Groups[1].Value
        if ($success.Groups.Count -eq 3) { $value *= [int]$success.Groups[2].Value }
        $count += $value
    }
    if ($count -ne $configuration.Expected) { throw "Unexpected scenario count: $directory $count" }
    if ($configuration.Suite -eq 'integration') {
        $deepRows = [regex]::Matches($log, 'PASS integration=deep-chain depth=(32|64) mode=(continuous|single-step) complete_scopes=10000')
        if ($deepRows.Count -ne 4) { throw 'Deep-chain repeat count not satisfied' }
    }
    if ($configuration.Suite -eq 'notification') {
        $idRows = [regex]::Matches($log, 'PASS id=NOTIFY-0[1-6]-[^ ]+ complete_scopes=10000 ')
        if ($idRows.Count -ne 6) { throw 'Notification ID count not satisfied' }
    }
    [pscustomobject]@{
        suite = $configuration.Suite
        configuration = $configuration.Config
        repetitions_each = 10000
        complete_scenarios = $count
        exit = $exitCode
        source_hashes_stable = $true
        elapsed_including_compile_seconds = [int](Get-Content -LiteralPath (Join-Path $path 'elapsed-seconds.txt') -Raw)
        log = "$directory/$($configuration.Test)-run.log"
        binary_sha256 = (Get-FileHash -LiteralPath (Join-Path $path $configuration.Test) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$positiveExit = [int](Get-Content -LiteralPath (Join-Path $auditDir 'positive-control-exit.txt') -Raw)
$positiveLog = Get-Content -LiteralPath (Join-Path $auditDir 'positive-control-run.log') -Raw
if ($positiveExit -ne 66 -or $positiveLog -notmatch 'WARNING: ThreadSanitizer: data race') { throw 'Positive control not detected' }
$summary = [pscustomobject]@{
    primary_header = 'pause.hpp (direct coop_pause production header, no POSIX shim)'
    total_complete_scenarios = ($records | Measure-Object -Property complete_scenarios -Sum).Sum
    suites = @($records)
    tsan_options = 'halt_on_error=1:exitcode=66'
    positive_control_expected_exit = $positiveExit
    limits = 'Finite schedules and tested toolchain only. Smoke runs and positive control are excluded from passing scenario counts.'
}
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $auditDir 'summary.json') -Encoding UTF8
$summary | Select-Object total_complete_scenarios, positive_control_expected_exit
