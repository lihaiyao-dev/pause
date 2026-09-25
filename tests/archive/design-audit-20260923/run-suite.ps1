<# 测试项目：历史设计审计的隔离进程运行器
测试项: 用 Name、Executable、TestArguments 启动单个隐藏测试进程, 轮询并按 TimeoutSeconds 控制总时间。
预期: 未超时且零退出; 超时终止子进程并写结果, 非零退出或超时抛出 Test failed。
入口: 三个命名参数必填, TimeoutSeconds 默认 1800; 同目录 run-asan.cmd/build-extra.cmd 使用。
保留 stdout/stderr、时间和参数 JSON 到 build/design-audit-20260923。
边界: 不负责构建、断言语义或 stderr 内容判定; 历史运行器不能替代当前完整 Pause 契约验收。
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
$auditDirectory = Join-Path $testRepositoryRoot 'build/design-audit-20260923'
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
