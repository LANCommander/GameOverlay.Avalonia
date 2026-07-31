<#
.SYNOPSIS
    Launches SampleGame, attaches the overlay host, switches display mode and
    captures a screenshot. This is the end-to-end verification harness.

.DESCRIPTION
    Screen-capturing a D3D game is fiddlier than it looks:

      * The capturing process must be per-monitor DPI aware, otherwise
        GetWindowRect and CopyFromScreen disagree about coordinates and you
        photograph whatever window happens to be nearby.
      * The game window must actually be in the foreground - a desktop capture
        records what is composited on screen, not what a given window drew.
      * Exclusive fullscreen has to be reached by asking the game to switch,
        because the overlay's whole point is that it survives that transition.

.EXAMPLE
    ./capture-overlay.ps1 -Mode exclusive -TestPattern
#>
[CmdletBinding()]
param(
    [ValidateSet('windowed', 'borderless', 'exclusive')]
    [string]$Mode = 'windowed',

    # Which sample renderer to overlay. D3D12 exercises a completely different
    # compositing path in the payload; D3D10 exercises the legacy-shared texture;
    # D3D9 exercises the CPU shared-memory frame transport.
    [ValidateSet('d3d9', 'd3d10', 'd3d11', 'd3d12', 'vulkan', 'opengl')]
    [string]$Api = 'd3d11',

    [switch]$TestPattern,

    [int]$SettleSeconds = 6,

    [string]$OutFile,

    # Leave the game and host running afterwards (for manual inspection).
    [switch]$KeepAlive
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$gameName = switch ($Api) { 'd3d9' { 'SampleGameD3D9' } 'd3d10' { 'SampleGameD3D10' } 'd3d12' { 'SampleGameD3D12' } 'vulkan' { 'SampleGameVulkan' } 'opengl' { 'SampleGameOpenGL' } default { 'SampleGame' } }
$gameExe = Join-Path $repo "build\bin\$gameName.exe"
$hostExe = Join-Path $repo 'src\GameOverlay.Avalonia.Sample\bin\Release\net10.0-windows\win-x64\GameOverlay.Avalonia.Sample.exe'

if (-not (Test-Path $gameExe)) { throw "$gameName not built: $gameExe" }
if (-not (Test-Path $hostExe)) { throw "Host not built: $hostExe" }
if (-not $OutFile) { $OutFile = Join-Path $repo "build\overlay-$Api-$Mode.png" }

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

# Must happen before any coordinate is read, or every rect is virtualised.
[void][Win]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))   # PER_MONITOR_AWARE_V2

$logs = Join-Path $env:TEMP 'GameOverlay.Avalonia.*.log'
Remove-Item $logs -Force -ErrorAction SilentlyContinue

$hostOut = Join-Path $env:TEMP 'overlay_host_out.txt'
$hostErr = Join-Path $env:TEMP 'overlay_host_err.txt'
Remove-Item $hostOut, $hostErr -Force -ErrorAction SilentlyContinue

$game = $null
$hostProc = $null
try {
    # Vulkan must be launched BY the host: the payload has to hook
    # vkCreateDevice before the game calls it, so the process is started
    # suspended and injected into first. Attaching to a running Vulkan game is
    # too late to add the external-memory extensions it needs.
    if ($Api -eq 'vulkan') {
        $hostArgs = @('--launch', $gameExe)
        if ($TestPattern) { $hostArgs += '--test-pattern' }
        $hostProc = Start-Process $hostExe -ArgumentList $hostArgs -PassThru -NoNewWindow `
                        -RedirectStandardOutput $hostOut -RedirectStandardError $hostErr
        Start-Sleep -Seconds 6

        $game = Get-Process $gameName -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $game) { throw "$gameName did not start under the host" }
        Write-Host "[capture] game pid $($game.Id) (launched suspended by the host)"
    }
    else {
        $game = Start-Process $gameExe -PassThru
        Start-Sleep -Seconds 2
        if ($game.HasExited) { throw "$gameName exited immediately (code $($game.ExitCode))" }
        Write-Host "[capture] game pid $($game.Id)"

        $hostArgs = @('--pid', $game.Id)
        if ($TestPattern) { $hostArgs += '--test-pattern' }
        $hostProc = Start-Process $hostExe -ArgumentList $hostArgs -PassThru -NoNewWindow `
                        -RedirectStandardOutput $hostOut -RedirectStandardError $hostErr
        Start-Sleep -Seconds 4
    }

    $game.Refresh()
    $hwnd = $game.MainWindowHandle

    # Bring the game forward before switching modes; exclusive fullscreen is
    # refused for a background window.
    [void][Win]::ShowWindow($hwnd, 9)
    [void][Win]::BringWindowToTop($hwnd)
    [void][Win]::SetForegroundWindow($hwnd)
    Start-Sleep -Seconds 1

    $vk = @{ windowed = 0x70; borderless = 0x71; exclusive = 0x72 }[$Mode]   # F1 / F2 / F3
    Write-Host "[capture] switching to $Mode"
    [void][Win]::PostMessage($hwnd, 0x0100, [IntPtr]$vk, [IntPtr]0)          # WM_KEYDOWN
    [void][Win]::PostMessage($hwnd, 0x0101, [IntPtr]$vk, [IntPtr]0)          # WM_KEYUP

    Start-Sleep -Seconds $SettleSeconds

    $game.Refresh()
    if ($game.HasExited) { throw "game exited during mode switch (code $($game.ExitCode))" }

    # A desktop capture records whatever is composited on screen, so if some
    # other application holds the foreground we would silently photograph that
    # instead and call it a pass.
    [void][Win]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 700
    if ([Win]::GetForegroundWindow() -ne $hwnd) {
        Write-Warning "The game window is NOT in the foreground - the capture will show whatever is on top of it. Close or minimise other fullscreen apps and re-run."
    }

    $r = New-Object Win+RECT
    [void][Win]::GetWindowRect($hwnd, [ref]$r)
    $w = $r.Right - $r.Left
    $h = $r.Bottom - $r.Top
    Write-Host "[capture] window $($r.Left),$($r.Top) ${w}x${h}"

    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))

    # Downscale wide captures so the result is reviewable at a glance.
    if ($w -gt 1400) {
        $scale = 1400 / $w
        $small = New-Object System.Drawing.Bitmap($bmp, (New-Object System.Drawing.Size([int]($w * $scale), [int]($h * $scale))))
        $small.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
        $small.Dispose()
    } else {
        $bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    $gfx.Dispose(); $bmp.Dispose()

    Write-Host "[capture] saved $OutFile"
    Write-Host "[capture] title: $($game.MainWindowTitle)"
}
finally {
    if (-not $KeepAlive) {
        if ($hostProc -and -not $hostProc.HasExited) { Stop-Process -Id $hostProc.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 300
        if ($game -and -not $game.HasExited) { Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue }
    }

    Write-Host "`n--- host stdout ---"
    if (Test-Path $hostOut) { Get-Content $hostOut }
    # Get-Content -Raw yields $null for an empty file, so coalesce before Trim.
    $errText = if (Test-Path $hostErr) { ((Get-Content $hostErr -Raw) ?? '') } else { '' }
    if ($errText.Trim()) { Write-Host "--- host stderr ---"; Write-Host $errText }

    Write-Host "--- payload log ---"
    # The payload holds its log open, so read it with sharing rather than
    # Get-Content, which would fail with a sharing violation.
    Get-ChildItem $logs -ErrorAction SilentlyContinue | ForEach-Object {
        $fs = [System.IO.File]::Open($_.FullName, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object System.IO.StreamReader($fs)
        Write-Host $sr.ReadToEnd()
        $sr.Dispose(); $fs.Dispose()
    }
}
