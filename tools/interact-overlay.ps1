<#
.SYNOPSIS
    Drives the overlay's Avalonia controls with synthetic input and captures the
    result.

.DESCRIPTION
    Because the payload subclasses the game's window procedure, messages sent
    with PostMessage to the game's HWND are intercepted by the overlay exactly
    as real input would be. That makes interaction fully scriptable: no physical
    mouse, no focus races, and deterministic coordinates.

    Coordinates are in the game's CLIENT pixels, which is also the overlay's
    backbuffer space, so they map 1:1 onto what the overlay rendered.

.EXAMPLE
    ./interact-overlay.ps1 -Mode exclusive
#>
[CmdletBinding()]
param(
    [ValidateSet('windowed', 'borderless', 'exclusive')]
    [string]$Mode = 'windowed',

    [ValidateSet('d3d11', 'd3d12', 'vulkan', 'opengl')]
    [string]$Api = 'd3d11',

    [string]$OutFile,

    [switch]$KeepAlive
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$gameName = switch ($Api) { 'd3d12' { 'SampleGameD3D12' } 'vulkan' { 'SampleGameVulkan' } 'opengl' { 'SampleGameOpenGL' } default { 'SampleGame' } }
$gameExe = Join-Path $repo "build\bin\$gameName.exe"
$hostExe = Join-Path $repo 'src\GameOverlay.Avalonia.Sample\bin\Release\net10.0-windows\win-x64\GameOverlay.Avalonia.Sample.exe'
if (-not (Test-Path $gameExe)) { throw "$gameName not built: $gameExe" }
if (-not $OutFile) { $OutFile = Join-Path $repo "build\interact-$Api-$Mode.png" }

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Ui {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
[void][Ui]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))

# Window messages
$WM_KEYDOWN=0x0100; $WM_KEYUP=0x0101; $WM_CHAR=0x0102
$WM_MOUSEMOVE=0x0200; $WM_LBUTTONDOWN=0x0201; $WM_LBUTTONUP=0x0202

function LParam([int]$x, [int]$y) { [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }

# Not named Move: that is a built-in alias for Move-Item, and PowerShell
# resolves aliases before functions, so calls would silently go there.
function MoveTo([IntPtr]$h, [int]$x, [int]$y) {
    [void][Ui]::PostMessage($h, $WM_MOUSEMOVE, [IntPtr]0, (LParam $x $y)); Start-Sleep -Milliseconds 40
}
function Click([IntPtr]$h, [int]$x, [int]$y) {
    MoveTo $h $x $y
    [void][Ui]::PostMessage($h, $WM_LBUTTONDOWN, [IntPtr]1, (LParam $x $y)); Start-Sleep -Milliseconds 60
    [void][Ui]::PostMessage($h, $WM_LBUTTONUP,   [IntPtr]0, (LParam $x $y)); Start-Sleep -Milliseconds 120
}
function TypeText([IntPtr]$h, [string]$text) {
    foreach ($ch in $text.ToCharArray()) {
        [void][Ui]::PostMessage($h, $WM_CHAR, [IntPtr][int][char]$ch, [IntPtr]0)
        Start-Sleep -Milliseconds 30
    }
}
function Drag([IntPtr]$h, [int]$x1, [int]$y, [int]$x2) {
    MoveTo $h $x1 $y
    [void][Ui]::PostMessage($h, $WM_LBUTTONDOWN, [IntPtr]1, (LParam $x1 $y)); Start-Sleep -Milliseconds 60
    for ($x = $x1; $x -le $x2; $x += 12) {
        [void][Ui]::PostMessage($h, $WM_MOUSEMOVE, [IntPtr]1, (LParam $x $y)); Start-Sleep -Milliseconds 25
    }
    [void][Ui]::PostMessage($h, $WM_LBUTTONUP, [IntPtr]0, (LParam $x2 $y)); Start-Sleep -Milliseconds 120
}

# Toggles capture through the host's real global hotkey. keybd_event feeds the
# low-level keyboard hook the same way a physical key would, so this exercises
# the actual toggle path rather than a test-only back door.
function ToggleCapture {
    $VK_SHIFT = 0x10; $VK_F1 = 0x70; $KEYUP = 2
    [Ui]::keybd_event($VK_SHIFT, 0, 0, [UIntPtr]::Zero)
    [Ui]::keybd_event($VK_F1,    0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [Ui]::keybd_event($VK_F1,    0, $KEYUP, [UIntPtr]::Zero)
    [Ui]::keybd_event($VK_SHIFT, 0, $KEYUP, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 600
}

function GameTitle([System.Diagnostics.Process]$p) { $p.Refresh(); return $p.MainWindowTitle }
function InputCount([System.Diagnostics.Process]$p) {
    if ((GameTitle $p) -match 'input (\d+)') { return [int]$Matches[1] } else { return -1 }
}

$hostOut = Join-Path $env:TEMP 'interact_host.txt'
$hostErr = Join-Path $env:TEMP 'interact_host_err.txt'
Remove-Item $hostOut, $hostErr -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:TEMP 'GameOverlay.Avalonia.*.log') -Force -ErrorAction SilentlyContinue

$game = $null; $hostProc = $null
$results = [System.Collections.Generic.List[string]]::new()
try {
    # Vulkan must be launched by the host (payload has to hook vkCreateDevice
    # before the game calls it); the D3D backends attach to a running process.
    # Either way the overlay starts passive - capture has to come AFTER the
    # display-mode switch, because a capturing payload swallows the very F2/F3
    # keypress that changes mode.
    if ($Api -eq 'vulkan') {
        $hostProc = Start-Process $hostExe -ArgumentList @('--launch', $gameExe) -PassThru -NoNewWindow `
                        -RedirectStandardOutput $hostOut -RedirectStandardError $hostErr
        Start-Sleep -Seconds 6
        $game = Get-Process $gameName -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $game) { throw "$gameName did not start under the host" }
        Write-Host "[interact] game pid $($game.Id) (launched suspended by the host)"
    }
    else {
        $game = Start-Process $gameExe -PassThru
        Start-Sleep -Seconds 2
        Write-Host "[interact] game pid $($game.Id)"

        $hostProc = Start-Process $hostExe -ArgumentList @('--pid', $game.Id) -PassThru -NoNewWindow `
                        -RedirectStandardOutput $hostOut -RedirectStandardError $hostErr
        Start-Sleep -Seconds 5
    }

    $game.Refresh()
    $hwnd = $game.MainWindowHandle
    [void][Ui]::ShowWindow($hwnd, 9)
    [void][Ui]::BringWindowToTop($hwnd)
    [void][Ui]::SetForegroundWindow($hwnd)
    Start-Sleep -Seconds 1

    if ($Mode -ne 'windowed') {
        $vk = @{ borderless = 0x71; exclusive = 0x72 }[$Mode]
        [void][Ui]::PostMessage($hwnd, $WM_KEYDOWN, [IntPtr]$vk, [IntPtr]0)
        [void][Ui]::PostMessage($hwnd, $WM_KEYUP,   [IntPtr]$vk, [IntPtr]0)
        Start-Sleep -Seconds 4
    }

    # Now that the mode is settled, take input.
    Write-Host '[interact] enabling capture via Shift+F1'
    ToggleCapture

    $cr = New-Object Ui+RECT
    [void][Ui]::GetClientRect($hwnd, [ref]$cr)
    Write-Host "[interact] client $($cr.Right)x$($cr.Bottom)"

    # The host reports where each demo control actually laid out, in client
    # pixels. Hardcoding coordinates would break the moment the UI scale changes
    # - and it scales with the game's resolution, so windowed and 4K borderless
    # would need different constants.
    $targets = @{}
    foreach ($line in (Get-Content $hostOut)) {
        if ($line -match '^\[target\] (\w+)=(\d+),(\d+),(\d+),(\d+)') {
            $targets[$Matches[1]] = [pscustomobject]@{
                X = [int]$Matches[2]; Y = [int]$Matches[3]
                W = [int]$Matches[4]; H = [int]$Matches[5]
            }
        }
    }
    if ($targets.Count -eq 0) { throw 'Host did not report any [target] lines; cannot aim at controls.' }
    $targets.GetEnumerator() | Sort-Object Name | ForEach-Object {
        Write-Host ("[interact] target {0,-12} x={1} y={2} {3}x{4}" -f $_.Key, $_.Value.X, $_.Value.Y, $_.Value.W, $_.Value.H)
    }
    function CentreOf($name) {
        $t = $targets[$name]
        if (-not $t) { throw "No reported bounds for '$name'." }
        return @([int]($t.X + $t.W / 2), [int]($t.Y + $t.H / 2))
    }

    $before = InputCount $game
    Write-Host "[interact] hwnd=$hwnd  game input count before: $before"
    if ($hwnd -eq [IntPtr]::Zero) { throw 'Game window handle is null; PostMessage would go nowhere.' }

    # Sanity-check that synthetic messages are reaching the window at all.
    $probeOk = [Ui]::PostMessage($hwnd, $WM_MOUSEMOVE, [IntPtr]0, (LParam 10 10))
    Write-Host "[interact] PostMessage probe returned $probeOk"

    # --- 1. click the demo button -------------------------------------------
    $p = CentreOf 'ClickButton'
    Click $hwnd $p[0] $p[1]
    Click $hwnd $p[0] $p[1]
    Click $hwnd $p[0] $p[1]
    $results.Add('clicked button x3')

    # --- 2. focus the text box and type --------------------------------------
    $p = CentreOf 'DemoTextBox'
    Click $hwnd $p[0] $p[1]
    TypeText $hwnd 'hello from PostMessage'
    $results.Add('typed into TextBox')

    # --- 3. drag the slider ---------------------------------------------------
    $s = $targets['DemoSlider']
    $y = [int]($s.Y + $s.H / 2)
    Drag $hwnd ([int]($s.X + 8)) $y ([int]($s.X + $s.W - 8))
    $results.Add('dragged Slider')

    # --- 4. park the cursor over the button so hover state is visible ---------
    $p = CentreOf 'ClickButton'
    MoveTo $hwnd ($p[0] + 120) $p[1]
    Start-Sleep -Milliseconds 400

    $during = InputCount $game
    Write-Host "[interact] game input count after interaction: $during"

    # --- capture --------------------------------------------------------------
    $wr = New-Object Ui+RECT
    [void][Ui]::GetWindowRect($hwnd, [ref]$wr)
    $w = $wr.Right - $wr.Left; $h = $wr.Bottom - $wr.Top
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($wr.Left, $wr.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    if ($w -gt 1400) {
        $s = 1400 / $w
        $small = New-Object System.Drawing.Bitmap($bmp, (New-Object System.Drawing.Size([int]($w*$s), [int]($h*$s))))
        $small.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png); $small.Dispose()
    } else { $bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png) }
    $g.Dispose(); $bmp.Dispose()
    Write-Host "[interact] saved $OutFile"

    # The host prints its counters every 5 s. Without this wait the only line in
    # the log predates the interaction entirely, which makes the counters look
    # alarming for no reason.
    Start-Sleep -Seconds 6

    # --- 5. suppression must be reversible ------------------------------------
    # Kill the host: the payload's heartbeat timeout has to release capture,
    # otherwise a crashed host leaves the game permanently deaf.
    Stop-Process -Id $hostProc.Id -Force -ErrorAction SilentlyContinue
    $hostProc = $null
    Start-Sleep -Seconds 4

    $released = InputCount $game
    Click $hwnd 400 300
    Start-Sleep -Milliseconds 500
    $after = InputCount $game

    Write-Host ''
    Write-Host '================ RESULTS ================'
    $results | ForEach-Object { Write-Host "  did: $_" }
    Write-Host ''
    Write-Host ("  game input count: before=$before duringCapture=$during afterHostKill=$after")
    if ($during -eq $before) {
        Write-Host '  PASS: game received NO input while the overlay was capturing' -ForegroundColor Green
    } else {
        Write-Host "  FAIL: game input count advanced by $($during - $before) during capture" -ForegroundColor Red
    }
    if ($after -gt $released) {
        Write-Host '  PASS: game regained input after the host was killed' -ForegroundColor Green
    } else {
        Write-Host '  FAIL: game did NOT regain input after the host was killed' -ForegroundColor Red
    }
    Write-Host '========================================='
}
finally {
    if ($hostProc -and -not $hostProc.HasExited) { Stop-Process -Id $hostProc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 300
    if (-not $KeepAlive -and $game -and -not $game.HasExited) { Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue }

    Write-Host "`n--- host stdout ---"
    if (Test-Path $hostOut) { Get-Content $hostOut }
    $e = if (Test-Path $hostErr) { ((Get-Content $hostErr -Raw) ?? '') } else { '' }
    if ($e.Trim()) { Write-Host "--- host stderr ---"; Write-Host $e }
    Write-Host '--- payload log ---'
    Get-ChildItem (Join-Path $env:TEMP 'GameOverlay.Avalonia.*.log') -ErrorAction SilentlyContinue | ForEach-Object {
        $fs = [System.IO.File]::Open($_.FullName, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object System.IO.StreamReader($fs)
        Write-Host $sr.ReadToEnd(); $sr.Dispose(); $fs.Dispose()
    }
}
