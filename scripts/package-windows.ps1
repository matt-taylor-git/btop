# SPDX-License-Identifier: Apache-2.0

[CmdletBinding()]
param(
  [string]$BuildDir = "",
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

$BtopExe = Join-Path $BuildDir "btop.exe"
if (-not (Test-Path -LiteralPath $BtopExe)) {
  throw "Missing built executable: $BtopExe"
}

$ThemesDir = Join-Path $RepoRoot "themes"
if (-not (Test-Path -LiteralPath $ThemesDir)) {
  throw "Missing themes directory: $ThemesDir"
}

$BinDir = Join-Path $PackageDir "bin"
$ShareDir = Join-Path $PackageDir "share\btop"

if (Test-Path -LiteralPath $PackageDir) {
  Remove-Item -LiteralPath $PackageDir -Recurse -Force
}
if (Test-Path -LiteralPath $ZipPath) {
  Remove-Item -LiteralPath $ZipPath -Force
}

New-Item -ItemType Directory -Force -Path $BinDir, $ShareDir | Out-Null
Copy-Item -LiteralPath $BtopExe -Destination (Join-Path $BinDir "btop.exe") -Force
Copy-Item -LiteralPath $ThemesDir -Destination (Join-Path $ShareDir "themes") -Recurse -Force

$SystemDlls = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
@(
  "advapi32.dll", "bcrypt.dll", "cfgmgr32.dll", "combase.dll", "crypt32.dll",
  "dnsapi.dll", "gdi32.dll", "iphlpapi.dll", "kernel32.dll", "msvcrt.dll",
  "ntdll.dll", "ole32.dll", "oleaut32.dll", "pdh.dll", "powrprof.dll",
  "psapi.dll", "rpcrt4.dll", "sechost.dll", "shell32.dll", "shlwapi.dll",
  "user32.dll", "version.dll", "ws2_32.dll"
) | ForEach-Object { [void]$SystemDlls.Add($_) }

function Get-ImportedDllNames {
  param([string]$BinaryPath)

  $Objdump = Get-Command objdump -ErrorAction SilentlyContinue
  if ($null -eq $Objdump) {
    throw "objdump not found on PATH; cannot build a complete portable package."
  }

  & $Objdump.Source -p $BinaryPath 2>$null |
    Select-String -Pattern "DLL Name:\s*(.+)$" |
    ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
}

function Find-DllOnPath {
  param([string]$DllName)

  $SearchDirs = @($BinDir) + ($env:PATH -split [System.IO.Path]::PathSeparator)
  foreach ($Dir in $SearchDirs) {
    if ([string]::IsNullOrWhiteSpace($Dir)) { continue }
    $Candidate = Join-Path $Dir $DllName
    if (Test-Path -LiteralPath $Candidate) {
      return (Resolve-Path -LiteralPath $Candidate).Path
    }
  }
  return $null
}

$CopiedDlls = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$VisitedBinaries = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$Queue = [System.Collections.Generic.Queue[string]]::new()
$Queue.Enqueue((Join-Path $BinDir "btop.exe"))

while ($Queue.Count -gt 0) {
  $Binary = $Queue.Dequeue()
  if (-not $VisitedBinaries.Add($Binary)) { continue }

  foreach ($DllName in Get-ImportedDllNames $Binary) {
    if ($DllName.StartsWith("api-ms-win-", [System.StringComparison]::OrdinalIgnoreCase)) { continue }
    if ($SystemDlls.Contains($DllName)) { continue }
    if ($CopiedDlls.Contains($DllName)) { continue }

    $DllPath = Find-DllOnPath $DllName
    if ($null -eq $DllPath) {
      throw "Could not find runtime DLL dependency on PATH: $DllName"
    }

    $Destination = Join-Path $BinDir $DllName
    Copy-Item -LiteralPath $DllPath -Destination $Destination -Force
    [void]$CopiedDlls.Add($DllName)
    $Queue.Enqueue($Destination)
  }
}

Compress-Archive -Path (Join-Path $PackageDir "*") -DestinationPath $ZipPath -Force
Write-Host "Created $ZipPath"
if ($CopiedDlls.Count -gt 0) {
  $DllList = ($CopiedDlls | Sort-Object -CaseSensitive) -join ", "
  Write-Host "Included runtime DLLs: $DllList"
}
