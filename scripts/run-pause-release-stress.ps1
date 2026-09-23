param(
    [string]$OutputDirectory = 'build/pause-release-extreme',
    [ValidateRange(1,1000000)][int]$Iterations = 100000,
    [ValidateRange(1,86400)][int]$TimeoutSeconds = 14400,
    [ValidateSet('pause_integration_test','pause_sender_test','pause_sender_hook_test',
        'pause_sender_death_test','pause_resource_test','pause_task_lifetime_test',
        'pause_extreme_test','pause_batch_extreme_test','pause_transition_extreme_test','pause_state_model_test',
        'stdexec_stop_callback_test','stdexec_stop_callback_hook_test',
        'stdexec_callback_affine_lifetime_test','stdexec_task_lifetime_test',
        'stdexec_standard_task_api_test','stdexec_standard_task_lifetime_test',
        'stdexec_standard_task_scheduler_test','stdexec_deep_chain_test')]
    [string]$OnlyTest
)

# Build first: scripts/msvc-cmake.cmd test release
# No sanitizer or extra assertion flags: execute the actual Release preset.
$ErrorActionPreference = 'Stop'
$pauseRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").Path
Set-Location -LiteralPath $pauseRoot
$pauseBuild = Join-Path $pauseRoot 'build/msvc-release'
$pauseCache = Get-Content -LiteralPath "$pauseBuild/CMakeCache.txt" -Raw
if ($pauseCache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$') {
    throw 'Expected the msvc-release CMake build with CMAKE_BUILD_TYPE=Release.'
}
$pauseCommands = Get-Content -LiteralPath "$pauseBuild/compile_commands.json" -Raw | ConvertFrom-Json
if ($pauseCache -notmatch '(?m)^STDEXEC_SOURCE_DIR:STATIC=(.+)\r?$') {
    throw 'Cannot locate the pinned stdexec source in the CMake cache.'
}
$pauseStdexec = $Matches[1].Trim()
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$pauseOutput = (Resolve-Path -LiteralPath $OutputDirectory).Path
$pauseTests = @('pause_integration_test','pause_sender_test','pause_sender_hook_test',
    'pause_resource_test','pause_task_lifetime_test','pause_extreme_test',
    'pause_batch_extreme_test','pause_transition_extreme_test','pause_sender_death_test','pause_state_model_test',
    'stdexec_stop_callback_test','stdexec_stop_callback_hook_test',
    'stdexec_callback_affine_lifetime_test','stdexec_task_lifetime_test',
    'stdexec_standard_task_api_test','stdexec_standard_task_lifetime_test',
    'stdexec_standard_task_scheduler_test','stdexec_deep_chain_test')
if ($OnlyTest) { $pauseTests = @($OnlyTest) }

function Get-PauseHashes($Paths) {
    $hashes = [ordered]@{}
    foreach ($entry in $Paths.GetEnumerator()) {
        $hashes[$entry.Key] = (Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256).Hash
    }
    return $hashes
}

foreach ($testName in $pauseTests) {
    $executable = Join-Path $pauseBuild "$testName.exe"
    $source = Join-Path $pauseRoot "tests/$testName.cpp"
    $compile = @($pauseCommands | Where-Object { (Split-Path $_.file -Leaf) -eq "$testName.cpp" })
    if ($compile.Count -ne 1 -or $compile[0].command -notmatch '/O2\b' -or
        $compile[0].command -notmatch '/DNDEBUG\b') {
        throw "Expected /O2 /DNDEBUG for $testName."
    }
    $paths = [ordered]@{
        header = Join-Path $pauseRoot 'pause.hpp'
        source = $source
        support = Join-Path $pauseRoot 'tests/pause_sender_test_support.hpp'
        binary = $executable
        pinned_stop_token = Join-Path $pauseStdexec 'include/stdexec/stop_token.hpp'
    }
    if ($testName -eq 'pause_sender_hook_test') {
        $paths.instrumented_stop_token = Join-Path $pauseBuild 'pause-instrumented-stdexec/stdexec/stop_token.hpp'
    }
    $inputs = @($source, $paths.pinned_stop_token)
    if ($paths.Contains('instrumented_stop_token')) { $inputs += $paths.instrumented_stop_token }
    if ($testName -notlike 'stdexec_*' -and $testName -ne 'pause_state_model_test') {
        $inputs += $paths.header
    }
    if ((Get-Content -LiteralPath $source -Raw) -match '#include "pause_sender_test_support.hpp"') {
        $inputs += $paths.support
    }
    foreach ($path in $inputs) {
        if ((Get-Item -LiteralPath $path).LastWriteTimeUtc -gt (Get-Item -LiteralPath $executable).LastWriteTimeUtc) {
            throw "Rebuild $testName before running: $path is newer than the executable."
        }
    }
    $arguments = switch ($testName) {
        'pause_sender_death_test' { @() }
        'stdexec_deep_chain_test' { @() }
        'pause_state_model_test' { @('--depth','10','--random-steps',"$Iterations",'--seeds','20','--max-records','32') }
        default { @('--iterations',"$Iterations") }
    }
    $stdout = Join-Path $pauseOutput "$testName.stdout.log"
    $stderr = Join-Path $pauseOutput "$testName.stderr.log"
    $before = Get-PauseHashes $paths
    $started = Get-Date
    $processArguments = @{
        FilePath = $executable; WindowStyle = 'Hidden'; PassThru = $true
        RedirectStandardOutput = $stdout; RedirectStandardError = $stderr
    }
    if ($arguments.Count -gt 0) { $processArguments.ArgumentList = $arguments }
    Write-Host "Running Release: $testName $arguments"
    $process = Start-Process @processArguments
    $handle = $process.Handle
    $timedOut = $false
    try {
        while (-not $process.WaitForExit(1000)) {
            if (((Get-Date) - $started).TotalSeconds -ge $TimeoutSeconds) {
                $timedOut = $true
                # Kill this test and its children (death tests), never unrelated processes.
                & taskkill.exe /PID $process.Id /T /F | Out-Null
                $killExit = $LASTEXITCODE
                if (-not $process.WaitForExit(10000)) {
                    throw "Could not stop timed-out test $testName (PID $($process.Id), taskkill exit $killExit)."
                }
                break
            }
        }
        $exitCode = $process.ExitCode
    } finally { $process.Dispose() }
    $after = Get-PauseHashes $paths
    $stable = $true
    foreach ($key in $before.Keys) { if ($before[$key] -ne $after[$key]) { $stable = $false } }
    $errorBytes = (Get-Item -LiteralPath $stderr).Length
    $passed = -not $timedOut -and $exitCode -eq 0 -and $errorBytes -eq 0 -and $stable
    $result = [pscustomobject][ordered]@{
        name = $testName; configuration = 'Release'; arguments = @($arguments)
        started_at = $started.ToString('o'); elapsed_seconds = ((Get-Date) - $started).TotalSeconds
        exit_code = $exitCode; timed_out = $timedOut; stderr_bytes = $errorBytes
        compile_command = $compile[0].command
        compile_database = Join-Path $pauseBuild 'compile_commands.json'
        stdout = $stdout; stderr = $stderr; hash_paths = $paths
        sha256_before = $before; sha256_after = $after; hashes_stable = $stable; passed = $passed
    }
    $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$pauseOutput/$testName.result.json"
    Get-Content -LiteralPath $stdout
    if (-not $passed) { throw "Release test failed: $testName; see $pauseOutput." }
    Write-Host "Passed Release: $testName ($($result.elapsed_seconds) seconds)"
}
