<#
.SYNOPSIS
    Builds the native payload and sample games (CMake), then the overlay library
    and sample app (dotnet).

.EXAMPLE
    ./build.ps1
    ./build.ps1 -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$buildDir = Join-Path $repo 'build'

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "==> cleaning $buildDir"
    Remove-Item $buildDir -Recurse -Force
}

# --- native: payload DLL + sample game ---------------------------------------
# The VS generator is used rather than Ninja because Ninja is not assumed to be
# installed; MSBuild ships with the Build Tools that provide the compiler.
Write-Host "==> configuring CMake (x64)"
cmake -S $repo -B $buildDir -G 'Visual Studio 17 2022' -A x64 | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }

Write-Host "==> building native ($Configuration)"
cmake --build $buildDir --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }

# --- managed: overlay library + sample app -----------------------------------
# The sample references the library, so building it builds both. The native DLL
# is built first (above) so the library can pack/copy it.
Write-Host "==> building library + sample ($Configuration)"
$sampleProj = Join-Path $repo 'src\GameOverlay.Avalonia.Sample\GameOverlay.Avalonia.Sample.csproj'
dotnet build $sampleProj -c $Configuration --nologo
if ($LASTEXITCODE -ne 0) { throw 'Managed build failed' }

$sampleExe = Join-Path $repo "src\GameOverlay.Avalonia.Sample\bin\$Configuration\net10.0-windows\win-x64\GameOverlay.Avalonia.Sample.exe"

Write-Host ''
Write-Host 'Build complete:'
Write-Host "  payload    : $(Join-Path $buildDir 'bin\GameOverlay.Native.dll')"
Write-Host "  game d3d9  : $(Join-Path $buildDir 'bin\SampleGameD3D9.exe')"
Write-Host "  game d3d10 : $(Join-Path $buildDir 'bin\SampleGameD3D10.exe')"
Write-Host "  game d3d11 : $(Join-Path $buildDir 'bin\SampleGame.exe')"
Write-Host "  game d3d12 : $(Join-Path $buildDir 'bin\SampleGameD3D12.exe')"
Write-Host "  game vulkan: $(Join-Path $buildDir 'bin\SampleGameVulkan.exe')"
Write-Host "  game opengl: $(Join-Path $buildDir 'bin\SampleGameOpenGL.exe')"
Write-Host "  library    : src\GameOverlay.Avalonia (GameOverlay.Avalonia.dll)"
Write-Host "  sample     : $sampleExe"
Write-Host ''
Write-Host 'Try it:'
Write-Host '  ./tools/capture-overlay.ps1 -Mode exclusive -KeepAlive'
Write-Host '  ./tools/measure-overlay.ps1'
