<#
测试项目：压力运行器超时与子进程清理自检
性质: 故意让 death 压测在 1 秒超时, 不是产品通过或失败场景.
项目: 启动同目录 cmd, 观察目标 exe 的父子 PID, 驱动退出后间隔 250ms 查询两次残留进程,
读取运行器 result.json 并记录 expected_timeout_observed 与 process_cleanup_verified.
预期: 驱动非零退出、结果明确 timed_out, 两次后置查询均无残留且进程查询未失败.
运行: powershell -File tests/archive/pause-contract-completion-20260925/timeout-selfcheck/run-selfcheck.ps1.
边界: 脚本生成判定字段但不据此抛错, 必须读取 selfcheck.result.json; 不计入产品压测数量.
#>
$ErrorActionPreference = 'Stop'
$pauseSelfRoot = 'D:\workspace\pause'
$pauseSelfOut = Join-Path $pauseSelfRoot 'build/pause-contract-completion-20260925/timeout-selfcheck'
$pauseSelfExe = Join-Path $pauseSelfOut 'pause_contract_death_test.exe'
$pauseSelfCmd = Join-Path $PSScriptRoot 'run-selfcheck.cmd'
$pauseSelfObserved = [System.Collections.Generic.Dictionary[int,object]]::new()
$pauseSelfProcessChecks = [System.Collections.Generic.List[object]]::new()
$pauseSelfQueryError = $null

function Get-SelfcheckProcesses {
    @(Get-CimInstance -ClassName Win32_Process -Filter "Name = 'pause_contract_death_test.exe'" |
        Where-Object { $_.ExecutablePath -eq $pauseSelfExe } |
        ForEach-Object { [pscustomobject]@{
            process_id = [int]$_.ProcessId
            parent_process_id = [int]$_.ParentProcessId
            executable = [string]$_.ExecutablePath
        } })
}

'powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run-pause-stress.ps1 -OnlyTest pause_contract_death_test -TerminationIterations 1000 -TimeoutSeconds 1 -OutputDirectory build/pause-contract-completion-20260925/timeout-selfcheck' |
    Set-Content -LiteralPath (Join-Path $pauseSelfOut 'command.log')
$pauseSelfStarted = Get-Date
$pauseSelfProcess = Start-Process -FilePath $env:ComSpec -ArgumentList @('/c', $pauseSelfCmd) -WindowStyle Hidden -PassThru -WorkingDirectory $pauseSelfRoot -RedirectStandardOutput (Join-Path $pauseSelfOut 'driver.stdout.log') -RedirectStandardError (Join-Path $pauseSelfOut 'driver.stderr.log')
try {
    $pauseSelfHandle = $pauseSelfProcess.Handle
    while (-not $pauseSelfProcess.WaitForExit(100)) {
        if ($null -eq $pauseSelfQueryError) {
            try {
                foreach ($pauseSelfMatch in (Get-SelfcheckProcesses)) {
                    $pauseSelfObserved[$pauseSelfMatch.process_id] = $pauseSelfMatch
                }
            } catch { $pauseSelfQueryError = $_.Exception.Message }
        }
    }
    $pauseSelfExit = $pauseSelfProcess.ExitCode
} finally { $pauseSelfProcess.Dispose() }
$pauseSelfExit | Set-Content -LiteralPath (Join-Path $pauseSelfOut 'driver-exit.txt')

if ($null -eq $pauseSelfQueryError) {
    try {
        foreach ($pauseSelfAttempt in 1..2) {
            $pauseSelfMatches = @(Get-SelfcheckProcesses)
            $pauseSelfProcessChecks.Add([pscustomobject]@{
                checked_at = (Get-Date).ToString('o')
                remaining = @($pauseSelfMatches)
            })
            if ($pauseSelfAttempt -eq 1) { Start-Sleep -Milliseconds 250 }
        }
    } catch { $pauseSelfQueryError = $_.Exception.Message }
}

$pauseSelfResultPath = Join-Path $pauseSelfOut 'pause_contract_death_test.result.json'
$pauseSelfRunnerResult = if (Test-Path -LiteralPath $pauseSelfResultPath) {
    [System.IO.File]::ReadAllText($pauseSelfResultPath) | ConvertFrom-Json
} else { $null }
$pauseSelfRemaining = @($pauseSelfProcessChecks | ForEach-Object { $_.remaining }).Count
$pauseSelfResult = [pscustomobject][ordered]@{
    category = 'intentional-runner-timeout-selfcheck'
    production_pass_count = 0
    production_failure_count = 0
    driver_exit_code = $pauseSelfExit
    runner_result_present = $null -ne $pauseSelfRunnerResult
    runner_timed_out = $pauseSelfRunnerResult.timed_out
    runner_test_exit_code = $pauseSelfRunnerResult.exit_code
    observed_test_processes = @($pauseSelfObserved.Values)
    post_exit_process_checks = @($pauseSelfProcessChecks)
    process_query_error = $pauseSelfQueryError
    expected_timeout_observed = $pauseSelfExit -ne 0 -and $null -ne $pauseSelfRunnerResult -and $pauseSelfRunnerResult.timed_out
    process_cleanup_verified = $null -eq $pauseSelfQueryError -and $pauseSelfProcessChecks.Count -eq 2 -and $pauseSelfRemaining -eq 0
    interpretation = 'A forced one-second timeout validates the runner cleanup path; it is neither a passing nor failing product stress result.'
    started_at = $pauseSelfStarted.ToString('o')
    finished_at = (Get-Date).ToString('o')
}
$pauseSelfResult | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $pauseSelfOut 'selfcheck.result.json')
$pauseSelfResult | ConvertTo-Json -Depth 6
