<#
测试项目：外层 operation 生命周期 ASan 单项补测
性质: 重建并执行 pause_outer_lifetime_test 的六类生命周期场景.
项目: 通过指定重复次数观察完成尾部与拥有者销毁边界, 保存源码/二进制哈希和独立日志.
预期: 编译/运行退出 0, 未超时、stderr 为空, 所有输入哈希稳定.
运行: MSVC 开发环境 powershell -File 本脚本 -OnlyTest pause_outer_lifetime_test;
-Iterations 默认 1000, -TimeoutSeconds 默认 600, 历史契约汇总使用 10000 轮证据.
边界: 仅本项 ASan 检查, 输出保持 build 对应 outer-lifetime 目录.
#>
param(
    [ValidateSet('pause_outer_lifetime_test')][string]$OnlyTest,
    [int]$Iterations = 1000,
    [int]$TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'
$auditRoot = 'D:\workspace\pause'
Set-Location -LiteralPath $auditRoot
$auditOutput = Join-Path $auditRoot 'build/pause-contract-audit-20260925/asan/outer-lifetime'
$auditInclude = Join-Path $auditRoot 'build/msvc-debug/_deps/stdexec-src/include'
$auditCompiler = (Get-Command cl.exe).Source
$auditSources = @{}
foreach ($auditFile in @('pause.hpp','pause-branch-design.md','tests/pause_sender_test_support.hpp',"tests/$OnlyTest.cpp")) {
    $auditSources[$auditFile] = (Get-FileHash -LiteralPath $auditFile -Algorithm SHA256).Hash
}
$auditArgs = @('/nologo','/std:c++latest','/Zc:__cplusplus','/Zc:preprocessor',
    '/EHsc','/MD','/O2','/Zi','/fsanitize=address',"/I$auditRoot","/I$auditInclude",
    "/I$auditInclude/stdexec","/I$auditRoot/build/msvc-debug/pause-test-hooks",
    "/Fe$auditOutput/$OnlyTest.exe","/Fo$auditOutput/$OnlyTest.obj",
    "/Fd$auditOutput/$OnlyTest.pdb","tests/$OnlyTest.cpp",'/link','/INCREMENTAL:NO')
$auditBuildStarted = Get-Date
$auditBuild = Start-Process -FilePath $auditCompiler -ArgumentList $auditArgs -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput "$auditOutput/$OnlyTest.build.stdout.log" `
    -RedirectStandardError "$auditOutput/$OnlyTest.build.stderr.log"
$auditBuildHandle = $auditBuild.Handle
$auditBuild.WaitForExit()
[ordered]@{
    started_utc=$auditBuildStarted.ToUniversalTime().ToString('o')
    command=@($auditCompiler)+$auditArgs; exit_code=$auditBuild.ExitCode
    elapsed_seconds=((Get-Date)-$auditBuildStarted).TotalSeconds; sources=$auditSources
    compiler_sha256=(Get-FileHash -LiteralPath $auditCompiler -Algorithm SHA256).Hash
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$auditOutput/$OnlyTest.build.result.json"
if ($auditBuild.ExitCode -ne 0) { throw "Build failed: $OnlyTest; exit=$($auditBuild.ExitCode)" }
$auditBuild.Dispose()
Write-Host "Built ASan $OnlyTest"
$auditExecutable = Join-Path $auditOutput "$OnlyTest.exe"
$auditRunArgs = if ($OnlyTest -eq 'pause_sender_death_test') { @() } else { @('--iterations',"$Iterations") }
$auditProcessArgs = @{
    FilePath=$auditExecutable; WindowStyle='Hidden'; PassThru=$true
    RedirectStandardOutput="$auditOutput/$OnlyTest.stdout.log"
    RedirectStandardError="$auditOutput/$OnlyTest.stderr.log"
}
if ($auditRunArgs.Count -gt 0) { $auditProcessArgs.ArgumentList=$auditRunArgs }
$auditRunStarted = Get-Date
$auditProcess = Start-Process @auditProcessArgs
$auditHandle = $auditProcess.Handle
$auditTimedOut = $false
while (-not $auditProcess.WaitForExit(1000)) {
    if (((Get-Date)-$auditRunStarted).TotalSeconds -ge $TimeoutSeconds) {
        $auditTimedOut=$true; $auditProcess.Kill(); $auditProcess.WaitForExit(); break
    }
}
$auditResult = [ordered]@{
    name=$OnlyTest; command=@($auditExecutable)+$auditRunArgs; exit_code=$auditProcess.ExitCode
    timed_out=$auditTimedOut; elapsed_seconds=((Get-Date)-$auditRunStarted).TotalSeconds
    stdout="$auditOutput/$OnlyTest.stdout.log"; stderr="$auditOutput/$OnlyTest.stderr.log"
    stderr_bytes=(Get-Item -LiteralPath "$auditOutput/$OnlyTest.stderr.log").Length
    sources=$auditSources; executable_sha256=(Get-FileHash -LiteralPath $auditExecutable -Algorithm SHA256).Hash
}
$auditResult | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$auditOutput/$OnlyTest.result.json"
$auditProcess.Dispose()
Get-Content -LiteralPath "$auditOutput/$OnlyTest.stdout.log"
if ($auditTimedOut -or $auditResult.exit_code -ne 0 -or $auditResult.stderr_bytes -ne 0) {
    throw "ASan test failed: $OnlyTest; exit=$($auditResult.exit_code), timed_out=$auditTimedOut"
}
foreach ($auditFile in $auditSources.Keys) {
    if ((Get-FileHash -LiteralPath $auditFile -Algorithm SHA256).Hash -ne $auditSources[$auditFile]) {
        throw "Source changed during supplemental run: $auditFile"
    }
}
Write-Host "Passed ASan $OnlyTest in $($auditResult.elapsed_seconds) seconds; source hashes unchanged"
