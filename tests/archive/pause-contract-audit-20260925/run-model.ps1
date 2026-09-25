<#
测试项目：亿级随机事件状态模型审计
性质: 调用已有 Release 模型程序, 独立于真实 Pause 多线程实现压力.
项目: 深度 10 探索, 100 个种子各 1000000 随机步骤, 最多 32 记录、链深度 64;
保存命令、运行时间、退出码与源码/设计/二进制前后哈希.
预期: 7200 秒内退出 0, stderr 为空且所有输入哈希不变, 否则抛错.
运行: powershell -File tests/archive/pause-contract-audit-20260925/run-model.ps1.
边界: 参数为此历史批次固定值, 不宣称真实宿主任意递归深度安全; 日志留在 build.
#>
$ErrorActionPreference = 'Stop'
$auditRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../../..").Path
Set-Location -LiteralPath $auditRoot
$auditExecutable = (Resolve-Path -LiteralPath 'build/msvc-release/pause_state_model_test.exe').Path
$auditOutput = Join-Path $auditRoot 'build/pause-contract-audit-20260925'
$auditArguments = @('--depth','10','--random-steps','1000000','--seeds','100','--max-records','32','--chain-depth','64')
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
        if (((Get-Date) - $auditStarted).TotalSeconds -ge 7200) {
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
