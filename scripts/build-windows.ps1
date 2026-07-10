# SPDX-License-Identifier: Apache-2.0

[CmdletBinding()]
param(
  [string]$BuildDir = "",
  [string]$Generator = "",
  [string]$BuildType = "",
  [string]$Gpu = "",
  [int]$Parallel = 2,
  [switch]$SkipTests,
  [switch]$SkipPackage,
  [string]$PackageDir = "",
  [string]$ZipPath = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = Join-Path $RepoRoot "build-windows"
}
elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDir = Join-Path $RepoRoot $BuildDir
}

if ([string]::IsNullOrWhiteSpace($Generator)) {
  $Generator = if ($env:BTOP_CMAKE_GENERATOR) { $env:BTOP_CMAKE_GENERATOR } else { "MinGW Makefiles" }
}
if ([string]::IsNullOrWhiteSpace($BuildType)) {
  $BuildType = if ($env:BTOP_BUILD_TYPE) { $env:BTOP_BUILD_TYPE } else { "RelWithDebInfo" }
}
if ([string]::IsNullOrWhiteSpace($Gpu)) {
  $Gpu = if ($env:BTOP_GPU) { $env:BTOP_GPU } else { "ON" }
}
$Gpu = $Gpu.ToUpperInvariant()
if ($Gpu -notin @("ON", "OFF")) {
  throw "Gpu must be ON or OFF, got: $Gpu"
}
if ([string]::IsNullOrWhiteSpace($PackageDir)) {
  $PackageDir = Join-Path $RepoRoot "package"
}
elseif (-not [System.IO.Path]::IsPathRooted($PackageDir)) {
  $PackageDir = Join-Path $RepoRoot $PackageDir
}
if ([string]::IsNullOrWhiteSpace($ZipPath)) {
  $ZipPath = Join-Path $RepoRoot "btop-windows-portable.zip"
}
elseif (-not [System.IO.Path]::IsPathRooted($ZipPath)) {
  $ZipPath = Join-Path $RepoRoot $ZipPath
}

function Remove-StaleObjectArchives {
  if (-not (Test-Path -LiteralPath $BuildDir)) { return }
  $Archives = @(Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter objects.a -ErrorAction Stop)
  foreach ($Archive in $Archives) {
    Remove-Item -LiteralPath $Archive.FullName -Force -ErrorAction Stop
  }
}

cmake -S $RepoRoot -B $BuildDir -G $Generator "-DCMAKE_BUILD_TYPE=$BuildType" "-DBTOP_GPU=$Gpu"

foreach ($Target in @("btop", "btop_windows_collect_diag", "btop_test")) {
  Remove-StaleObjectArchives
  cmake --build $BuildDir --target $Target --config $BuildType --parallel $Parallel
}

if (-not $SkipTests) {
  ctest --test-dir $BuildDir --output-on-failure

  $Diag = Join-Path $BuildDir "btop_windows_collect_diag.exe"
  if (-not (Test-Path -LiteralPath $Diag)) {
    throw "Missing Windows collector diagnostic executable: $Diag"
  }
  & $Diag
}

if (-not $SkipPackage) {
  $PackageScript = Join-Path $PSScriptRoot "package-windows.ps1"
  & $PackageScript -BuildDir $BuildDir -PackageDir $PackageDir -ZipPath $ZipPath
}
