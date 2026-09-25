<# 测试项目：历史一千万事件审计的隔离进程运行器
测试项: 接收 Name、Executable、TestArguments, 隐藏启动单个测试, 每秒观察并按 TimeoutSeconds 超时终止。
预期: 未超时且退出 0; 记录路径、参数、时间、退出码到 JSON, 保存并展示 stdout/stderr。
入口: 必填上述三个参数, TimeoutSeconds 默认 1800; 同目录 run-asan.cmd/run-model-final.cmd 使用本脚本。
边界: 不负责编译、用例语义或 stderr 内容判定, 不能把任意零退出直接认定为 Pause 契约全覆盖。
输出固定保留 build/review-10000000-20260923; 这是历史运行器, 非当前统一压力入口。
#>
param(
    [Parameter(Mandatory=$true)][string]$Name,
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string[]]$TestArguments,
    [int]$TimeoutSeconds = 1800
)
$ErrorActionPreference = 'Stop'
$testRepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
Set-Location -LiteralPath $testRepositoryRoot
$auditDirectory = Join-Path $testRepositoryRoot 'build/review-10000000-20260923'
$auditStarted = Get-Date
$auditProcess = Start-Process -FilePath (Resolve-Path -LiteralPath $Executable).Path -ArgumentList $TestArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $auditDirectory "$Name.stdout.log") -RedirectStandardError (Join-Path $auditDirectory "$Name.stderr.log")
$auditHandle = $auditProcess.Handle
$auditTimedOut = $false
while (!$auditProcess.WaitForExit(1000)) {
    if (((Get-Date) - $auditStarted).TotalSeconds -ge $TimeoutSeconds) {
        $auditTimedOut = $true
        $auditProcess.Kill()
        $auditProcess.WaitForExit()
        break
    }
}
$auditProcess.Refresh()
$auditResult = [ordered]@{
    name = $Name
    executable = (Resolve-Path -LiteralPath $Executable).Path
    arguments = $TestArguments
    started = $auditStarted.ToString('o')
    elapsed_seconds = ((Get-Date) - $auditStarted).TotalSeconds
    timed_out = $auditTimedOut
    exit_code = $auditProcess.ExitCode
}
$auditResult | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $auditDirectory "$Name.result.json")
$auditResult | ConvertTo-Json
Get-Content -LiteralPath (Join-Path $auditDirectory "$Name.stdout.log")
Get-Content -LiteralPath (Join-Path $auditDirectory "$Name.stderr.log")
if ($auditTimedOut -or $auditProcess.ExitCode -ne 0) { throw "Test failed: $Name" }
