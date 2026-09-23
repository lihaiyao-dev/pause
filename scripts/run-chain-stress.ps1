param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Label,
    [string]$OutputDirectory = 'build/chain-audit-20260923',
    [ValidateRange(1,16384)][int]$MaxDepth = 8192,
    [ValidateRange(1,600)][int]$TimeoutSeconds = 60,
    [ValidateRange(64,4096)][int]$MemoryBudgetMB = 1024,
    [switch]$Model,
    [ValidateSet('all','chain','fork')][string]$OnlyTopology = 'all',
    [ValidateSet('all','value','error','stopped','root-stop','error-fast','stopped-fast')][string]$OnlyOutcome = 'all',
    [switch]$InvestigateCrashes
)

# Run from an x64 VS developer environment (ASan needs its runtime DLL path).
# Each attempted depth is a separate process. A resource failure is recorded as
# a failure/limit, never counted as a passing lifecycle test.
$ErrorActionPreference = 'Stop'
$chainExecutable = (Resolve-Path -LiteralPath $Executable).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$chainOutput = (Resolve-Path -LiteralPath $OutputDirectory).Path
if (-not ('ChainStressNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class ChainStressNative {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint mode);
}
'@
}
# Children inherit this error mode; don't open crash dialogs during limit tests.
$chainOldErrorMode = [ChainStressNative]::SetErrorMode(0x0001 -bor 0x0002)
$chainResults = [System.Collections.Generic.List[object]]::new()
$chainBoundaries = [System.Collections.Generic.List[object]]::new()

function Invoke-ChainCase([string]$Topology, [string]$Outcome, [int]$Depth, [string]$Attempt) {
    $caseName = "$Label-$Topology-$Outcome-d$Depth-$Attempt"
    $caseOut = Join-Path $chainOutput "$caseName.stdout.log"
    $caseErr = Join-Path $chainOutput "$caseName.stderr.log"
    $caseStarted = Get-Date
    $caseArguments = if ($Model) { @('--chain-depth',"$Depth",'--chain-only','1') }
        else { @('--depth',"$Depth",'--topology',$Topology,'--outcome',$Outcome) }
    $caseProcess = Start-Process -FilePath $chainExecutable -ArgumentList $caseArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $caseOut -RedirectStandardError $caseErr
    $caseHandle = $caseProcess.Handle
    $casePeakMemory = 0L
    $caseLimit = $null
    while (!$caseProcess.WaitForExit(100)) {
        $caseProcess.Refresh()
        if (!$caseProcess.HasExited) {
            $casePeakMemory = [Math]::Max($casePeakMemory, $caseProcess.PeakWorkingSet64)
            if ($casePeakMemory -gt $MemoryBudgetMB * 1MB) { $caseLimit = 'memory_budget' }
            elseif (((Get-Date) - $caseStarted).TotalSeconds -ge $TimeoutSeconds) { $caseLimit = 'timeout' }
            if ($null -ne $caseLimit) {
                $caseProcess.Kill()
                $caseProcess.WaitForExit()
                break
            }
        }
    }
    $caseProcess.Refresh()
    $caseExit = $caseProcess.ExitCode
    $caseExitHex = '0x{0:X8}' -f [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$caseExit), 0)
    $caseText = [string](Get-Content -Raw -LiteralPath $caseOut)
    $caseError = [string](Get-Content -Raw -LiteralPath $caseErr)
    $caseStatus = if ($null -ne $caseLimit) { $caseLimit }
        elseif ($caseExit -eq 0 -and $caseText -match 'PASS' -and $caseError.Length -eq 0) { 'pass' }
        elseif ($caseExitHex -eq '0xC00000FD' -or $caseError -match 'AddressSanitizer: stack-overflow') { 'stack_overflow' }
        elseif ($caseExitHex -eq '0xC0000005') { 'access_violation' }
        else { 'failure' }
    $caseResult = [pscustomobject][ordered]@{
        name = $caseName; executable = $chainExecutable; arguments = $caseArguments; topology = $Topology
        outcome = $Outcome; depth = $Depth; attempt = $Attempt; status = $caseStatus
        exit_code = $caseExit; exit_hex = $caseExitHex
        started = $caseStarted.ToString('o')
        elapsed_seconds = ((Get-Date) - $caseStarted).TotalSeconds
        sampled_peak_working_set_bytes = $casePeakMemory
        timeout_seconds = $TimeoutSeconds; memory_budget_mb = $MemoryBudgetMB
    }
    $caseResult | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $chainOutput "$caseName.result.json")
    $chainResults.Add($caseResult)
    Write-Host "$caseName : $caseStatus ($caseExitHex)"
    $caseProcess.Dispose()
    return $caseResult
}

try {
    $chainTopologies = if ($Model) { @('model') } else { @('chain','fork') }
    $chainOutcomes = if ($Model) { @('all') } else { @('value','error','stopped','root-stop','error-fast','stopped-fast') }
    if (!$Model -and $OnlyTopology -ne 'all') { $chainTopologies = @($OnlyTopology) }
    if (!$Model -and $OnlyOutcome -ne 'all') { $chainOutcomes = @($OnlyOutcome) }
    foreach ($topology in $chainTopologies) {
        foreach ($outcome in $chainOutcomes) {
            $lastPass = 0
            $firstFailure = 0
            $failureStatus = $null
            for ($chainDepth = 1; $chainDepth -le $MaxDepth; $chainDepth *= 2) {
                $case = Invoke-ChainCase $topology $outcome $chainDepth 'ramp'
                if ($case.status -eq 'pass') { $lastPass = $chainDepth }
                else {
                    $firstFailure = $chainDepth
                    $failureStatus = $case.status
                    break
                }
            }
            # This locates an observed bracket, not a theorem of monotonicity.
            $refineStatuses = @('stack_overflow')
            if ($InvestigateCrashes) { $refineStatuses += 'access_violation' }
            if ($firstFailure -gt 0 -and $failureStatus -in $refineStatuses) {
                while ($firstFailure - $lastPass -gt 1) {
                    $middle = [int][Math]::Floor(($firstFailure + $lastPass) / 2)
                    $case = Invoke-ChainCase $topology $outcome $middle 'bisect'
                    if ($case.status -eq 'pass') { $lastPass = $middle }
                    elseif ($case.status -in $refineStatuses) { $firstFailure = $middle; $failureStatus = $case.status }
                    else { $failureStatus = $case.status; break }
                }
                if ($failureStatus -in $refineStatuses) {
                    for ($repeat = 1; $repeat -le 3; ++$repeat) {
                        if ($lastPass -gt 0) { $null = Invoke-ChainCase $topology $outcome $lastPass "confirm$repeat" }
                        $null = Invoke-ChainCase $topology $outcome $firstFailure "confirm$repeat"
                    }
                }
            }
            $lowerConfirm = @($chainResults | Where-Object {
                $_.topology -eq $topology -and $_.outcome -eq $outcome -and
                $_.depth -eq $lastPass -and $_.attempt -like 'confirm*'
            })
            $upperConfirm = @($chainResults | Where-Object {
                $_.topology -eq $topology -and $_.outcome -eq $outcome -and
                $_.depth -eq $firstFailure -and $_.attempt -like 'confirm*'
            })
            $chainBoundaries.Add([pscustomobject]@{
                topology = $topology; outcome = $outcome; observed_pass_depth = $lastPass
                observed_failure_depth = $firstFailure; failure_status = $failureStatus
                requested_max_depth = $MaxDepth
                lower_confirm_passes = @($lowerConfirm | Where-Object status -eq 'pass').Count
                lower_confirm_attempts = $lowerConfirm.Count
                upper_confirm_failures = @($upperConfirm | Where-Object status -ne 'pass').Count
                upper_confirm_attempts = $upperConfirm.Count
            })
            $chainBoundaries | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $chainOutput "$Label-boundaries.json")
        }
    }
}
finally {
    $chainResults | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $chainOutput "$Label-cases.json")
    [void][ChainStressNative]::SetErrorMode($chainOldErrorMode)
}

$chainBoundaries | Format-Table -AutoSize
if (@($chainResults | Where-Object { $_.status -in @('failure','access_violation') }).Count -gt 0) {
    throw 'Unexpected failures occurred; inspect per-process logs.'
}
