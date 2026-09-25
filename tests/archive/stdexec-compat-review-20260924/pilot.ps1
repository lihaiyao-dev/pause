<# 测试项目：stdexec-compat-review-20260924/pilot.ps1
对 nvhpc-26.05/first-affine/august-main/latest-main 编译 sender 测试, 成功后运行 --iterations 5.
预期: 分别记录编译 120 秒/运行 60 秒超时与退出码, 不要求全部快照通过.
入口: 同目录 pilot.cmd; 需要 build 中四份 include/revision.txt 和 VS 2022, 只验证该 sender 程序的兼容性.
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
$results = @()
foreach ($snapshot in @('nvhpc-26.05', 'first-affine', 'august-main', 'latest-main')) {
    $directory = Join-Path $pilotRoot $snapshot
    $revision = (Get-Content -LiteralPath (Join-Path $directory 'revision.txt') -Raw).Trim()
    $include = Join-Path $directory 'include'
    $objectFile = Join-Path $directory 'pause_sender_pilot.obj'
    $executableFile = Join-Path $directory 'pause_sender_pilot.exe'
    $source = Join-Path $sourceRoot 'tests\pause_sender_test.cpp'
    $arguments = '/nologo /std:c++latest /EHsc /MD /O2 /Zc:__cplusplus /Zc:preprocessor /I"' + $include + '" /I"' + $sourceRoot + '" /Fo"' + $objectFile + '" /Fe"' + $executableFile + '" "' + $source + '"'
    [System.IO.File]::WriteAllText((Join-Path $directory 'pilot-compile-command.txt'), ('cl.exe ' + $arguments))
    Write-Output ('Compiling ' + $snapshot + ' ' + $revision)
    $compile = Invoke-CapturedProcess 'cl.exe' $arguments $directory (Join-Path $directory 'pilot-compile') 120000
    $run = $null
    if (($compile.ExitCode -eq 0) -and (-not $compile.TimedOut)) {
        Write-Output ('Running ' + $snapshot + ' --iterations 5')
        $run = Invoke-CapturedProcess $executableFile '--iterations 5' $directory (Join-Path $directory 'pilot-run') 60000
    }
    $result = [ordered]@{ Snapshot = $snapshot; Revision = $revision; Compile = $compile; Run = $run }
    $results += $result
    Write-Output ($result | ConvertTo-Json -Depth 5 -Compress)
}
[System.IO.File]::WriteAllText((Join-Path $pilotRoot 'pilot-results.json'), ($results | ConvertTo-Json -Depth 5))
