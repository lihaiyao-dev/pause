<# 测试项目：历史 ASan 已知深链失败边界细化
测试项: 读取 task-asan-boundaries.json, 跳过无失败边界项, 对其拓扑/结果限定已有失败深度重新调查。
预期: run-chain-stress 保存成功与崩溃证据, 捕获调查异常并另写 campaign-error.log, 不改记为通过。
入口: 同目录 refine-asan.cmd; 需已有 boundaries JSON 与 task-asan.exe, 调用 tests/scripts/run-chain-stress.ps1。
边界: -InvestigateCrashes 可能重现栈溢出; 本文件仅历史取证, 不是当前压力入口, 本次未运行。
异常被本脚本捕获, 因而脚本正常返回不等于子进程全通过; 输出须按记录逐项审阅。
#>
$ErrorActionPreference = 'Stop'
$testRepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
Set-Location -LiteralPath $testRepositoryRoot
$refineArtifactRoot = Join-Path $testRepositoryRoot 'build/chain-audit-20260923'
$refineCases = Get-Content -Raw -LiteralPath (Join-Path $refineArtifactRoot 'task-asan-boundaries.json') | ConvertFrom-Json
foreach ($refineCase in $refineCases) {
    if ($refineCase.observed_failure_depth -eq 0) { continue }
    $refineLabel = 'refine-asan-' + $refineCase.topology + '-' + $refineCase.outcome
    try {
        & .\tests\scripts\run-chain-stress.ps1 -Executable (Join-Path $refineArtifactRoot 'task-asan.exe') -Label $refineLabel -OnlyTopology $refineCase.topology -OnlyOutcome $refineCase.outcome -MaxDepth $refineCase.observed_failure_depth -InvestigateCrashes
    } catch {
        # The purpose of this run is to locate and repeat the known crashes.
        # Preserve the failure; don't relabel it as a passing sanitizer test.
        $_ | Out-String | Set-Content -LiteralPath (Join-Path $refineArtifactRoot "$refineLabel.campaign-error.log")
        Write-Host "$refineLabel : crash investigation recorded"
    }
}
