# SPDX-License-Identifier: Apache-2.0

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build-windows"
$Generator = if ($env:BTOP_CMAKE_GENERATOR) { $env:BTOP_CMAKE_GENERATOR } else { "MinGW Makefiles" }
$BuildType = if ($env:BTOP_BUILD_TYPE) { $env:BTOP_BUILD_TYPE } else { "RelWithDebInfo" }

cmake -S $RepoRoot -B $BuildDir -G $Generator -DCMAKE_BUILD_TYPE=$BuildType -DBTOP_GPU=OFF
cmake --build $BuildDir --config $BuildType --parallel

$Diag = Join-Path $BuildDir "btop_windows_collect_diag.exe"
if (Test-Path $Diag) {
  & $Diag
}