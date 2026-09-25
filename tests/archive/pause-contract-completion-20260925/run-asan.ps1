<#
测试项目：契约覆盖补齐 ASan 批次入口
性质: 逐项调用正式 PowerShell 压力运行器, 不自行实现测试断言.
项目: 重跑 extreme、integration、exception, 并运行 sender death 与 contract death;
普通和固定深度链均 10000 轮, 终止各 1 轮, 每项超时 1800 秒.
预期: 下游运行器所有构建/场景与哈希检查通过, 失败抛错并中止后续项目.
运行: MSVC 开发环境 powershell -File 本脚本, 或调用同目录 run-asan.cmd.
边界: 输出仍在 build/pause-contract-completion-20260925/asan, 不把补测重复计入其他证据.
#>
$ErrorActionPreference = 'Stop'
foreach ($name in @('pause_extreme_test','pause_integration_test','pause_exception_matrix_test','pause_sender_death_test','pause_contract_death_test')) {
    & "$PSScriptRoot/../../scripts/run-pause-stress.ps1" -OnlyTest $name -Iterations 10000 -DeepIterations 10000 -TerminationIterations 1 -TimeoutSeconds 1800 -OutputDirectory "$PSScriptRoot/../../../build/pause-contract-completion-20260925/asan/$name"
}
