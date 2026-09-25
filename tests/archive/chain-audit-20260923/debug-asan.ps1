<# 测试项目：历史 ASan 宿主递归崩溃 CDB 定点诊断
测试项: 对 fork/error-fast 深度 128 和 chain/stopped 深度 2048 捕获目标异常、寄存器、TEB 栈界及调用栈。
预期: 输出调试证据及进程结果; 全组 60 秒预算, 超时终止调试进程树并明确失败。
入口: 同目录 debug-asan.cmd; 依赖已构建 task-asan.exe/PDB 和 Windows SDK x64 CDB。
边界: 这些是历史失败调查参数, 不是当前压力入口, 不执行即可保留归因证据。
调试器零退出不代表被测程序通过; 无 pause.hpp, 不将依赖栈深极限直接归因为 Pause 设计错误。
#>
$ErrorActionPreference = 'Stop'
$testRepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
Set-Location -LiteralPath $testRepositoryRoot
$auditDirectory = Join-Path $testRepositoryRoot 'build/chain-audit-20260923'
$debuggerPath = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
$diagnostics = @(
    @{ Label = 'asan-fork-error-fast-d128'; Arguments = '--depth 128 --topology fork --outcome error-fast' },
    @{ Label = 'asan-chain-stopped-d2048'; Arguments = '--depth 2048 --topology chain --outcome stopped' }
)
$timer = [System.Diagnostics.Stopwatch]::StartNew()
$results = @()
foreach ($diagnostic in $diagnostics) {
    $remainingMilliseconds = 60000 - [int]$timer.ElapsedMilliseconds
    if ($remainingMilliseconds -le 0) { throw 'Total debugger budget of 60 seconds exceeded.' }
    $prefix = Join-Path $auditDirectory ('debug-target-' + $diagnostic.Label)
    $arguments = '-G -y "' + $auditDirectory + '" -cf "' + (Join-Path $PSScriptRoot 'debug-asan-target-exceptions.txt') + '" -logo "' + $prefix + '.cdb.log" "' + (Join-Path $auditDirectory 'task-asan.exe') + '" ' + $diagnostic.Arguments
    $process = Start-Process -FilePath $debuggerPath -ArgumentList $arguments -WorkingDirectory $testRepositoryRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput ($prefix + '.stdout.log') -RedirectStandardError ($prefix + '.stderr.log')
    $processHandle = $process.Handle
    $timedOut = -not $process.WaitForExit($remainingMilliseconds)
    if ($timedOut) {
        & taskkill.exe /PID $process.Id /T /F | Out-File -LiteralPath ($prefix + '.termination.log')
        $process.WaitForExit()
    }
    $process.Refresh()
    $results += [pscustomobject]@{ label = $diagnostic.Label; debugger_pid = $process.Id; debugger_exit_code = $process.ExitCode; timed_out = $timedOut; target_arguments = $diagnostic.Arguments; cumulative_seconds = $timer.Elapsed.TotalSeconds }
    $results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $auditDirectory 'debug-asan-target-results.json')
    if ($timedOut) { throw 'Debugger process tree exceeded total 60 second budget and was terminated.' }
}
$results | Format-Table -AutoSize
