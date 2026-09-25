<# 测试项目：stdexec-compat-review-20260924/normalized-pilot.ps1
核对 5 个普通及 2 个控制器 request_stop 断言位置, 在生成副本中根据 stop_requested 前后状态归一化返回值.
对 august-main/latest-main 编译副本并各运行 --iterations 5, 记录超时/退出码, 正式源码不修改.
入口: 同目录 normalized-pilot.cmd; 需要对应历史源码形状和依赖, 计数不匹配应失败.
生成 .cpp/obj/exe/日志仍属 build 产物; 归一化通过不等于公共 API 全兼容.
归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
#>
$ErrorActionPreference = 'Stop'
$pilotRoot = 'D:\workspace\pause\build\stdexec-compat-review-20260924'
$sourceRoot = 'D:\workspace\pause'
function Invoke-CapturedProcess([string]$Executable, [string]$Arguments, [string]$Directory, [string]$Prefix, [int]$TimeoutMs) {
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Executable
    $startInfo.Arguments = $Arguments
    $startInfo.WorkingDirectory = $Directory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        [void]$process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutMs)
        if ($timedOut) { $process.Kill(); $process.WaitForExit() }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        $exitCode = $process.ExitCode
        $clock.Stop()
        [System.IO.File]::WriteAllText($Prefix + '.stdout.log', $stdout)
        [System.IO.File]::WriteAllText($Prefix + '.stderr.log', $stderr)
        $result = [ordered]@{ ExitCode = $exitCode; TimedOut = $timedOut; Seconds = [math]::Round($clock.Elapsed.TotalSeconds, 3) }
        [System.IO.File]::WriteAllText($Prefix + '.result.json', ($result | ConvertTo-Json))
        return $result
    } finally { $process.Dispose() }
}
$source = Join-Path $sourceRoot 'tests\pause_sender_test.cpp'
$originalSource = [System.IO.File]::ReadAllText($source)
$firstPattern = 'CHECK(!stop.request_stop())'
$secondPattern = 'CHECK(!control.stop.request_stop())'
if (([regex]::Matches($originalSource, [regex]::Escape($firstPattern))).Count -ne 5) { throw 'Unexpected stop request assertion count' }
if (([regex]::Matches($originalSource, [regex]::Escape($secondPattern))).Count -ne 2) { throw 'Unexpected control stop request assertion count' }
if (([regex]::Matches($originalSource, 'request_stop\s*\(')).Count -ne 7) { throw 'Unexpected stop request form' }
$insertionPoint = 'using namespace pause_sender_test;'
if (([regex]::Matches($originalSource, [regex]::Escape($insertionPoint))).Count -ne 1) { throw 'Unexpected helper insertion count' }
$helper = @"
using namespace pause_sender_test;
template<class S> bool first_stop(S& source) {
    bool before = source.stop_requested();
    (void)source.request_stop();
    return !before && source.stop_requested();
}
"@
$normalizedSource = $originalSource.Replace($firstPattern, 'CHECK(first_stop(stop))').Replace($secondPattern, 'CHECK(first_stop(control.stop))').Replace($insertionPoint, $helper)
$results = @()
foreach ($snapshot in @('august-main', 'latest-main')) {
    $directory = Join-Path $pilotRoot $snapshot
    $revision = (Get-Content -LiteralPath (Join-Path $directory 'revision.txt') -Raw).Trim()
    $include = Join-Path $directory 'include'
    $testInclude = Join-Path $sourceRoot 'tests'
    $objectFile = Join-Path $directory 'pause_sender_normalized_pilot.obj'
    $executableFile = Join-Path $directory 'pause_sender_normalized_pilot.exe'
    $normalizedFile = Join-Path $directory 'pause_sender_normalized_pilot.cpp'
    [System.IO.File]::WriteAllText($normalizedFile, $normalizedSource)
    $arguments = '/nologo /std:c++latest /EHsc /MD /O2 /Zc:__cplusplus /Zc:preprocessor /I"' + $include + '" /I"' + $sourceRoot + '" /I"' + $testInclude + '" /Fo"' + $objectFile + '" /Fe"' + $executableFile + '" "' + $normalizedFile + '"'
    [System.IO.File]::WriteAllText((Join-Path $directory 'normalized-compile-command.txt'), ('cl.exe ' + $arguments))
    Write-Output ('Compiling normalized ' + $snapshot + ' ' + $revision)
    $compile = Invoke-CapturedProcess 'cl.exe' $arguments $directory (Join-Path $directory 'normalized-compile') 120000
    $run = $null
    if (($compile.ExitCode -eq 0) -and (-not $compile.TimedOut)) {
        Write-Output ('Running normalized ' + $snapshot + ' --iterations 5')
        $run = Invoke-CapturedProcess $executableFile '--iterations 5' $directory (Join-Path $directory 'normalized-run') 60000
    }
    $result = [ordered]@{ Snapshot = $snapshot; Revision = $revision; Compile = $compile; Run = $run }
    $results += $result
    Write-Output ($result | ConvertTo-Json -Depth 5 -Compress)
}
[System.IO.File]::WriteAllText((Join-Path $pilotRoot 'normalized-results.json'), ($results | ConvertTo-Json -Depth 5))
