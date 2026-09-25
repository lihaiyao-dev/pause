<#
测试项目：契约 ASan 历史证据强校验汇总
性质: 读取十二程序主批次与三项补测, 不运行产品测试.
项目: 用最终 notification 证据替换旧版本, 合入 outer/host, 检查源码/二进制哈希,
编译含 fsanitize=address、stderr 为空、无超时以及每个运行期状态族 10000 轮.
预期: 13 个运行期程序合计 2380000 场景, 另有 6 个终止子进程; 不符立即抛错.
运行: powershell -File tests/archive/pause-contract-audit-20260925/asan/summarize-contract-asan.ps1.
边界: 源文件新增注释也会改变哈希, 故迁移后的当前源码不能直接冒充原历史证据; 不改写旧 manifest.
#>
$ErrorActionPreference='Stop'
$auditRoot='D:\workspace\pause'
$auditOutput=Join-Path $auditRoot 'build/pause-contract-audit-20260925/asan'
Set-Location -LiteralPath $auditRoot
$manifest=Get-Content -LiteralPath "$auditOutput/manifest.json" -Raw | ConvertFrom-Json
$main=Get-Content -LiteralPath "$auditOutput/results.json" -Raw | ConvertFrom-Json
if($main.Count -ne 12){throw "Main campaign incomplete: $($main.Count) of 12 test programs"}
$records=[System.Collections.Generic.List[object]]::new()
$additionalSources=@{}
foreach($row in $main){if($row.name -ne 'pause_notification_matrix_test'){$records.Add($row)}}
$oldNotification=$main | Where-Object name -eq 'pause_notification_matrix_test'
foreach($extra in @(
    @{name='pause_notification_matrix_test';dir='notification-final';families=6},
    @{name='pause_outer_lifetime_test';dir='outer-lifetime';families=6},
    @{name='pause_host_failure_test';dir='host-failure';families=2}
)){
    $name=$extra.name
    $directory=Join-Path $auditOutput $extra.dir
    $row=Get-Content -LiteralPath "$directory/$name.result.json" -Raw | ConvertFrom-Json
    foreach($property in $row.sources.PSObject.Properties){
        $actual=(Get-FileHash -LiteralPath $property.Name -Algorithm SHA256).Hash
        if($actual -ne $property.Value){throw "Supplemental source changed: $($property.Name)"}
        $additionalSources[$property.Name]=$actual
    }
    $records.Add([pscustomobject][ordered]@{
        name=$name;command=$row.command;exit_code=$row.exit_code;timed_out=$row.timed_out
        elapsed_seconds=$row.elapsed_seconds;stdout=$row.stdout;stderr=$row.stderr;stderr_bytes=$row.stderr_bytes
        header_sha256=$row.sources.'pause.hpp';design_sha256=$row.sources.'pause-branch-design.md'
        source_sha256=$row.sources.PSObject.Properties["tests/$name.cpp"].Value
        executable_sha256=$row.executable_sha256;scenario_families=$extra.families;repetitions=10000
        expected_scenarios=$extra.families*10000;termination_subprocesses=$false;ndebug=$false
    })
}
$verifiedSources=@{}
foreach($entry in $additionalSources.GetEnumerator()){$verifiedSources[$entry.Key]=$entry.Value}
foreach($property in $manifest.sources.PSObject.Properties){
    if($property.Name -eq 'tests/pause_notification_matrix_test.cpp'){continue}
    $actual=(Get-FileHash -LiteralPath $property.Name -Algorithm SHA256).Hash
    if($actual -ne $property.Value){throw "Initial source no longer matches: $($property.Name)"}
    $verifiedSources[$property.Name]=$actual
}
foreach($row in $records){
    if($row.exit_code -ne 0 -or $row.timed_out -or $row.stderr_bytes -ne 0){throw "Test not successful: $($row.name)"}
    if((Get-Item -LiteralPath $row.stderr).Length -ne 0){throw "Nonempty current stderr: $($row.name)"}
    if((Get-FileHash -LiteralPath $row.command[0] -Algorithm SHA256).Hash -ne $row.executable_sha256){throw "Binary changed: $($row.name)"}
    $source="tests/$($row.name).cpp"
    $actual=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    if($actual -ne $row.source_sha256){throw "Test source changed: $($row.name)"}
    $verifiedSources[$source]=$actual
    if($row.header_sha256 -ne $manifest.sources.'pause.hpp' -or $row.design_sha256 -ne $manifest.sources.'pause-branch-design.md'){
        throw "Header/design mismatch: $($row.name)"
    }
    $directory=Split-Path -Parent $row.command[0]
    $build=Get-Content -LiteralPath "$directory/$($row.name).build.result.json" -Raw | ConvertFrom-Json
    if($build.exit_code -ne 0 -or '/fsanitize=address' -notin $build.command){throw "ASan build not verified: $($row.name)"}
    if(-not $row.termination_subprocesses -and $row.repetitions -ne 10000){throw "Wrong repetition count: $($row.name)"}
}
$runtime=$records | Where-Object {-not $_.termination_subprocesses}
$death=$records | Where-Object termination_subprocesses
$scenarios=($runtime | Measure-Object -Property expected_scenarios -Sum).Sum
if($runtime.Count -ne 13 -or $scenarios -ne 2380000){throw "Unexpected runtime coverage: programs=$($runtime.Count), scenarios=$scenarios"}
if(($death | Measure-Object -Property expected_scenarios -Sum).Sum -ne 6){throw 'Unexpected termination smoke count'}
$summary=[ordered]@{
    verified_utc=(Get-Date).ToUniversalTime().ToString('o');compiler_version=$manifest.compiler_version
    dependency_commit=$manifest.dependency_commit;production_header_sha256=$manifest.sources.'pause.hpp'
    design_sha256=$manifest.sources.'pause-branch-design.md';runtime_programs=13;test_parent_processes=14
    real_pause_scenarios=$scenarios;termination_smoke_subprocesses=6;runtime_repetitions_per_family=10000
    all_builds_and_tests_exit_zero=$true;all_stderr_empty=$true;all_final_sources_and_binaries_match=$true
    timeouts=0;runtime_seconds_sum=($records | Measure-Object -Property elapsed_seconds -Sum).Sum
    ndebug_suites=@('pause_integration_test','pause_extreme_test','pause_batch_extreme_test')
    superseded_notification=[ordered]@{source_sha256=$oldNotification.source_sha256;excluded_scenarios=60000
        result="$auditOutput/pause_notification_matrix_test.result.json"
        reason='Host receipt-order assertions strengthened after original run; final source rerun for all 6 IDs x 10000'}
    main_final_hash_audit='Original main manifest retains old notification hash; final source verification uses the separately rebuilt final notification run.'
    sources=$verifiedSources;records=$records
}
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath "$auditOutput/audit-summary.json"
$records | Select-Object name,expected_scenarios,elapsed_seconds,exit_code | Format-Table -AutoSize
Write-Host "Verified: $scenarios real Pause scenarios + 6 termination subprocesses; all final source and binary hashes current."
