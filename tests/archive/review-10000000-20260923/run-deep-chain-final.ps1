<# 测试项目：历史 stdexec 深链 ASan 有限深度最终复核
测试项: 默认 48 场景, 另对深度 32/64 x chain/fork x 六种结果各启动独立进程, 共 25 进程/72 场景。
预期: 每进程 60 秒内零退出且 stderr 为空; 保存逐场景及汇总 JSON, 任一失败使脚本返回 1。
入口: 同目录 run-deep-chain-final.cmd, 需先构建 build 中的 deep-chain-final.exe。
边界: 是不含 pause.hpp 的宿主依赖探针, 非当前压力入口; 固定参数仍受平台递归栈容量影响。
不应扩展为默认无界深度扫描, 常规压力使用已验证深度增加重复次数。
#>
$ErrorActionPreference = 'Stop'
$testRepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
Set-Location -LiteralPath $testRepositoryRoot
$artifactDirectory = [IO.Path]::GetFullPath('build/review-10000000-20260923')
$deepExecutable = Join-Path $artifactDirectory 'deep-chain-final.exe'
$cases = @([pscustomobject]@{name='default'; arguments=@(); scenarios=48})
foreach ($depth in @(32,64)) {
    foreach ($topology in @('chain','fork')) {
        foreach ($outcome in @('value','error','stopped','root-stop','error-fast','stopped-fast')) {
            $cases += [pscustomobject]@{name="d$depth-$topology-$outcome"; arguments=@('--depth',"$depth",'--topology',$topology,'--outcome',$outcome); scenarios=1}
        }
    }
}
$results = @()
foreach ($case in $cases) {
    $outputPath = Join-Path $artifactDirectory "deep-chain-final-$($case.name).stdout.log"
    $errorPath = Join-Path $artifactDirectory "deep-chain-final-$($case.name).stderr.log"
    $launch = @{ FilePath=$deepExecutable; WindowStyle='Hidden'; PassThru=$true; RedirectStandardOutput=$outputPath; RedirectStandardError=$errorPath }
    if ($case.arguments.Count -gt 0) { $launch.ArgumentList = $case.arguments }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process @launch
    $processHandle = $process.Handle
    $finished = $process.WaitForExit(60000)
    if (-not $finished) { $process.Kill(); $process.WaitForExit() }
    $process.Refresh()
    $timer.Stop()
    $result = [pscustomobject]@{ name=$case.name; arguments=$case.arguments; scenarios=$case.scenarios; exit_code=$process.ExitCode; timed_out=(-not $finished); elapsed_ms=$timer.ElapsedMilliseconds; stderr_length=(Get-Item -LiteralPath $errorPath).Length }
    $results += $result
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $artifactDirectory "deep-chain-final-$($case.name).result.json")
    Write-Output ($result | ConvertTo-Json -Compress)
}
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $artifactDirectory 'deep-chain-final-results.json')
$failures = @($results | Where-Object { $_.timed_out -or $_.exit_code -ne 0 -or $_.stderr_length -ne 0 })
Write-Output "processes=$($results.Count) scenarios=$(($results | Measure-Object -Property scenarios -Sum).Sum) failures=$($failures.Count)"
if ($failures.Count -gt 0) { exit 1 }
