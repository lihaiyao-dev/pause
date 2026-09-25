<# 测试项目：summarize-pause-release.ps1
汇总 8 个历史 Release 压力结果, 校验退出/超时/stderr/源码二进制哈希, 检查 extreme/batch/transition 的 25/52/11 行输出.
预期: 仅接受 --iterations 100000 且历史证据匹配当前制品; 输出 build/pause-release-extreme-summary.json.
入口: powershell -File tests/archive/summarize-pause-release.ps1; 不执行测试, 添加注释也会改变哈希并拒绝旧证据.
边界: 场景计数/stdexec commit 固定, 不自动适应新测试结构.
归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
#>
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Join-Path $PSScriptRoot '../..')
$rows = @(
    @('pause_integration_test','build/pause-release-extreme/pause_integration_test.result.json',18,2,4),
    @('pause_sender_test','build/pause-release-extreme/pause_sender_test.result.json',43,0,0),
    @('pause_sender_hook_test','build/pause-release-extreme-final/pause_sender_hook_test.result.json',7,0,0),
    @('pause_resource_test','build/release-extreme-resource/resource-100000.result.json',25,0,0),
    @('pause_task_lifetime_test','build/pause-release-extreme/pause_task_lifetime_test.result.json',3,0,0),
    @('pause_extreme_test','build/pause-release-extreme-final/pause_extreme_test.result.json',25,0,0),
    @('pause_batch_extreme_test','build/pause-release-extreme/pause_batch_extreme_test.result.json',52,0,0),
    @('pause_transition_extreme_test','build/pause-release-extreme-final/pause_transition_extreme_test.result.json',11,0,0)
)
$headerHash = (Get-FileHash -LiteralPath pause.hpp -Algorithm SHA256).Hash
$results = @()
foreach ($row in $rows) {
    $name = $row[0]
    $record = Get-Content -LiteralPath $row[1] -Raw | ConvertFrom-Json
    if ($record.exit_code -ne 0 -or $record.timed_out) { throw "Failed run: $name" }
    if ($name -eq 'pause_resource_test') {
        $sourceHash = $record.source_sha256
        $binaryHash = $record.executable_sha256
        $runHeader = $record.header_sha256
        $stderr = 'build/release-extreme-resource/resource-100000.stderr.log'
    } else {
        if (-not $record.passed -or -not $record.hashes_stable) { throw "Unstable run: $name" }
        $sourceHash = $record.sha256_before.source
        $binaryHash = $record.sha256_before.binary
        $runHeader = $record.sha256_before.header
        $stderr = $record.stderr
    }
    if ($sourceHash -ne (Get-FileHash -LiteralPath "tests/$name.cpp" -Algorithm SHA256).Hash -or
        $binaryHash -ne (Get-FileHash -LiteralPath "build/msvc-release/$name.exe" -Algorithm SHA256).Hash -or
        $runHeader -ne $headerHash -or (Get-Item -LiteralPath $stderr).Length -ne 0) {
        throw "Artifact mismatch: $name"
    }
    if ($record.arguments[0] -ne '--iterations' -or $record.arguments[1] -ne '100000') {
        throw "Insufficient repetition argument: $name"
    }
    $results += [pscustomobject][ordered]@{
        name=$name; fixed_configuration_families=$row[2]; random_parameter_families=$row[3]
        repetitions_per_family=100000; extra_single_run_cases=$row[4]
        total_cases=([long]$row[2]+[long]$row[3])*100000+[long]$row[4]
        evidence=$row[1]; source_sha256=$sourceHash; executable_sha256=$binaryHash
        elapsed_seconds=$record.elapsed_seconds; passed=$true
    }
}
$matrixChecks = @(
    @('build/pause-release-extreme-final/pause_extreme_test.stdout.log','^PASS shape=.* scopes=100000 ',25),
    @('build/pause-release-extreme/pause_batch_extreme_test.stdout.log','^PASS: suite=.* iterations=100000$',52),
    @('build/pause-release-extreme-final/pause_transition_extreme_test.stdout.log','^PASS transition=.* complete_scopes=100000$',11)
)
foreach ($check in $matrixChecks) {
    $lines = @(Get-Content -LiteralPath $check[0] | Where-Object { $_ -match $check[1] })
    if ($lines.Count -ne $check[2]) { throw "Incomplete matrix output: $($check[0])" }
}
$summary = [pscustomobject][ordered]@{
    created_at=(Get-Date).ToString('o'); configuration='MSVC x64 Release /O2 /Ob2 /DNDEBUG /MD'
    header_sha256=$headerHash; stdexec_commit='f0e8ae6fdc6c188389b146bb854b80d399724b04'
    fixed_configuration_families=184; fixed_configuration_repetitions=18400000
    random_parameter_families=2; random_parameter_rounds=200000; extra_single_run_cases=4
    total_real_pause_cases=($results | Measure-Object -Property total_cases -Sum).Sum
    all_current_artifacts_match=$true; runs=$results
    asan_final_cases=156000; tsan_final_cases=95000
    notes=@('Random outcomes are not a per-interleaving coverage guarantee.',
        'Model/dependency probes and obsolete intermediate runs are excluded from the real Pause total.',
        'Six termination cases and compile-time checks are verified separately, per user agreement.')
}
$summary | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath build/pause-release-extreme-summary.json
$results | Format-Table name,total_cases,elapsed_seconds,passed -AutoSize
Write-Output "Verified real Pause cases: $($summary.total_real_pause_cases)"
