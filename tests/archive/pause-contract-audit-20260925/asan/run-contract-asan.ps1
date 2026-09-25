<#
测试项目：契约十二程序 ASan 压力批次
性质: MSVC ASan 编译并运行十一套运行期测试及一个 sender death 终止冒烟测试.
项目: 覆盖异常、通知、资源、状态迁移、callback 钩子、sender、task 寿命、并发、批量、极端与集成;
普通/API/固定深度链次数统一, death 固定 1 轮; 保存编译/运行日志与输入/二进制哈希.
预期: 每项构建与运行退出 0, 未超时、stderr 为空, 整批源文件前后哈希不变.
运行: MSVC 开发环境 powershell -File 本脚本; -Iterations 默认 10000,
-TimeoutSeconds 默认 7200, -Resume 从历史 results.json 跳过已有名称.
边界: Resume 不重新验证历史条目, 总体验收须运行相应汇总器; 输出仍保留原 build 审计目录.
#>
param([int]$Iterations=10000, [int]$TimeoutSeconds=7200, [switch]$Resume)
$ErrorActionPreference = 'Stop'
$auditRoot = 'D:\workspace\pause'
$auditOutput = Join-Path $auditRoot 'build/pause-contract-audit-20260925/asan'
$auditInclude = Join-Path $auditRoot 'build/msvc-debug/_deps/stdexec-src/include'
$auditHooks = Join-Path $auditRoot 'build/msvc-debug/pause-instrumented-stdexec'
Set-Location -LiteralPath $auditRoot
$auditTests = @(
    @{name='pause_exception_matrix_test';count=27},
    @{name='pause_notification_matrix_test';count=6},
    @{name='pause_resource_test';count=25},
    @{name='pause_transition_extreme_test';count=11},
    @{name='pause_sender_hook_test';count=7},
    @{name='pause_sender_test';count=43},
    @{name='pause_task_lifetime_test';count=3},
    @{name='pause_concurrency_stress_test';count=7},
    @{name='pause_batch_extreme_test';count=52;release=$true},
    @{name='pause_extreme_test';count=25;release=$true},
    @{name='pause_integration_test';count=24;release=$true},
    @{name='pause_sender_death_test';count=6;death=$true}
)
$auditResults = [System.Collections.Generic.List[object]]::new()
if ($Resume) {
    $auditPrevious = Get-Content -LiteralPath "$auditOutput/results.json" -Raw | ConvertFrom-Json
    foreach ($row in $auditPrevious) { $auditResults.Add($row) }
}
$auditFiles = @('pause.hpp','pause-branch-design.md','tests/pause_sender_test_support.hpp',
    'tests/instrument_stop_token.cmake','tests/instrument_pause_stdexec.cmake',
    'build/msvc-debug/_deps/stdexec-src/include/stdexec/stop_token.hpp',
    'build/msvc-debug/_deps/stdexec-src/include/stdexec/__detail/__task.hpp',
    'build/msvc-debug/pause-instrumented-stdexec/stdexec/stop_token.hpp',
    'build/msvc-debug/pause-test-hooks/pause_test_stop_token.hpp')
$auditFiles += @($auditTests | ForEach-Object { "tests/$($_.name).cpp" })
$auditHashes = @{}
foreach ($file in $auditFiles) { $auditHashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }
$auditCompiler = (Get-Command cl.exe).Source
$auditManifest = [ordered]@{
    started_utc=(Get-Date).ToUniversalTime().ToString('o');pid=$PID;compiler=$auditCompiler
    compiler_version=(Get-Item -LiteralPath $auditCompiler).VersionInfo.FileVersion
    compiler_sha256=(Get-FileHash -LiteralPath $auditCompiler -Algorithm SHA256).Hash
    dependency_commit=(& git -c safe.directory=D:/workspace/pause/build/msvc-debug/_deps/stdexec-src -C build/msvc-debug/_deps/stdexec-src rev-parse HEAD)
    sources=$auditHashes;iterations=$Iterations;timeout_seconds=$TimeoutSeconds
    ASAN_OPTIONS=$env:ASAN_OPTIONS;ASAN_SAVE_DUMPS=$env:ASAN_SAVE_DUMPS
}
$manifestName=if($Resume){'resume-manifest.json'}else{'manifest.json'}
$auditManifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$auditOutput/$manifestName"
foreach ($test in $auditTests) {
    $name=$test.name
    if ($Resume -and $name -in $auditPrevious.name) { continue }
    $include=if($name -eq 'pause_sender_hook_test'){$auditHooks}else{$auditInclude}
    $buildArgs=@('/nologo','/std:c++latest','/Zc:__cplusplus','/Zc:preprocessor','/EHsc',
        '/MD','/O2','/Zi','/fsanitize=address',"/I$auditRoot","/I$include", "/I$auditInclude/stdexec",
        "/I$auditRoot/build/msvc-debug/pause-test-hooks","/Fe$auditOutput/$name.exe",
        "/Fo$auditOutput/$name.obj","/Fd$auditOutput/$name.pdb","tests/$name.cpp")
    if ($test.release) {$buildArgs+='/DNDEBUG'}
    $buildArgs+=@('/link','/INCREMENTAL:NO')
    $built=Get-Date
    $compiler=Start-Process -FilePath $auditCompiler -ArgumentList $buildArgs -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$auditOutput/$name.build.stdout.log" -RedirectStandardError "$auditOutput/$name.build.stderr.log"
    $compilerHandle=$compiler.Handle
    $compiler.WaitForExit()
    [ordered]@{command=@($auditCompiler)+$buildArgs;exit_code=$compiler.ExitCode;elapsed_seconds=((Get-Date)-$built).TotalSeconds
        source_sha256=$auditHashes["tests/$name.cpp"];header_sha256=$auditHashes['pause.hpp'];design_sha256=$auditHashes['pause-branch-design.md'];ndebug=[bool]$test.release} |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$auditOutput/$name.build.result.json"
    if($compiler.ExitCode -ne 0){throw "Build failed: $name; exit=$($compiler.ExitCode)"}
    $compiler.Dispose()
    Write-Host "Built ASan $name; NDEBUG=$([bool]$test.release)"
    $repetitions=if($test.death){1}else{$Iterations}
    $runArgs=@('--iterations',"$repetitions")
    if($name -eq 'pause_integration_test'){$runArgs+=@('--api-iterations',"$Iterations",'--deep-iterations',"$Iterations")}
    if($name -in @('pause_sender_test','pause_exception_matrix_test')){$runArgs+=@('--ordinary-iterations',"$Iterations")}
    $exe=Join-Path $auditOutput "$name.exe"
    $started=Get-Date
    $process=Start-Process -FilePath $exe -ArgumentList $runArgs -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$auditOutput/$name.stdout.log" -RedirectStandardError "$auditOutput/$name.stderr.log"
    $processHandle=$process.Handle
    $timedOut=$false
    while(-not $process.WaitForExit(1000)){
        if(((Get-Date)-$started).TotalSeconds -ge $TimeoutSeconds){$timedOut=$true;$process.Kill();$process.WaitForExit();break}
    }
    $result=[pscustomobject][ordered]@{
        name=$name;command=@($exe)+$runArgs;pid=$process.Id;exit_code=$process.ExitCode;timed_out=$timedOut
        elapsed_seconds=((Get-Date)-$started).TotalSeconds;stdout="$auditOutput/$name.stdout.log";stderr="$auditOutput/$name.stderr.log"
        stderr_bytes=(Get-Item -LiteralPath "$auditOutput/$name.stderr.log").Length
        header_sha256=$auditHashes['pause.hpp'];design_sha256=$auditHashes['pause-branch-design.md'];source_sha256=$auditHashes["tests/$name.cpp"]
        executable_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
        scenario_families=$test.count;repetitions=$repetitions;expected_scenarios=$test.count*$repetitions
        termination_subprocesses=[bool]$test.death;ndebug=[bool]$test.release
    }
    $auditResults.Add($result)
    $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$auditOutput/$name.result.json"
    $auditResults | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$auditOutput/results.json"
    $process.Dispose()
    Get-Content -LiteralPath "$auditOutput/$name.stdout.log"
    if($timedOut -or $result.exit_code -ne 0 -or $result.stderr_bytes -ne 0){throw "ASan test failed: $name; exit=$($result.exit_code), timed_out=$timedOut"}
    Write-Host "Passed ASan $name in $($result.elapsed_seconds) seconds"
}
$finalHashes=@{}
foreach($file in $auditFiles){
    $finalHashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    if($finalHashes[$file] -ne $auditHashes[$file]){throw "Source changed during campaign: $file"}
}
[ordered]@{finished_utc=(Get-Date).ToUniversalTime().ToString('o');sources=$finalHashes} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$auditOutput/final-source-verification.json"
