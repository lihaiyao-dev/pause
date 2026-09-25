<#
测试项目：MSVC Debug 迭代器代理分配终止压力
性质: 使用已构建 pause_resource_test, 先核对实际编译命令使用 MDd.
项目: 普通资源场景默认 1 轮; 三个 vector noexcept 代理分配点乘两种模式,
默认每类 1000 个预期终止子进程; 检查运行前后源码/设计/二进制 SHA-256.
预期: 未超过 7200 秒, 主测试退出 0 且 stderr 为空、哈希稳定; 超时终止整棵子进程树.
运行: powershell -File 本脚本; -Iterations、-ProxyIterations 均接受 1..1000000,
-Label 为 debug-proxy 或 debug-resource, 日志写回原 build 审计目录.
边界: 必须有 MSVC Debug compile_commands.json 与对应二进制, 不适用于非代理构建.
#>
param(
    [ValidateRange(1,1000000)][int]$Iterations = 1,
    [ValidateRange(1,1000000)][int]$ProxyIterations = 1000,
    [ValidateSet('debug-proxy','debug-resource')][string]$Label = 'debug-proxy'
)
$ErrorActionPreference = 'Stop'
$pauseRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../../..").Path
Set-Location -LiteralPath $pauseRoot
$output = Join-Path $pauseRoot "build/pause-contract-audit-20260925/$Label"
New-Item -ItemType Directory -Path $output -Force | Out-Null
$executable = Join-Path $pauseRoot 'build/msvc-debug/pause_resource_test.exe'
$paths = [ordered]@{
    header = Join-Path $pauseRoot 'pause.hpp'
    source = Join-Path $pauseRoot 'tests/pause_resource_test.cpp'
    design = Join-Path $pauseRoot 'pause-branch-design.md'
    binary = $executable
}
$before = [ordered]@{}
foreach ($entry in $paths.GetEnumerator()) { $before[$entry.Key] = (Get-FileHash -LiteralPath $entry.Value).Hash }
$commands = Get-Content build/msvc-debug/compile_commands.json -Raw | ConvertFrom-Json
$compile = @($commands | Where-Object { (Split-Path $_.file -Leaf) -eq 'pause_resource_test.cpp' })
if ($compile.Count -ne 1 -or $compile[0].command -notmatch '[-/]MDd\b') { throw 'Expected actual MSVC Debug CRT' }
$started = Get-Date
$stdout = Join-Path $output 'stdout.log'
$stderr = Join-Path $output 'stderr.log'
$arguments = @('--iterations',"$Iterations",'--proxy-iterations',"$ProxyIterations")
$process = Start-Process -FilePath $executable -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$handle = $process.Handle
$timedOut = $false
try {
    while (-not $process.WaitForExit(1000)) {
        if (((Get-Date) - $started).TotalSeconds -ge 7200) {
            $timedOut = $true
            & taskkill.exe /PID $process.Id /T /F | Out-Null
            if (-not $process.WaitForExit(10000)) { throw 'Timed-out proxy process tree did not exit' }
            break
        }
    }
    $exitCode = $process.ExitCode
} finally { $process.Dispose() }
$after = [ordered]@{}
$stable = $true
foreach ($entry in $paths.GetEnumerator()) {
    $after[$entry.Key] = (Get-FileHash -LiteralPath $entry.Value).Hash
    if ($after[$entry.Key] -ne $before[$entry.Key]) { $stable = $false }
}
$stderrBytes = (Get-Item -LiteralPath $stderr).Length
$result = [ordered]@{
    configuration='MSVC Debug, default iterator debug level'
    executable=$executable; arguments=$arguments; compile_command=$compile[0].command
    expected_termination_families=6; repetitions_per_family=$ProxyIterations; expected_termination_subprocesses=(6*$ProxyIterations)
    resource_scenarios=(25*$Iterations); exit_code=$exitCode; timed_out=$timedOut; stderr_bytes=$stderrBytes
    elapsed_seconds=((Get-Date)-$started).TotalSeconds; stdout=$stdout; stderr=$stderr
    hash_paths=$paths; sha256_before=$before; sha256_after=$after; hashes_stable=$stable
    passed=(-not $timedOut -and $exitCode -eq 0 -and $stderrBytes -eq 0 -and $stable)
}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$output/result.json" -Encoding UTF8
Get-Content -LiteralPath $stdout
if (-not $result.passed) { throw 'Debug proxy stress failed; see result.json' }
