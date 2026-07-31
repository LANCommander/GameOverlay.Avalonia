<#
.SYNOPSIS
    Measures the frame-time cost the overlay imposes on the game.

.DESCRIPTION
    Runs an A/B/A comparison - baseline, overlay attached, overlay detached -
    because a straight before/after cannot tell a real cost from thermal drift
    or another process warming up. The third phase should return to the first.

    SampleGame is run uncapped (vsync off) so the overlay's per-frame cost is
    not hidden inside a wait for the display refresh.

    Frame times come from the game's own title bar, which reports a rolling
    window over its last 2000 presents. The script samples that repeatedly and
    aggregates, rather than trusting a single reading.
#>
[CmdletBinding()]
param(
    [int]$PhaseSeconds = 20,

    [ValidateSet('d3d11', 'd3d12', 'vulkan', 'opengl')]
    [string]$Api = 'd3d11',

    [switch]$TestPattern
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$gameName = switch ($Api) { 'd3d12' { 'SampleGameD3D12' } 'vulkan' { 'SampleGameVulkan' } 'opengl' { 'SampleGameOpenGL' } default { 'SampleGame' } }
$gameExe = Join-Path $repo "build\bin\$gameName.exe"
$hostExe = Join-Path $repo 'src\GameOverlay.Avalonia.Sample\bin\Release\net10.0-windows\win-x64\GameOverlay.Avalonia.Sample.exe'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class M {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
}
'@
[void][M]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))

# Matches: "... | 9225 fps | avg 0.108 ms  p50 0.086  p99 0.909 | pid 37296"
$rx = [regex]'\|\s*(?<fps>\d+) fps \| avg (?<avg>[\d.]+) ms\s+p50 (?<p50>[\d.]+)\s+p99 (?<p99>[\d.]+)'

function Sample-Phase {
    param([System.Diagnostics.Process]$Game, [string]$Label, [int]$Seconds)

    # 'C' clears the game's frame-time history so the phase starts clean.
    [void][M]::PostMessage($Game.MainWindowHandle, 0x0100, [IntPtr]0x43, [IntPtr]0)
    [void][M]::PostMessage($Game.MainWindowHandle, 0x0101, [IntPtr]0x43, [IntPtr]0)
    Start-Sleep -Seconds 2   # let the rolling window refill

    $avg = [System.Collections.Generic.List[double]]::new()
    $p50 = [System.Collections.Generic.List[double]]::new()
    $p99 = [System.Collections.Generic.List[double]]::new()
    $fps = [System.Collections.Generic.List[double]]::new()

    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        $Game.Refresh()
        $m = $rx.Match($Game.MainWindowTitle)
        if ($m.Success) {
            $avg.Add([double]$m.Groups['avg'].Value)
            $p50.Add([double]$m.Groups['p50'].Value)
            $p99.Add([double]$m.Groups['p99'].Value)
            $fps.Add([double]$m.Groups['fps'].Value)
        }
        Start-Sleep -Milliseconds 250
    }

    if ($avg.Count -eq 0) { throw "No frame-time samples captured for phase '$Label'." }

    [pscustomobject]@{
        Phase   = $Label
        Samples = $avg.Count
        Fps     = [math]::Round(($fps  | Measure-Object -Average).Average, 0)
        AvgMs   = [math]::Round(($avg  | Measure-Object -Average).Average, 4)
        P50Ms   = [math]::Round(($p50  | Measure-Object -Average).Average, 4)
        P99Ms   = [math]::Round(($p99  | Measure-Object -Average).Average, 4)
    }
}

$game = $null
$hostProc = $null
try {
    $game = Start-Process $gameExe -PassThru
    Start-Sleep -Seconds 3
    [void][M]::SetForegroundWindow($game.MainWindowHandle)
    Start-Sleep -Seconds 2

    $results = @()
    $results += Sample-Phase -Game $game -Label 'baseline (no overlay)' -Seconds $PhaseSeconds

    $hostArgs = @('--pid', $game.Id)
    if ($TestPattern) { $hostArgs += '--test-pattern' }
    $hostProc = Start-Process $hostExe -ArgumentList $hostArgs -PassThru -NoNewWindow `
                    -RedirectStandardOutput (Join-Path $env:TEMP 'measure_host.txt') `
                    -RedirectStandardError  (Join-Path $env:TEMP 'measure_host_err.txt')
    Start-Sleep -Seconds 6
    $results += Sample-Phase -Game $game -Label 'overlay attached' -Seconds $PhaseSeconds

    Stop-Process -Id $hostProc.Id -Force -ErrorAction SilentlyContinue
    $hostProc = $null
    Start-Sleep -Seconds 3
    # Payload stays injected but the host is gone, so nothing is published.
    $results += Sample-Phase -Game $game -Label 'host killed (payload idle)' -Seconds $PhaseSeconds

    Write-Host ''
    $results | Format-Table -AutoSize | Out-String | Write-Host

    $base = $results[0]
    $with = $results[1]
    $dAvg = $with.AvgMs - $base.AvgMs
    $dP50 = $with.P50Ms - $base.P50Ms
    $dP99 = $with.P99Ms - $base.P99Ms

    # Round before formatting: a value like -1e-17 would otherwise pick the
    # negative format section and render as "-+0.000".
    function Signed([double]$v) {
        $r = [math]::Round($v, 3)
        if ($r -eq 0) { return ' 0.000' }
        return ('{0}{1:0.000}' -f $(if ($r -gt 0) { '+' } else { '-' }), [math]::Abs($r))
    }

    Write-Host ("overlay cost: avg {0} ms   p50 {1} ms   p99 {2} ms" -f (Signed $dAvg), (Signed $dP50), (Signed $dP99))

    # SampleGame runs at several thousand fps, where a fixed per-frame cost
    # looks enormous as a percentage (0.03 ms of a 0.06 ms frame is 50%). That
    # ratio says nothing useful about a real game, so express the same absolute
    # cost against frame budgets people actually ship at.
    Write-Host ''
    Write-Host 'Same absolute cost as a share of a real frame budget:'
    foreach ($target in 60, 144, 240) {
        $budget = 1000.0 / $target
        Write-Host ("  {0,4} fps ({1,5:0.00} ms budget): {2,5:0.0}%" -f $target, $budget, ($dAvg / $budget * 100))
    }
    Write-Host ''
    Write-Host ("(measured at {0:N0} fps, where the game's own frame is only {1:0.000} ms," -f $base.Fps, $base.AvgMs)
    Write-Host  " so the percentage at that rate is not representative.)"
}
finally {
    if ($hostProc -and -not $hostProc.HasExited) { Stop-Process -Id $hostProc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 300
    if ($game -and -not $game.HasExited) { Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue }
}
