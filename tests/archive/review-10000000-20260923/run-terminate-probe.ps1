<# 测试项目：历史八类 body/宿主组合错误的预期终止
测试项: 对 sender error、分派/factory 抛出、内部 connect 抛出、调度后 error 隔离运行场景 1..8。
预期: 30 秒内结束, 退出码 86, stdout 以当前场景 TERMINATE_OK 开头且 stderr 为空。
入口: 同目录 build-run-terminate-probe.cmd; 使用 build 中的 terminate-composition-probe.exe。
保存各场景日志、结果 JSON 及源码/可执行文件/脚本 SHA256, 任一条件不符返回 1。
边界: 不把任意崩溃当预期终止; 此程序仅组合探针, 不运行真实 Pause 状态机或回调所有权。
#>
$ErrorActionPreference = 'Stop'
$testRepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
Set-Location -LiteralPath $testRepositoryRoot
$probeRoot = Join-Path $testRepositoryRoot 'build/review-10000000-20260923'
$scenarios = @(
    'body_sender_error', 'body_let_dispatch_throw', 'body_internal_connect_throw',
    'body_error_after_run_loop_schedule', 'host_let_stopped_factory_throw',
    'host_let_dispatch_throw', 'host_internal_connect_throw',
    'host_error_after_unstoppable_run_loop_schedule'
)
$results = @()
for ($index = 0; $index -lt $scenarios.Count; ++$index) {
    $scenario = $index + 1
    $name = $scenarios[$index]
    $stdout = Join-Path $probeRoot "terminate-$scenario.stdout.log"
    $stderr = Join-Path $probeRoot "terminate-$scenario.stderr.log"
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath (Join-Path $probeRoot 'terminate-composition-probe.exe') `
        -ArgumentList @("$scenario") -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Retain the native handle before waiting; Windows PowerShell otherwise
    # loses ExitCode for a process which exits immediately after Start-Process.
    $processHandle = $process.Handle
    $finished = $process.WaitForExit(30000)
    if (-not $finished) {
        $process.Kill()
        $process.WaitForExit()
    }
    $process.Refresh()
    $watch.Stop()
    $output = [System.IO.File]::ReadAllText($stdout)
    $errors = [System.IO.File]::ReadAllText($stderr)
    $passed = $finished -and $process.ExitCode -eq 86 `
        -and $output.StartsWith("TERMINATE_OK scenario=$scenario ") `
        -and $errors.Length -eq 0
    $results += [pscustomobject]@{
        scenario = $scenario; name = $name; exit_code = $process.ExitCode
        timed_out = -not $finished; seconds = $watch.Elapsed.TotalSeconds
        passed = $passed; stdout = $stdout; stderr = $stderr
    }
    Write-Output "scenario=$scenario name=$name passed=$passed exit=$($process.ExitCode)"
    $process.Dispose()
}
$results | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $probeRoot 'terminate-probe.results.json')
Get-FileHash -Algorithm SHA256 -LiteralPath `
    (Join-Path $PSScriptRoot 'terminate-composition-probe.cpp'), `
    (Join-Path $probeRoot 'terminate-composition-probe.exe'), `
    (Join-Path $PSScriptRoot 'run-terminate-probe.ps1') |
    Format-List | Out-File -LiteralPath (Join-Path $probeRoot 'terminate-probe.sha256.log')
if (@($results | Where-Object { -not $_.passed }).Count -ne 0) { exit 1 }
Write-Output 'PASS: 8 checked termination scenarios (composition probe only)'
