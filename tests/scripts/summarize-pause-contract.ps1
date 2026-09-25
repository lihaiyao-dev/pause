<#
测试项目：逐场景压力证据审计与汇总（不启动测试）。
读取 Release/ASan/TSan 的日志、退出码、重复数、源码/二进制/依赖 SHA256，
校验唯一场景与每类阈值，只选当前哈希匹配且完整成功的证据；模型/阳性对照另计。
-EvidenceDirectory 与 -CompletionDirectory 指定记录目录；汇总写后者 evidence-summary.json。
-RequireComplete 在缺少目标证据时失败。新增注释也会改变源码哈希，旧结果不能冒充新验证；
本次整理后的旧压力证据预期不再满足严格当前哈希检查，不应放宽规则。
该脚本会写汇总文件；保留旧汇总时先备份 evidence-summary.json，或使用含所需证据的独立目录副本。
CompletionDirectory 同时是补充证据输入根和汇总输出根，必须已存在；空目录会缺少这些证据。
#>
param(
    [string]$EvidenceDirectory = 'build/pause-contract-audit-20260925',
    [string]$CompletionDirectory = 'build/pause-contract-completion-20260925',
    [switch]$RequireComplete
)

# Read existing evidence only; never build or launch a test. Counts are complete
# scenario repetitions, not individual nodes, callbacks, or model transitions.
$ErrorActionPreference = 'Stop'
$pauseRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
Set-Location -LiteralPath $pauseRoot
$evidenceRoot = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$completionRoot = (Resolve-Path -LiteralPath $CompletionDirectory).Path
$hashCache = @{}
$candidates = [System.Collections.Generic.List[object]]::new()
$families = [ordered]@{
    pause_sender_test = 43; pause_integration_test = 24
    pause_sender_hook_test = 7; pause_task_lifetime_test = 3
    pause_batch_extreme_test = 52; pause_extreme_test = 25
    pause_concurrency_stress_test = 7; pause_resource_test = 25
    pause_transition_extreme_test = 11; pause_exception_matrix_test = 29
    pause_notification_matrix_test = 6; pause_outer_lifetime_test = 6
    pause_host_failure_test = 2
    pause_sender_death_test = 6; pause_contract_death_test = 5
}
$runtimeNames = @($families.Keys | Where-Object { $_ -notmatch '_death_test$' })

function Resolve-EvidencePath([string]$Path) {
    if ($Path -match '^/mnt/([a-z])/(.*)$') { return "$($Matches[1]):/$($Matches[2])" }
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $pauseRoot $Path
}

function Get-EvidenceHash([string]$Path) {
    $resolved = Resolve-EvidencePath $Path
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) { return $null }
    if (-not $hashCache.ContainsKey($resolved)) {
        $hashCache[$resolved] = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash
    }
    return $hashCache[$resolved]
}

function Assert-EvidenceHash([string]$Path, [string]$Expected, $Problems) {
    if ($Expected -notmatch '^[0-9a-fA-F]{64}$') { $Problems.Add("Missing/invalid SHA256: $Path"); return }
    $actual = Get-EvidenceHash $Path
    if (-not $actual) { $Problems.Add("Missing hash input: $Path") }
    elseif ($actual -ine $Expected) { $Problems.Add("Current SHA256 differs: $Path") }
}

function Test-Sha256List([string]$Path, $Problems, [string]$TestName = '', $Limits = $null) {
    if (-not (Test-Path -LiteralPath $Path)) { $Problems.Add("Missing hash list: $Path"); return }
    $count = 0
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ($line -notmatch '^([0-9a-fA-F]{64})\s+\*?(.+)$') { $Problems.Add("Malformed hash line: $Path"); continue }
        $expected = $Matches[1]; $inputPath = $Matches[2]
        # Some campaign manifests include every test translation unit. A change
        # to another executable's source does not invalidate this executable.
        if ($TestName -and $inputPath -match 'tests[/\\](pause_.+_test\.cpp)$' -and
            $Matches[1] -ne "$TestName.cpp") { continue }
        if ($null -ne $Limits -and $inputPath -match '\.(sh|ps1)$' -and (Get-EvidenceHash $inputPath) -ine $expected) {
            $Limits.Add("Historical runner changed; recorded executable/source evidence retained: $inputPath")
            continue
        }
        Assert-EvidenceHash $inputPath $expected $Problems
        $count++
    }
    if ($count -eq 0) { $Problems.Add("Empty hash list: $Path") }
}

function Assert-UniqueScenarioRows($Rows) {
    $keys = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($row in $Rows) {
        $key = $row.Value -replace '\s+(?:class|subprocesses|scopes|complete_scopes|iterations)=.*$', ''
        if (-not $keys.Add($key)) { throw "Duplicate scenario ID/parameters: $key" }
    }
}

function Get-ScenarioCounts([string]$Name, [string]$Log, [string]$Channel) {
    $repetitions = [System.Collections.Generic.List[long]]::new()
    $ordinary = [System.Collections.Generic.List[long]]::new()
    $scopes = 0L; $subprocesses = 0L; $observedFamilies = 0
    if ($Channel -eq 'debug-proxy') {
        $rows = [regex]::Matches($Log, '(?m)^PASS proxy-termination=\d+ mode=\S+ subprocesses=(\d+) expected_exit=77\r?$')
        Assert-UniqueScenarioRows $rows
        foreach ($row in $rows) { $repetitions.Add([long]$row.Groups[1].Value) }
        $observedFamilies = $rows.Count
        $subprocesses = ($repetitions | Measure-Object -Sum).Sum
        if ($Log -notmatch 'Pause resource rollback and no-allocation paths: 25 cases passed \(1 repetitions\)') {
            throw 'Missing completed proxy ordinary smoke summary'
        }
        $scopes = 25
    } elseif ($Name -match '_death_test$') {
        $rows = [regex]::Matches($Log, '(?m)^PASS (?:termination|diagnostic)=\S+ subprocesses=(\d+) expected_exit=77\r?$')
        Assert-UniqueScenarioRows $rows
        foreach ($row in $rows) { $repetitions.Add([long]$row.Groups[1].Value) }
        $observedFamilies = $rows.Count
        $subprocesses = ($repetitions | Measure-Object -Sum).Sum
    } elseif ($Name -eq 'pause_sender_test') {
        foreach ($kind in @('ordinary','threaded')) {
            $match = [regex]::Match($Log, "PASS: sender $kind (\d+) cases x (\d+) iterations")
            if (-not $match.Success) { throw "Missing sender $kind summary" }
            $count = [int]$match.Groups[1].Value; $repeat = [long]$match.Groups[2].Value
            $observedFamilies += $count; $scopes += $count * $repeat
            $repetitions.Add($repeat); if ($kind -eq 'ordinary') { $ordinary.Add($repeat) }
        }
        $total = [regex]::Match($Log, 'PASS: (\d+) real Pause sender/scheduler/lifetime cases(?: x (\d+) iterations)?')
        if (-not $total.Success) { throw 'Missing sender final summary' }
        $reported = [long]$total.Groups[1].Value
        if ($total.Groups[2].Success) { $reported *= [long]$total.Groups[2].Value }
        if ($reported -ne $scopes) { throw 'Sender final summary disagrees with family counts' }
    } elseif ($Name -eq 'pause_integration_test') {
        $match = [regex]::Match($Log, 'real Pause public integration: (\d+) cases passed \((\d+) threaded x (\d+), API x (\d+), (\d+) deep chains x (\d+)\)')
        if (-not $match.Success) { throw 'Missing integration total' }
        $scopes = [long]$match.Groups[1].Value
        $threaded = [int]$match.Groups[2].Value; $deep = [int]$match.Groups[5].Value
        $observedFamilies = $threaded + 1 + $deep
        $calculated = $threaded * [long]$match.Groups[3].Value + [long]$match.Groups[4].Value + $deep * [long]$match.Groups[6].Value
        if ($scopes -ne $calculated) { throw 'Integration total disagrees with repetitions' }
        foreach ($group in @(3,4,6)) { $repetitions.Add([long]$match.Groups[$group].Value) }
        $ordinary.Add([long]$match.Groups[4].Value)
        if ([regex]::Matches($Log, 'PASS integration=deep-chain depth=(32|64) mode=(continuous|single-step) complete_scopes=\d+').Count -ne 4) {
            throw 'Missing one of four integration deep-chain rows'
        }
    } elseif ($Name -in @('pause_batch_extreme_test','pause_extreme_test')) {
        $pattern = if ($Name -eq 'pause_batch_extreme_test') { '(?m)^PASS: suite=.* iterations=(\d+)\r?$' } else { '(?m)^PASS shape=.* scopes=(\d+) ' }
        $rows = [regex]::Matches($Log, $pattern)
        Assert-UniqueScenarioRows $rows
        foreach ($row in $rows) { $repetitions.Add([long]$row.Groups[1].Value) }
        $observedFamilies = $rows.Count; $scopes = ($repetitions | Measure-Object -Sum).Sum
    } elseif ($Name -in @('pause_exception_matrix_test','pause_notification_matrix_test','pause_outer_lifetime_test','pause_host_failure_test','pause_transition_extreme_test')) {
        $rows = [regex]::Matches($Log, '(?m)^PASS (?:id|case|transition)=\S+(?: class=(\S+))? complete_scopes=(\d+)')
        Assert-UniqueScenarioRows $rows
        foreach ($row in $rows) {
            $repeat = [long]$row.Groups[2].Value; $repetitions.Add($repeat)
            if ($row.Groups[1].Value -eq 'ordinary' -or $row.Value -match '^PASS case=outer\.') { $ordinary.Add($repeat) }
        }
        $observedFamilies = $rows.Count; $scopes = ($repetitions | Measure-Object -Sum).Sum
        $total = [regex]::Match($Log, '(?:real Pause (?:notification matrix|outer lifetime|directed transitions): |Pause host failure: )(\d+) complete scopes|real Pause exception matrix: ordinary=\d+ concurrent=\d+ complete_scopes=(\d+)')
        if (-not $total.Success -or [long]($total.Groups[1].Value + $total.Groups[2].Value) -ne $scopes) {
            throw 'Missing or inconsistent complete-scope summary'
        }
    } else {
        $pattern = switch ($Name) {
            'pause_sender_hook_test' { 'PASS: (\d+) instrumented real Pause callback/lifetime windows x (\d+) iterations' }
            'pause_task_lifetime_test' { 'PASS: (\d+) real Pause task lifetime/optional-host cancellation cases x (\d+) iterations' }
            'pause_resource_test' { 'Pause resource rollback and no-allocation paths: (\d+) cases passed \((\d+) repetitions\)' }
            'pause_concurrency_stress_test' { 'real Pause concurrency: (\d+) complete scopes, \d+ state families, (\d+) repetitions each' }
        }
        $match = [regex]::Match($Log, $pattern)
        if (-not $match.Success) { throw 'Missing complete-suite summary' }
        $repeat = [long]$match.Groups[2].Value; $repetitions.Add($repeat)
        if ($Name -in @('pause_resource_test','pause_concurrency_stress_test')) {
            $scopes = [long]$match.Groups[1].Value
            if ($repeat -le 0 -or $scopes % $repeat -ne 0) { throw 'Invalid scenario/repetition ratio' }
            $observedFamilies = $scopes / $repeat
        } else { $observedFamilies = [int]$match.Groups[1].Value; $scopes = $observedFamilies * $repeat }
    }
    $expectedFamilies = if ($Channel -eq 'debug-proxy') { 6 } else { $families[$Name] }
    if ($observedFamilies -ne $expectedFamilies) { throw "Incomplete family coverage: $observedFamilies / $expectedFamilies" }
    if ($repetitions.Count -eq 0 -or ($repetitions | Measure-Object -Minimum).Minimum -le 0) { throw 'No positive repetition count' }
    $minimum = if ($Channel -eq 'debug-proxy' -or $Name -match '_death_test$') {
        if ($Channel -eq 'asan') { 1 } else { 1000 }
    } elseif ($Channel -eq 'release') {
        if ($Name -in @('pause_resource_test','pause_transition_extreme_test')) { 1000000 } else { 100000 }
    } else { 10000 }
    $meetsTarget = ($repetitions | Measure-Object -Minimum).Minimum -ge $minimum
    if ($Channel -eq 'release' -and $ordinary.Count -gt 0) {
        $meetsTarget = $meetsTarget -and ($ordinary | Measure-Object -Minimum).Minimum -ge 1000000
    }
    return [pscustomobject]@{ complete_scopes = $scopes; termination_subprocesses = $subprocesses
        scenario_families = $observedFamilies; minimum_repetitions = ($repetitions | Measure-Object -Minimum).Minimum
        meets_stress_target = [bool]$meetsTarget }
}

function Add-JsonCandidate($File, [string]$Origin) {
    $row = Get-Content -LiteralPath $File.FullName -Raw | ConvertFrom-Json
    $name = $row.name; $channel = if ($File.FullName -match '[/\\]asan[/\\]') { 'asan' } else { 'release' }
    if ($File.FullName -match '[/\\]debug-proxy[/\\]result.json$') { $name = 'pause_resource_test'; $channel = 'debug-proxy' }
    if (-not $name -or -not $families.Contains($name)) { return }
    $problems = [System.Collections.Generic.List[string]]::new()
    $limits = [System.Collections.Generic.List[string]]::new()
    if ($null -eq $row.exit_code -or $row.exit_code -ne 0) { $problems.Add('Missing/nonzero exit code') }
    if ($row.timed_out -ne $false) { $problems.Add('Missing timeout result or timed out') }
    if ($row.PSObject.Properties['passed'] -and -not $row.passed) { $problems.Add('Runner marked failed') }
    if ($channel -eq 'release' -and ($row.compile_command -notmatch '/O2\b' -or $row.compile_command -notmatch '/DNDEBUG\b')) {
        $problems.Add('Missing Release /O2 /DNDEBUG compile command')
    }
    $stdout = if ($row.stdout) { $row.stdout } else { Join-Path $File.DirectoryName "$name.stdout.log" }
    $stderr = if ($row.stderr) { $row.stderr } else { Join-Path $File.DirectoryName "$name.stderr.log" }
    if (-not (Test-Path -LiteralPath $stderr)) { $problems.Add('Missing stderr') }
    elseif ((Get-Item -LiteralPath $stderr).Length -ne 0) { $problems.Add('Nonempty stderr') }
    if ($row.stderr_bytes -and $row.stderr_bytes -ne 0) { $problems.Add('Recorded nonempty stderr') }
    if ($row.hash_paths) {
        foreach ($property in $row.hash_paths.PSObject.Properties) {
            $key = $property.Name; $expected = $row.sha256_after.$key; $path = $property.Value
            if ($row.sha256_before.$key -ine $expected) { $problems.Add("Before/after hash differs: $key") }
            # This archived binary was retained before an unrelated CMake relink.
            if ($key -eq 'binary' -and $Origin -eq 'historical' -and $name -eq 'pause_sender_hook_test' -and
                (Get-EvidenceHash $path) -ine $expected) {
                $path = Join-Path $evidenceRoot 'release/binaries/pause_sender_hook_test.exe'
                $limits.Add('Verified archived hook binary; current build binary was relinked')
            }
            Assert-EvidenceHash $path $expected $problems
        }
    } else {
        if ($row.sources) {
            foreach ($property in $row.sources.PSObject.Properties) { Assert-EvidenceHash $property.Name $property.Value $problems }
        } else {
            Assert-EvidenceHash 'pause.hpp' $row.header_sha256 $problems
            Assert-EvidenceHash "tests/$name.cpp" $row.source_sha256 $problems
            if ($row.design_sha256) { Assert-EvidenceHash 'pause-branch-design.md' $row.design_sha256 $problems }
            else { $limits.Add('Result does not record the design document hash') }
        }
        $executable = if ($row.executable) { $row.executable } else { $row.command[0] }
        Assert-EvidenceHash $executable $row.executable_sha256 $problems
    }
    $inputList = Join-Path $File.DirectoryName 'inputs.sha256'
    if (Test-Path -LiteralPath $inputList) { Test-Sha256List $inputList $problems $name $limits }
    elseif ($channel -eq 'asan') { $limits.Add('No per-run complete input manifest; checked all hashes recorded in the result') }
    if ($channel -eq 'asan') {
        $manifestPath = Join-Path $File.DirectoryName 'manifest.json'
        if (Test-Path -LiteralPath $manifestPath) {
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
            foreach ($property in $manifest.sources.PSObject.Properties) {
                if ($property.Name -match 'tests[/\\](pause_.+_test\.cpp)$' -and $Matches[1] -ne "$name.cpp") { continue }
                Assert-EvidenceHash $property.Name $property.Value $problems
            }
        }
        $buildPath = Join-Path $File.DirectoryName "$name.build.result.json"
        if (Test-Path -LiteralPath $buildPath) {
            $build = Get-Content -LiteralPath $buildPath -Raw | ConvertFrom-Json
            if ($build.exit_code -ne 0 -or '/fsanitize=address' -notin $build.command) { $problems.Add('ASan build failed or instrumentation flag missing') }
        } else { $limits.Add('ASan build command/exit is not stored in a build result; directory/runner identifies the configuration') }
    }
    $counts = $null
    if (-not (Test-Path -LiteralPath $stdout)) { $problems.Add('Missing stdout') }
    else {
        $log = Get-Content -LiteralPath $stdout -Raw
        if ($log -match 'ERROR: AddressSanitizer|WARNING: ThreadSanitizer|FATAL: ThreadSanitizer|SUMMARY: ThreadSanitizer') { $problems.Add('Sanitizer report in stdout') }
        try { $counts = Get-ScenarioCounts $name $log $channel } catch { $problems.Add($_.Exception.Message) }
    }
    $candidates.Add([pscustomobject]@{ name=$name; channel=$channel; origin=$Origin; evidence=$File.FullName
        stdout=$stdout; timestamp=$File.LastWriteTimeUtc.ToString('o'); valid=($problems.Count -eq 0)
        problems=@($problems); provenance_limits=@($limits); counts=$counts })
}

function Add-TsanCandidate($LogFile, [string]$Origin) {
    $name = $LogFile.Name -replace '-run.log$', ''
    if (-not $families.Contains($name)) { return }
    $directory = $LogFile.DirectoryName
    $problems = [System.Collections.Generic.List[string]]::new()
    $limits = [System.Collections.Generic.List[string]]::new()
    $limits.Add('TSan combines stdout/stderr in its run log')
    foreach ($exitName in @("$name-exit.txt", 'driver-exit.txt')) {
        $path = Join-Path $directory $exitName
        if (-not (Test-Path -LiteralPath $path) -or [string](Get-Content -LiteralPath $path -Raw).Trim() -ne '0') { $problems.Add("Missing/nonzero $exitName") }
    }
    $newFormat = Test-Path -LiteralPath (Join-Path $directory 'inputs.sha256')
    $inputList = Join-Path $directory $(if ($newFormat) { 'inputs.sha256' } else { 'source-sha256-before.txt' })
    $binaryList = Join-Path $directory $(if ($newFormat) { 'binaries.sha256' } else { 'binary-sha256.txt' })
    Test-Sha256List $inputList $problems $name $limits
    Test-Sha256List $binaryList $problems
    $afterPath = Join-Path $directory $(if ($newFormat) { 'input-check.log' } else { 'source-sha256-after-check.txt' })
    if (-not (Test-Path -LiteralPath $afterPath)) { $problems.Add('Missing after-run hash check') }
    else {
        $afterLines = @([IO.File]::ReadAllLines($afterPath))
        if ($afterLines.Count -eq 0 -or @($afterLines | Where-Object { $_ -notmatch ': OK$' }).Count -ne 0) { $problems.Add('After-run hash check incomplete/failed') }
    }
    if (-not $newFormat) {
        $hashExit = Join-Path $directory 'hash-exit.txt'
        if (-not (Test-Path -LiteralPath $hashExit) -or (Get-Content -LiteralPath $hashExit -Raw).Trim() -ne '0') { $problems.Add('Missing/nonzero hash check exit') }
    }
    $log = Get-Content -LiteralPath $LogFile.FullName -Raw
    if ($log -match 'WARNING: ThreadSanitizer|FATAL: ThreadSanitizer|SUMMARY: ThreadSanitizer') { $problems.Add('ThreadSanitizer report') }
    $driverPath = Join-Path $directory 'driver.log'
    if (-not (Test-Path -LiteralPath $driverPath)) { $problems.Add('Missing driver log') }
    elseif ((Get-Content -LiteralPath $driverPath -Raw) -notmatch '-fsanitize=thread') {
        $limits.Add('Compiler command was not echoed; TSan configuration identified by recorded runner inputs and driver log')
    }
    foreach ($problem in $dependencyProblems) { $problems.Add($problem) }
    $counts = $null
    try { $counts = Get-ScenarioCounts $name $log 'tsan' } catch { $problems.Add($_.Exception.Message) }
    $candidates.Add([pscustomobject]@{ name=$name; channel='tsan'; origin=$Origin; evidence=$LogFile.FullName
        stdout=$LogFile.FullName; timestamp=$LogFile.LastWriteTimeUtc.ToString('o'); valid=($problems.Count -eq 0)
        problems=@($problems); provenance_limits=@($limits); counts=$counts })
}

$dependencyProblems = [System.Collections.Generic.List[string]]::new()
Test-Sha256List (Join-Path $evidenceRoot 'tsan/stdexec-include-sha256.txt') $dependencyProblems
foreach ($campaign in @(@{root=$evidenceRoot;origin='historical'}, @{root=$completionRoot;origin='completion'})) {
    foreach ($file in Get-ChildItem -LiteralPath $campaign.root -Recurse -File -Filter '*.result.json') {
        if ($file.Name -notlike '*.build.result.json') { Add-JsonCandidate $file $campaign.origin }
    }
    $proxy = Join-Path $campaign.root 'debug-proxy/result.json'
    if (Test-Path -LiteralPath $proxy) { Add-JsonCandidate (Get-Item -LiteralPath $proxy) $campaign.origin }
    $tsan = Join-Path $campaign.root 'tsan'
    if (Test-Path -LiteralPath $tsan) {
        foreach ($file in Get-ChildItem -LiteralPath $tsan -Recurse -File -Filter '*-run.log') { Add-TsanCandidate $file $campaign.origin }
    }
}

$selected = [System.Collections.Generic.List[object]]::new()
$missing = [System.Collections.Generic.List[string]]::new()
foreach ($channel in @('release','asan','tsan','debug-proxy')) {
    $names = if ($channel -eq 'debug-proxy') { @('pause_resource_test') } elseif ($channel -eq 'tsan') { $runtimeNames } else { @($families.Keys) }
    foreach ($name in $names) {
        $matching = @($candidates | Where-Object { $_.name -eq $name -and $_.channel -eq $channel -and $_.valid -and $_.counts.meets_stress_target } | Sort-Object timestamp -Descending)
        if ($matching.Count -eq 0) { $missing.Add("$channel/$name") }
        else { $selected.Add($matching[0]) }
    }
}
$totals = foreach ($channel in @('release','asan','tsan','debug-proxy')) {
    $rows = @($selected | Where-Object channel -eq $channel)
    [pscustomobject]@{ channel=$channel; selected_programs=$rows.Count
        complete_scopes=[long]($rows.counts | Measure-Object complete_scopes -Sum).Sum
        termination_subprocesses=[long]($rows.counts | Measure-Object termination_subprocesses -Sum).Sum
        reused_programs=@($rows | Where-Object origin -eq 'historical').Count
        completion_programs=@($rows | Where-Object origin -eq 'completion').Count }
}
$modelPath = Join-Path $completionRoot 'model/pause_state_model_test.result.json'
$model = [ordered]@{ evidence=$modelPath; valid=$false; problems=@('Missing completed model result') }
if (Test-Path -LiteralPath $modelPath) {
    $row = Get-Content -LiteralPath $modelPath -Raw | ConvertFrom-Json
    $problems = [System.Collections.Generic.List[string]]::new()
    if ($row.exit_code -ne 0 -or $row.timed_out -ne $false -or $row.passed -ne $true) { $problems.Add('Model runner did not complete successfully') }
    foreach ($property in $row.hash_paths.PSObject.Properties) {
        $key = $property.Name
        if ($row.sha256_before.$key -ine $row.sha256_after.$key) { $problems.Add("Model input changed during run: $key") }
        Assert-EvidenceHash $property.Value $row.sha256_after.$key $problems
    }
    if (-not $row.hash_paths) { $problems.Add('Missing model input hashes') }
    if (-not (Test-Path -LiteralPath $row.stderr) -or (Get-Item -LiteralPath $row.stderr).Length -ne 0) { $problems.Add('Model stderr missing/nonempty') }
    $log = if (Test-Path -LiteralPath $row.stdout) { Get-Content -LiteralPath $row.stdout -Raw } else { '' }
    $random = [regex]::Match($log, 'random_model: seed_base=\d+ seeds=(\d+) steps_per_seed=(\d+) transitions=(\d+)')
    if (-not $random.Success -or $log -notmatch 'PASS \(design model only; not production concurrency/lifetime proof\)') { $problems.Add('Missing final model summary') }
    $model = [ordered]@{ evidence=$modelPath; valid=($problems.Count -eq 0); problems=@($problems)
        random_seeds=[long]$random.Groups[1].Value; steps_per_seed=[long]$random.Groups[2].Value
        random_transitions=[long]$random.Groups[3].Value; included_in_runtime_scopes=$false }
}
$positivePath = Join-Path $completionRoot 'tsan-positive/result.json'
$positive = [ordered]@{ evidence=$positivePath; valid=$false; problems=@('Missing TSan positive control result') }
if (Test-Path -LiteralPath $positivePath) {
    $row = Get-Content -LiteralPath $positivePath -Raw | ConvertFrom-Json
    $problems = [System.Collections.Generic.List[string]]::new()
    Assert-EvidenceHash $row.source $row.source_sha256 $problems
    Assert-EvidenceHash $row.executable $row.executable_sha256 $problems
    if ($row.compile_exit_code -ne 0 -or $row.run_exit_code -ne 66 -or $row.timed_out -ne $false) { $problems.Add('Positive control did not exit as expected') }
    $positiveLog = Join-Path (Split-Path -Parent $positivePath) 'run.log'
    if (-not (Test-Path -LiteralPath $positiveLog) -or
        (Get-Content -LiteralPath $positiveLog -Raw) -notmatch 'WARNING: ThreadSanitizer: data race') { $problems.Add('Missing positive control data-race report') }
    $positive = [ordered]@{ evidence=$positivePath; valid=($problems.Count -eq 0); problems=@($problems)
        expected_detection_exit=66; included_in_runtime_scopes=$false }
}
$ctestPath = Join-Path $completionRoot 'final-ctest.log'
$ctest = [ordered]@{ evidence=$ctestPath; completed_configurations=0; included_in_runtime_scopes=$false
    limitation='Log summaries only; CTest does not record per-test input hashes in this artifact.' }
if (Test-Path -LiteralPath $ctestPath) {
    $ctestLog = Get-Content -LiteralPath $ctestPath -Raw
    $ctest.completed_configurations = [regex]::Matches($ctestLog, '100% tests passed, 0 tests failed out of 56').Count
}
$summary = [ordered]@{
    schema_version=1; audited_utc=(Get-Date).ToUniversalTime().ToString('o')
    historical_directory=$evidenceRoot; completion_directory=$completionRoot
    production_header_sha256=(Get-EvidenceHash 'pause.hpp'); design_sha256=(Get-EvidenceHash 'pause-branch-design.md')
    all_scenario_targets_satisfied=($missing.Count -eq 0); missing_targets=@($missing)
    counting_rule='Only latest current-hash, complete, successful candidate meeting the per-family stress threshold per suite/configuration; scopes and expected termination subprocesses counted separately.'
    limits=@('Historical results are revalidated evidence, not tests executed by this summarizer.',
        'Finite tested schedules and toolchains do not establish coverage of every possible interleaving.',
        'Model, CTest, dependency-depth probes and sanitizer positive controls are separate evidence and excluded from scenario totals.')
    dependency_hash_problems=@($dependencyProblems); model=$model; tsan_positive_control=$positive; ctest=$ctest
    totals=@($totals); selected=@($selected)
    rejected_or_superseded=@($candidates | Where-Object { $_.evidence -notin $selected.evidence })
}
$outputPath = Join-Path $completionRoot 'evidence-summary.json'
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $outputPath -Encoding UTF8
$totals | Format-Table -AutoSize
Write-Host "Missing scenario targets: $($missing.Count). Evidence: $outputPath"
if ($RequireComplete -and $missing.Count -ne 0) { throw 'Scenario evidence is incomplete; inspect missing_targets and rejected_or_superseded.' }
