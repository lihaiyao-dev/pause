<# 测试项目：stdexec-cmake-check-20260924/run-matrix.ps1
old-nvhpc-26.05 配置预期失败, first-affine/latest-main 构建及运行 toolchain/header 两个烟雾目标.
同一 reconfigure 目录先接受最新快照再拒绝旧快照, 检查能力探针缓存是否失效.
入口: 同目录 run-matrix.cmd; 为上游预置 RAPIDS.cmake/execution.bs 离线元数据, 不修改依赖头快照.
预期: 各退出码符合预设, 不匹配即退出 1; 需固定 CMake/Ninja/MSVC/离线 sources, 结果保留 build.
归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
#>
$ErrorActionPreference = 'Continue'
$taskRoot = 'D:/workspace/pause'
$taskMatrix = "$taskRoot/build/stdexec-cmake-check-20260924"
$taskCmake = 'D:/CMake/bin/cmake.exe'
$taskResults = [System.Collections.Generic.List[object]]::new()

function Invoke-MatrixCommand {
    param([string]$Name, [string]$Executable, [string[]]$CommandArguments, [int]$ExpectedExit = 0)
    $taskLog = "$taskMatrix/logs/$Name.log"
    & $Executable @CommandArguments *> $taskLog
    $taskExit = $LASTEXITCODE
    $taskResults.Add([pscustomobject]@{
        name = $Name
        exit_code = $taskExit
        expected_exit = $ExpectedExit
        log = $taskLog
    })
    Write-Host "$Name : exit=$taskExit expected=$ExpectedExit"
    if ($taskExit -ne $ExpectedExit) {
        Get-Content -LiteralPath $taskLog -Tail 65
        $taskResults | ConvertTo-Json -Depth 5 | Set-Content "$taskMatrix/results.json"
        exit 1
    }
}

function Invoke-MatrixConfigure {
    param([string]$Name, [string]$SourceVersion, [string]$BuildName, [int]$ExpectedExit = 0)
    # Older upstream CMake directly downloads these build metadata inputs even
    # with FETCHCONTENT_FULLY_DISCONNECTED. Seed the local helper and the revision
    # field used only to form the project version, keeping source/header archives
    # untouched and the compatibility probe independent of Git/version metadata.
    $taskUpstreamBuild = "$taskMatrix/builds/$BuildName/_deps/stdexec-build"
    New-Item -ItemType Directory -Force $taskUpstreamBuild | Out-Null
    Copy-Item -LiteralPath "$taskRoot/build/msvc-debug/_deps/rapids-cmake-src/RAPIDS.cmake" -Destination "$taskUpstreamBuild/RAPIDS.cmake"
    Set-Content -LiteralPath "$taskUpstreamBuild/execution.bs" -Value "# Offline configure metadata only; not a specification snapshot.`nRevision: 11"
    $taskArgs = @(
        '-S', $taskRoot,
        '-B', "$taskMatrix/builds/$BuildName",
        '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_MAKE_PROGRAM=D:/Ninja/ninja.exe',
        '-DPAUSE_BUILD_TESTS=OFF',
        '-DPAUSE_BUILD_DESIGN_TESTS=OFF',
        '-DPAUSE_BUILD_TOOLCHAIN_SMOKE_TEST=ON',
        '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
        "-DFETCHCONTENT_SOURCE_DIR_STDEXEC=$taskMatrix/sources/$SourceVersion",
        "-DFETCHCONTENT_SOURCE_DIR_RAPIDS-CMAKE=$taskRoot/build/msvc-debug/_deps/rapids-cmake-src",
        "-DFETCHCONTENT_SOURCE_DIR_ICM=$taskRoot/build/msvc-debug/_deps/icm-src",
        "-DCPM_SOURCE_CACHE=$taskRoot/build/msvc-debug"
    )
    Invoke-MatrixCommand -Name $Name -Executable $taskCmake -CommandArguments $taskArgs -ExpectedExit $ExpectedExit
}

Invoke-MatrixConfigure -Name 'old-rejected' -SourceVersion 'old-nvhpc-26.05' -BuildName 'old-nvhpc-26.05' -ExpectedExit 1
foreach ($taskVersion in @('first-affine', 'latest-main')) {
    Invoke-MatrixConfigure -Name "$taskVersion-configure" -SourceVersion $taskVersion -BuildName $taskVersion
    Invoke-MatrixCommand -Name "$taskVersion-build" -Executable $taskCmake -CommandArguments @('--build', "$taskMatrix/builds/$taskVersion", '--target', 'pause_toolchain_smoke', 'pause_header_compile')
    Invoke-MatrixCommand -Name "$taskVersion-toolchain-smoke" -Executable "$taskMatrix/builds/$taskVersion/pause_toolchain_smoke.exe" -CommandArguments @()
    Invoke-MatrixCommand -Name "$taskVersion-header-smoke" -Executable "$taskMatrix/builds/$taskVersion/pause_header_compile.exe" -CommandArguments @()
}
Invoke-MatrixConfigure -Name 'same-build-new-accepted' -SourceVersion 'latest-main' -BuildName 'reconfigure'
Invoke-MatrixConfigure -Name 'same-build-old-rejected' -SourceVersion 'old-nvhpc-26.05' -BuildName 'reconfigure' -ExpectedExit 1
$taskResults | ConvertTo-Json -Depth 5 | Set-Content "$taskMatrix/results.json"
