<# 测试项目：debug-release-stress/run.ps1
对 Debug/Release 的 integration/sender/hook/lifetime 各执行 --iterations 10000.
预期: 每项 1800 秒内退出 0, stderr 为空, 源码/二进制/构建配置哈希前后一致.
入口: powershell -File tests/archive/debug-release-stress/run.ps1; 需要两配置已构建产物, 输出保留 build/debug-release-stress.
边界: expected_cases 是历史计数, 未逐行核对输出, 不直接代表当前全部场景数.
归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
#>
$ErrorActionPreference = 'Stop'
$pauseRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../../..").Path
$pauseOutput = Join-Path $pauseRoot 'build/debug-release-stress'
$pauseResults = [System.Collections.Generic.List[object]]::new()
$pauseTests = [ordered]@{
    pause_integration_test = 200004
    pause_sender_test = 270000
    pause_sender_hook_test = 70000
    pause_task_lifetime_test = 30000
}
$timeoutSeconds = 1800

foreach ($configuration in @('Debug', 'Release')) {
    $buildDirectory = Join-Path $pauseRoot "build/msvc-$($configuration.ToLowerInvariant())"
    foreach ($testName in $pauseTests.Keys) {
        $label = "$($configuration.ToLowerInvariant())-$testName"
        $executable = Join-Path $buildDirectory "$testName.exe"
        $stdout = Join-Path $pauseOutput "$label.stdout.log"
        $stderr = Join-Path $pauseOutput "$label.stderr.log"
        $hashPaths = [ordered]@{
            header = (Join-Path $pauseRoot 'pause.hpp')
            source = (Join-Path $pauseRoot "tests/$testName.cpp")
            support_header = (Join-Path $pauseRoot 'tests/pause_sender_test_support.hpp')
            binary = $executable
            compile_database = (Join-Path $buildDirectory 'compile_commands.json')
            cache = (Join-Path $buildDirectory 'CMakeCache.txt')
        }
        if ($testName -eq 'pause_sender_hook_test') {
            $hashPaths.instrumented_stop_token = Join-Path $buildDirectory 'pause-instrumented-stdexec/stdexec/stop_token.hpp'
        }
        $hashesBefore = [ordered]@{}
        foreach ($key in $hashPaths.Keys) {
            $hashesBefore[$key] = (Get-FileHash -LiteralPath $hashPaths[$key] -Algorithm SHA256).Hash
        }
        $started = Get-Date
        Write-Output "Started $label at $($started.ToString('o'))"
        $process = Start-Process -FilePath $executable -ArgumentList @('--iterations', '10000') `
            -WorkingDirectory $pauseRoot -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $processHandle = $process.Handle
        $timedOut = $false
        while (-not $process.WaitForExit(1000)) {
            if (((Get-Date) - $started).TotalSeconds -ge $timeoutSeconds) {
                $timedOut = $true
                $process.Kill()
                $process.WaitForExit()
                break
            }
        }
        $exitCode = $process.ExitCode
        $process.Dispose()
        $hashesAfter = [ordered]@{}
        $hashesStable = $true
        foreach ($key in $hashPaths.Keys) {
            $hashesAfter[$key] = (Get-FileHash -LiteralPath $hashPaths[$key] -Algorithm SHA256).Hash
            if ($hashesAfter[$key] -ne $hashesBefore[$key]) { $hashesStable = $false }
        }
        $stderrBytes = (Get-Item -LiteralPath $stderr).Length
        $result = [pscustomobject][ordered]@{
            configuration = $configuration
            name = $testName
            arguments = @('--iterations', '10000')
            integration_seed = $(if ($testName -eq 'pause_integration_test') { '4542363620118899781' } else { $null })
            expected_cases = $pauseTests[$testName]
            exit_code = $exitCode
            timed_out = $timedOut
            elapsed_seconds = ((Get-Date) - $started).TotalSeconds
            started_at = $started.ToString('o')
            stdout = $stdout
            stderr = $stderr
            stderr_bytes = $stderrBytes
            hash_paths = $hashPaths
            sha256_before = $hashesBefore
            sha256_after = $hashesAfter
            hashes_stable = $hashesStable
            passed = (-not $timedOut -and $exitCode -eq 0 -and $stderrBytes -eq 0 -and $hashesStable)
        }
        $pauseResults.Add($result)
        $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$pauseOutput/$label.result.json"
        $pauseResults | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$pauseOutput/results.json"
        Get-Content -LiteralPath $stdout
        if (-not $result.passed) { throw "Test failed: $label; inspect its result and logs." }
        Write-Output "Passed $label in $($result.elapsed_seconds) seconds"
    }
}
[pscustomobject][ordered]@{
    completed_at = (Get-Date).ToString('o')
    runs = $pauseResults.Count
    cases = ($pauseResults | Measure-Object -Property expected_cases -Sum).Sum
    passed = @($pauseResults | Where-Object { -not $_.passed }).Count -eq 0
    debug_cases = ($pauseResults | Where-Object configuration -eq 'Debug' | Measure-Object -Property expected_cases -Sum).Sum
    release_cases = ($pauseResults | Where-Object configuration -eq 'Release' | Measure-Object -Property expected_cases -Sum).Sum
} | ConvertTo-Json | Set-Content -LiteralPath "$pauseOutput/summary.json"
Get-Content -LiteralPath "$pauseOutput/summary.json"
