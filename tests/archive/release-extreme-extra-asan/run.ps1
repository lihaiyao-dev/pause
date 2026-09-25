<# 测试项目：release-extreme-extra-asan/run.ps1
可选调用同目录 build.cmd, 对 sender/resource 各执行 --iterations 1000.
预期: 每项 300 秒内退出 0, stderr 为空, 输出匹配 43/25 个历史场景, 构建前后源码哈希不变.
入口: 从仓库根 powershell -File tests/archive/release-extreme-extra-asan/run.ps1 [-SkipBuild], 结果留在 build/release-extreme-extra-asan.
前提: MSVC ASan 环境及对应输出格式; 计数固定, 不自动适应当前测试变化.
归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
#>
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$pauseOutput = (Resolve-Path -LiteralPath 'build/release-extreme-extra-asan').Path
$sourceFiles = @('pause.hpp','tests/pause_sender_test.cpp','tests/pause_sender_test_support.hpp','tests/pause_resource_test.cpp')
$hashesBefore = @{}
foreach ($sourceFile in $sourceFiles) { $hashesBefore[$sourceFile] = (Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash }
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.cmd') }
if (-not $SkipBuild -and $LASTEXITCODE -ne 0) { throw ('ASan build exited ' + $LASTEXITCODE) }
foreach ($sourceFile in $sourceFiles) {
    if ((Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash -ne $hashesBefore[$sourceFile]) { throw ('Source changed during build: ' + $sourceFile) }
}
$results = @()
foreach ($testName in @('pause_sender_test','pause_resource_test')) {
    $executable = Join-Path $pauseOutput ($testName + '.exe')
    $stdout = Join-Path $pauseOutput ($testName + '.stdout.log')
    $stderr = Join-Path $pauseOutput ($testName + '.stderr.log')
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $startedAt = (Get-Date).ToString('o')
    $process = Start-Process -FilePath $executable -ArgumentList @('--iterations','1000') -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $processHandle = $process.Handle
    $timedOut = $false
    while (-not $process.WaitForExit(1000)) {
        if ($watch.Elapsed.TotalSeconds -gt 300) { $timedOut = $true; $process.Kill(); $process.WaitForExit(); break }
    }
    $watch.Stop()
    $stderrBytes = (Get-Item -LiteralPath $stderr).Length
    $cases = if ($testName -eq 'pause_sender_test') { 43 } else { 25 }
    $stdoutText = Get-Content -LiteralPath $stdout -Raw
    $coverageMatched = if ($testName -eq 'pause_sender_test') { $stdoutText -match 'PASS: 43 .* x 1000 iterations' } else { $stdoutText -match '25000 cases passed \(1000 repetitions\)' }
    $result = [ordered]@{test=$testName; configuration='MSVC x64 /O2 /Ob2 /DNDEBUG /MD /Zi /fsanitize=address'; arguments=@('--iterations','1000'); iterations_per_case=1000; cases=$cases; completed_scenarios=$(if ($process.ExitCode -eq 0 -and $coverageMatched) { $cases * 1000 } else { 0 }); timeout_seconds=300; timed_out=$timedOut; exit_code=$process.ExitCode; elapsed_seconds=$watch.Elapsed.TotalSeconds; stderr_bytes=$stderrBytes; coverage_output_matched=$coverageMatched; source_sha256=$hashesBefore['tests/' + $testName + '.cpp']; header_sha256=$hashesBefore['pause.hpp']; sender_support_sha256=$hashesBefore['tests/pause_sender_test_support.hpp']; executable_sha256=(Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash; started_at=$startedAt}
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $pauseOutput ($testName + '.result.json')) -Encoding utf8
    $results += [pscustomobject]$result
    if ($timedOut -or $process.ExitCode -ne 0 -or $stderrBytes -ne 0 -or -not $coverageMatched) { throw ('ASan verification failed: ' + $testName) }
    Write-Output ('PASS: ASan ' + $testName + ' ' + $cases + ' cases x 1000, elapsed=' + $watch.Elapsed.TotalSeconds)
}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $pauseOutput 'results.json') -Encoding utf8
