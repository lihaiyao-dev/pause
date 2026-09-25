<# 测试项目：历史 callback affinity 四交错 ASan 最终复核
测试项: 运行 lifetime-final.exe --iterations 10000, 共 40000 个依赖组合场景, 超时 240 秒。
预期: 退出码为 0 才记 PASS; 超时终止并写 TIMEOUT。保存 stdout/stderr、计时、源文件 SHA256 与结果 JSON。
入口: 同目录 lifetime-run-final.cmd 或在已初始化 ASan 运行环境下直接执行本脚本。
边界: 使用已构建程序, 不是编译器; 脚本展示 stderr 但未将非空 stderr 单独判失败, 需结合输出审阅。
本脚本是历史依赖生命周期复核, 不含真实 Pause 屏障/分支测试。
#>
$ErrorActionPreference = 'Stop'
$testRepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
Set-Location -LiteralPath $testRepositoryRoot
$taskRoot = Join-Path $testRepositoryRoot 'build/review-10000000-20260923'
$watch = [System.Diagnostics.Stopwatch]::StartNew()
$taskProcess = Start-Process -FilePath "$taskRoot/lifetime-final.exe" -ArgumentList '--iterations','10000' -WorkingDirectory $testRepositoryRoot -WindowStyle Hidden -RedirectStandardOutput "$taskRoot/lifetime-final.stdout.log" -RedirectStandardError "$taskRoot/lifetime-final.stderr.log" -PassThru
$null = $taskProcess.Handle
$finished = $taskProcess.WaitForExit(240000)
if (-not $finished) {
    Stop-Process -Id $taskProcess.Id -Force
    $watch.Stop()
    @{ status='TIMEOUT'; seconds=$watch.Elapsed.TotalSeconds; cases_per_iteration=4; iterations=10000 } | ConvertTo-Json | Set-Content -LiteralPath "$taskRoot/lifetime-final.result.json"
    throw 'Probe exceeded 240 second process timeout'
}
$taskProcess.WaitForExit()
$watch.Stop()
$exitCode = $taskProcess.ExitCode
@{ status= $(if($exitCode -eq 0){'PASS'}else{'FAIL'}); exit_code=$exitCode; seconds=$watch.Elapsed.TotalSeconds; cases_per_iteration=4; iterations=10000; total_cases=40000; asan=$true; source_sha256=(Get-FileHash -Algorithm SHA256 (Join-Path $testRepositoryRoot 'tests/stdexec_callback_affine_lifetime_test.cpp')).Hash } | ConvertTo-Json | Set-Content -LiteralPath "$taskRoot/lifetime-final.result.json"
Get-Content -LiteralPath "$taskRoot/lifetime-final.stdout.log"
Get-Content -LiteralPath "$taskRoot/lifetime-final.stderr.log"
Get-Content -LiteralPath "$taskRoot/lifetime-final.result.json"
exit $exitCode
