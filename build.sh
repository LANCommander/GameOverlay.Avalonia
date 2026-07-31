#!/usr/bin/env bash
# Builds the Linux native payload (CMake) and the portable managed library
# (dotnet). The Linux counterpart of build.ps1. Windows-only sample games and
# the net10.0-windows TFM are not built here.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config="${1:-Debug}"
build_dir="$repo/build-linux"

echo "==> configuring CMake (Linux, $config)"
cmake -S "$repo" -B "$build_dir" -DCMAKE_BUILD_TYPE="$config"

echo "==> building native payload"
cmake --build "$build_dir" -j"$(nproc)"

echo "==> building managed library (net10.0)"
dotnet build "$repo/src/GameOverlay.Avalonia/GameOverlay.Avalonia.csproj" -c "$config" -f net10.0 --nologo

echo ""
echo "Build complete:"
echo "  payload : $build_dir/bin/GameOverlay.Native.so"
echo "  library : src/GameOverlay.Avalonia (net10.0)"
