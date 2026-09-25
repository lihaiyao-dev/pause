<#
测试项目：十程序 ASan 历史压力批次
性质: MSVC ASan 编译/运行真实 Pause 八套件及 stdexec 两个依赖寿命套件.
项目: 覆盖 integration、sender、hook、resource、task lifetime、extreme、batch、transition,
以及 standard task lifetime、callback affine lifetime; 记录每项命令/退出码/哈希及总 manifest.
预期: 所有构建/运行退出 0, 无超时和 stderr, 批次结束源文件哈希不变.
运行: MSVC 开发环境 powershell -File 本脚本; -Iterations 默认 1000,
-TimeoutSeconds 默认 600, -Resume 从既有 results.json 跳过已有条目.
边界: 历史 Resume 不重新验证被跳过条目是否成功, 需另行核查; 输出保留原 build 审计路径.
#>
param([int]$Iterations = 1000, [int]$TimeoutSeconds = 600, [switch]$Resume)
$ErrorActionPreference = 'Stop'
$auditRoot = 'D:\workspace\pause'
$auditOutput = Join-Path $auditRoot 'build/pause-audit-20260925/asan'
Set-Location -LiteralPath $auditRoot
$auditInclude = Join-Path $auditRoot 'build/msvc-debug/_deps/stdexec-src/include'
$auditHooks = Join-Path $auditRoot 'build/msvc-debug/pause-instrumented-stdexec'
$auditTests = @(
    @{name='pause_integration_test'; count=20; extra=4},
    @{name='pause_sender_test'; count=43; extra=0},
    @{name='pause_sender_hook_test'; count=7; extra=0},
    @{name='pause_resource_test'; count=25; extra=0},
    @{name='pause_task_lifetime_test'; count=3; extra=0},
    @{name='pause_extreme_test'; count=25; extra=0; release=$true},
    @{name='pause_batch_extreme_test'; count=52; extra=0; release=$true},
    @{name='pause_transition_extreme_test'; count=11; extra=0},
    @{name='stdexec_standard_task_lifetime_test'; count=23; extra=0; dependency=$true},
    @{name='stdexec_callback_affine_lifetime_test'; count=4; extra=0; dependency=$true}
)
$auditResults = [System.Collections.Generic.List[object]]::new()
if ($Resume) {
    $auditPreviousResults = Get-Content -LiteralPath "$auditOutput/results.json" -Raw | ConvertFrom-Json
    foreach ($auditPreviousResult in $auditPreviousResults) { $auditResults.Add($auditPreviousResult) }
}
$auditCompiler = (Get-Command cl.exe).Source
$auditFiles = @('pause.hpp','pause-branch-design.md','tests/pause_sender_test_support.hpp',
    'tests/instrument_stop_token.cmake','tests/instrument_pause_stdexec.cmake',
    'build/msvc-debug/_deps/stdexec-src/include/stdexec/stop_token.hpp',
    'build/msvc-debug/_deps/stdexec-src/include/stdexec/__detail/__task.hpp',
    'build/msvc-debug/pause-instrumented-stdexec/stdexec/stop_token.hpp',
    'build/msvc-debug/pause-test-hooks/pause_test_stop_token.hpp')
$auditFiles += @($auditTests | ForEach-Object { "tests/$($_.name).cpp" })
$auditHashes = @{}
foreach ($auditFile in $auditFiles) {
    $auditHashes[$auditFile] = (Get-FileHash -LiteralPath $auditFile -Algorithm SHA256).Hash
}
$auditManifest = [ordered]@{
    started_utc=(Get-Date).ToUniversalTime().ToString('o'); compiler=$auditCompiler
    compiler_sha256=(Get-FileHash -LiteralPath $auditCompiler -Algorithm SHA256).Hash
    compiler_version=(Get-Item -LiteralPath $auditCompiler).VersionInfo.FileVersion
    dependency_commit=(& git -c safe.directory=D:/workspace/pause/build/msvc-debug/_deps/stdexec-src -C build/msvc-debug/_deps/stdexec-src rev-parse HEAD)
    iterations=$Iterations; timeout_seconds=$TimeoutSeconds; sources=$auditHashes
}
$auditManifestName = if ($Resume) { 'resume-manifest.json' } else { 'manifest.json' }
$auditManifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$auditOutput/$auditManifestName"
foreach ($auditTest in $auditTests) {
    $auditName = $auditTest.name
    if ($Resume -and $auditName -in $auditPreviousResults.name) { continue }
    $testInclude = if ($auditName -eq 'pause_sender_hook_test') { $auditHooks } else { $auditInclude }
    $auditArgs = @('/nologo','/std:c++latest','/Zc:__cplusplus','/Zc:preprocessor',
        '/EHsc','/MD','/O2','/Zi','/fsanitize=address',"/I$auditRoot","/I$testInclude",
        "/I$auditInclude/stdexec","/I$auditRoot/build/msvc-debug/pause-test-hooks",
        "/Fe$auditOutput/$auditName.exe","/Fo$auditOutput/$auditName.obj",
        "/Fd$auditOutput/$auditName.pdb","tests/$auditName.cpp")
    if ($auditTest.release) { $auditArgs += '/DNDEBUG' }
    $auditArgs += @('/link','/INCREMENTAL:NO')
    $auditBuildStarted = Get-Date
    $auditBuild = Start-Process -FilePath $auditCompiler -ArgumentList $auditArgs -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$auditOutput/$auditName.build.stdout.log" `
        -RedirectStandardError "$auditOutput/$auditName.build.stderr.log"
    $auditBuildHandle = $auditBuild.Handle
    $auditBuild.WaitForExit()
    $auditBuildResult = [ordered]@{
        command=@($auditCompiler)+$auditArgs; exit_code=$auditBuild.ExitCode
        elapsed_seconds=((Get-Date)-$auditBuildStarted).TotalSeconds
        source_sha256=$auditHashes["tests/$auditName.cpp"]; header_sha256=$auditHashes['pause.hpp']
        design_sha256=$auditHashes['pause-branch-design.md']; ndebug=[bool]$auditTest.release
    }
    $auditBuildResult | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$auditOutput/$auditName.build.result.json"
    if ($auditBuild.ExitCode -ne 0) { throw "Build failed: $auditName, exit=$($auditBuild.ExitCode)" }
    Write-Host "Built ASan $auditName; NDEBUG=$([bool]$auditTest.release)"
    $auditBuild.Dispose()
    $auditRunArgs = @('--iterations',"$Iterations")
    $auditExecutable = Join-Path $auditOutput "$auditName.exe"
    $auditRunStarted = Get-Date
    $auditProcess = Start-Process -FilePath $auditExecutable -ArgumentList $auditRunArgs -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$auditOutput/$auditName.stdout.log" `
        -RedirectStandardError "$auditOutput/$auditName.stderr.log"
    $auditHandle = $auditProcess.Handle
    $auditTimedOut = $false
    while (-not $auditProcess.WaitForExit(1000)) {
        if (((Get-Date)-$auditRunStarted).TotalSeconds -ge $TimeoutSeconds) {
            $auditTimedOut=$true; $auditProcess.Kill(); $auditProcess.WaitForExit(); break
        }
    }
    $auditResult = [ordered]@{
        name=$auditName; command=@($auditExecutable)+$auditRunArgs; exit_code=$auditProcess.ExitCode
        timed_out=$auditTimedOut; elapsed_seconds=((Get-Date)-$auditRunStarted).TotalSeconds
        stdout="$auditOutput/$auditName.stdout.log"; stderr="$auditOutput/$auditName.stderr.log"
        stderr_bytes=(Get-Item -LiteralPath "$auditOutput/$auditName.stderr.log").Length
        header_sha256=$auditHashes['pause.hpp']; design_sha256=$auditHashes['pause-branch-design.md']
        source_sha256=$auditHashes["tests/$auditName.cpp"]
        executable_sha256=(Get-FileHash -LiteralPath $auditExecutable -Algorithm SHA256).Hash
        scenario_families=$auditTest.count; repetitions=$Iterations; expected_scenarios=$auditTest.count*$Iterations+$auditTest.extra
        dependency_only=[bool]$auditTest.dependency; ndebug=[bool]$auditTest.release
    }
    $auditResults.Add([pscustomobject]$auditResult)
    $auditResult | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$auditOutput/$auditName.result.json"
    $auditResults | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$auditOutput/results.json"
    $auditProcess.Dispose()
    Get-Content -LiteralPath "$auditOutput/$auditName.stdout.log"
    if ($auditTimedOut -or $auditResult.exit_code -ne 0 -or $auditResult.stderr_bytes -ne 0) {
        throw "ASan test failed: $auditName; exit=$($auditResult.exit_code), timed_out=$auditTimedOut"
    }
    Write-Host "Passed ASan $auditName in $($auditResult.elapsed_seconds) seconds"
}
$auditFinalHashes = @{}
foreach ($auditFile in $auditFiles) {
    $auditFinalHashes[$auditFile] = (Get-FileHash -LiteralPath $auditFile -Algorithm SHA256).Hash
    if ($auditFinalHashes[$auditFile] -ne $auditHashes[$auditFile]) { throw "Source changed during campaign: $auditFile" }
}
[ordered]@{finished_utc=(Get-Date).ToUniversalTime().ToString('o');sources=$auditFinalHashes} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$auditOutput/final-source-verification.json"
