<#
测试项目：千万事件状态模型历史验证
性质: 执行已构建的 Release 状态模型, 不代表真实多线程调度压力.
项目: 深度 10 探索, 100 个种子各 100000 随机步骤, 最多 32 记录及深度 64 链模型;
记录源码/设计/二进制哈希, 600 秒超时后停止进程.
预期: 未超时、退出 0、stderr 为空且前后哈希不变, 否则抛错.
运行: powershell -File tests/archive/pause-audit-20260925/run-model.ps1.
边界: 依赖 build/msvc-release 已有模型程序, 日志仍写入原 build 审计目录.
#>
$ErrorActionPreference = 'Stop'
$auditRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../../..").Path
Set-Location -LiteralPath $auditRoot
$auditExecutable = (Resolve-Path -LiteralPath 'build/msvc-release/pause_state_model_test.exe').Path
$auditOutput = Join-Path $auditRoot 'build/pause-audit-20260925'
$auditArguments = @('--depth','10','--random-steps','100000','--seeds','100','--max-records','32','--chain-depth','64')
$auditInputs = [ordered]@{}
foreach ($auditInput in @('pause.hpp','pause-branch-design.md','tests/pause_state_model_test.cpp','build/msvc-release/pause_state_model_test.exe')) {
    $auditInputs[$auditInput] = (Get-FileHash -LiteralPath $auditInput -Algorithm SHA256).Hash
}
$auditStarted = Get-Date
$auditProcess = Start-Process -FilePath $auditExecutable -ArgumentList $auditArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$auditOutput/model.stdout.log" -RedirectStandardError "$auditOutput/model.stderr.log"
$auditHandle = $auditProcess.Handle
$auditTimeout = $false
try {
    while (-not $auditProcess.WaitForExit(1000)) {
        if (((Get-Date) - $auditStarted).TotalSeconds -ge 600) {
            $auditTimeout = $true
            $auditProcess.Kill()
            $auditProcess.WaitForExit()
            break
        }
    }
    $auditExit = $auditProcess.ExitCode
} finally { $auditProcess.Dispose() }
$auditStable = $true
foreach ($auditInput in $auditInputs.Keys) {
    if ((Get-FileHash -LiteralPath $auditInput -Algorithm SHA256).Hash -ne $auditInputs[$auditInput]) { $auditStable = $false }
}
$auditPassed = -not $auditTimeout -and $auditExit -eq 0 -and $auditStable -and (Get-Item "$auditOutput/model.stderr.log").Length -eq 0
[pscustomobject]@{ executable=$auditExecutable; arguments=$auditArguments; started_at=$auditStarted.ToString('o'); elapsed_seconds=((Get-Date)-$auditStarted).TotalSeconds; exit_code=$auditExit; timed_out=$auditTimeout; hashes=$auditInputs; hashes_stable=$auditStable; passed=$auditPassed } | ConvertTo-Json -Depth 5 | Set-Content "$auditOutput/model.result.json"
Get-Content "$auditOutput/model.stdout.log" -Tail 15
if (-not $auditPassed) { throw 'Model run failed; inspect audit logs.' }
