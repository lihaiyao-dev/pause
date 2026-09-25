<#
测试项目：MSVC AddressSanitizer 构建与逐进程压力验收。
编译当前 tests 中的 Pause 运行时、异常、契约终止、生命周期、模型和依赖探针；
使用 /O2 /fsanitize=address，按测试需要启用 NDEBUG，不能等同普通 Release 构建。
先用 scripts/msvc-cmake.cmd test 生成固定依赖/测试 hook，再在 x64 VS 开发环境执行。
-OnlyTest 选择单项；-Iterations 为完整重复次数，深层用例用 -DeepIterations 重复固定深度；
模型用 -ModelSteps/-ModelSeeds，死亡测试用 -TerminationIterations。
各子进程须退出 0 且无 ASan 错误；超时终止该测试树，日志/哈希/结果写 -OutputDirectory。
本脚本不扫描栈极限；小量冒烟可用 -OnlyTest pause_sender_test -Iterations 2。
#>
param(
    [string]$OutputDirectory = 'build/pause-acceptance-asan',
    [ValidateRange(1,1000000)][int]$Iterations = 10000,
    [ValidateRange(1,1000000)][int]$DeepIterations = 1,
    [ValidateRange(1,1000000)][int]$TerminationIterations = 1,
    [ValidateRange(1,1000000)][int]$ModelSteps = 100000,
    [ValidateRange(1,10000)][int]$ModelSeeds = 100,
    [ValidateRange(1,7200)][int]$TimeoutSeconds = 1800,
    [ValidateSet('pause_integration_test','pause_sender_test','pause_sender_hook_test',
        'pause_sender_death_test','pause_contract_death_test','pause_resource_test','pause_task_lifetime_test','pause_concurrency_stress_test',
        'pause_exception_matrix_test','pause_notification_matrix_test',
        'pause_outer_lifetime_test','pause_host_failure_test',
        'pause_extreme_test','pause_batch_extreme_test','pause_transition_extreme_test',
        'pause_state_model_test','stdexec_stop_callback_test','stdexec_stop_callback_hook_test',
        'stdexec_callback_affine_lifetime_test','stdexec_task_lifetime_test',
        'stdexec_standard_task_api_test','stdexec_standard_task_lifetime_test',
        'stdexec_deep_chain_test','stdexec_standard_task_scheduler_test')]
    [string]$OnlyTest
)

# Run from the repository root in an x64 Visual Studio developer environment,
# after scripts/msvc-cmake.cmd test has generated the pinned test-only headers.
$ErrorActionPreference = 'Stop'
$pauseRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
Set-Location -LiteralPath $pauseRoot
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'Run this script in an x64 Visual Studio developer environment.'
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$pauseOutput = (Resolve-Path -LiteralPath $OutputDirectory).Path
$pauseInclude = Join-Path $pauseRoot 'build/msvc-debug/_deps/stdexec-src/include'
$pauseHooks = Join-Path $pauseRoot 'build/msvc-debug/pause-instrumented-stdexec'
if (-not (Test-Path -LiteralPath "$pauseHooks/stdexec/stop_token.hpp")) {
    throw 'First run scripts/msvc-cmake.cmd test to generate the test-only stdexec tree.'
}

$pauseTests = @(
    'pause_integration_test', 'pause_sender_test', 'pause_sender_hook_test',
    'pause_sender_death_test', 'pause_contract_death_test', 'pause_resource_test', 'pause_task_lifetime_test',
    'pause_concurrency_stress_test', 'pause_state_model_test',
    'pause_exception_matrix_test', 'pause_notification_matrix_test',
    'pause_outer_lifetime_test', 'pause_host_failure_test',
    'pause_extreme_test', 'pause_batch_extreme_test', 'pause_transition_extreme_test',
    'stdexec_stop_callback_test', 'stdexec_stop_callback_hook_test',
    'stdexec_callback_affine_lifetime_test', 'stdexec_task_lifetime_test',
    'stdexec_standard_task_api_test', 'stdexec_standard_task_lifetime_test',
    'stdexec_deep_chain_test', 'stdexec_standard_task_scheduler_test'
)
if ($OnlyTest) { $pauseTests = @($OnlyTest) }
$pauseResults = [System.Collections.Generic.List[object]]::new()
$pauseBuildHashes = @{}
foreach ($testName in $pauseTests) {
    $testInclude = if ($testName -eq 'pause_sender_hook_test') { $pauseHooks } else { $pauseInclude }
    $buildArgs = @('/nologo', '/std:c++latest', '/Zc:__cplusplus', '/Zc:preprocessor',
        '/EHsc', '/MD', '/O2', '/Zi', '/fsanitize=address', "/I$pauseRoot", "/I$testInclude",
        "/I$pauseInclude/stdexec", "/I$pauseRoot/build/msvc-debug/pause-test-hooks",
        "/Fe$pauseOutput/$testName.exe", "/Fo$pauseOutput/$testName.obj",
        "/Fd$pauseOutput/$testName.pdb", "tests/$testName.cpp", '/link', '/INCREMENTAL:NO')
    if ($testName -in @('pause_integration_test','pause_extreme_test','pause_batch_extreme_test','pause_contract_death_test')) {
        $buildArgs = @('/DNDEBUG') + $buildArgs
    }
    & cl.exe @buildArgs > "$pauseOutput/$testName.build.log" 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $testName; see its build log." }
    $pauseBuildHashes[$testName] = @{
        header = (Get-FileHash -LiteralPath 'pause.hpp' -Algorithm SHA256).Hash
        source = (Get-FileHash -LiteralPath "tests/$testName.cpp" -Algorithm SHA256).Hash
    }
    Write-Host "Built ASan: $testName"
}

function Invoke-PauseTest([string]$Name, [string[]]$Arguments, [string]$Label) {
    $testExecutable = Join-Path $pauseOutput "$Name.exe"
    $testOutput = Join-Path $pauseOutput "$Label.stdout.log"
    $testError = Join-Path $pauseOutput "$Label.stderr.log"
    $testStarted = Get-Date
    $processArgs = @{
        FilePath = $testExecutable; WindowStyle = 'Hidden'; PassThru = $true
        RedirectStandardOutput = $testOutput; RedirectStandardError = $testError
    }
    if ($Arguments.Count -gt 0) { $processArgs.ArgumentList = $Arguments }
    $testProcess = Start-Process @processArgs
    $timedOut = $false
    try {
        $testHandle = $testProcess.Handle
        while (-not $testProcess.WaitForExit(1000)) {
            if (((Get-Date) - $testStarted).TotalSeconds -ge $TimeoutSeconds) {
                $timedOut = $true
                # Kill this test and its children (death tests), never unrelated processes.
                & taskkill.exe /PID $testProcess.Id /T /F | Out-Null
                $killExit = $LASTEXITCODE
                if (-not $testProcess.WaitForExit(10000)) {
                    throw "Could not stop timed-out test $Label (PID $($testProcess.Id), taskkill exit $killExit)."
                }
                break
            }
        }
        $exitCode = $testProcess.ExitCode
    } finally { $testProcess.Dispose() }
    $result = [pscustomobject][ordered]@{
        name = $Label; executable = $testExecutable; arguments = $Arguments
        exit_code = $exitCode; timed_out = $timedOut
        elapsed_seconds = ((Get-Date) - $testStarted).TotalSeconds
        header_sha256 = $pauseBuildHashes[$Name].header
        source_sha256 = $pauseBuildHashes[$Name].source
        executable_sha256 = (Get-FileHash -LiteralPath $testExecutable -Algorithm SHA256).Hash
    }
    $pauseResults.Add($result)
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$pauseOutput/$Label.result.json"
    $pauseResults | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$pauseOutput/results.json"
    Get-Content -LiteralPath $testOutput
    if ($timedOut -or $exitCode -ne 0 -or (Get-Item -LiteralPath $testError).Length -ne 0) {
        throw "Test failed: $Label (exit=$exitCode timeout=$timedOut); inspect stdout/stderr logs."
    }
    Write-Host "Passed ASan: $Label"
}

foreach ($testName in $pauseTests) {
    $testArgs = switch ($testName) {
        'pause_state_model_test' { @('--depth','10','--random-steps',"$ModelSteps",'--seeds',"$ModelSeeds",'--max-records','24') }
        { $_ -in @('pause_sender_death_test','pause_contract_death_test') } { @('--iterations',"$TerminationIterations") }
        'pause_integration_test' { @('--iterations',"$Iterations",'--deep-iterations',"$DeepIterations") }
        'stdexec_deep_chain_test' { @() }
        default { @('--iterations',"$Iterations") }
    }
    Invoke-PauseTest $testName $testArgs $testName
}
if ($OnlyTest) { return }
Invoke-PauseTest 'pause_state_model_test' @('--chain-depth','64','--chain-only','1') 'pause_state_model_chain64'
foreach ($depth in @(32,64)) {
    foreach ($topology in @('chain','fork')) {
        foreach ($outcome in @('value','error','stopped','root-stop','error-fast','stopped-fast')) {
            Invoke-PauseTest 'stdexec_deep_chain_test' @('--depth',"$depth",'--topology',$topology,'--outcome',$outcome) "stdexec_chain-$depth-$topology-$outcome"
        }
    }
}
