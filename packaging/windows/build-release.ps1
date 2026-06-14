<#
.SYNOPSIS
  Build minNotes (Release) and produce the Windows installer.

.DESCRIPTION
  One-shot release build for Windows:
    1. Imports the MSVC x64 environment (vcvars64) + puts Qt's cmake/ninja/Qt bin
       on PATH.
    2. Configures build/ (Release) if not already configured.
    3. Builds minNotes - the CMake POST_BUILD steps run windeployqt and stage the
       vendored DLLs (WinSparkle / FFmpeg / KF6) next to the exe, so build/ is a
       self-contained runtime tree.
    4. Runs Inno Setup (ISCC) on packaging\windows\minNotes.iss, passing the
       version parsed from the root CMakeLists.txt.

  The resulting installer (packaging\windows\minNotes-<ver>-x64.exe) is UNSIGNED.
  Authenticode-sign it + generate/sign the WinSparkle appcast on macOS as a
  separate step (the Ed25519 private key lives in the macOS Keychain).

.PARAMETER VcVars
  Path to vcvars64.bat. Defaults to the VS 2022 Community location.

.PARAMETER QtRoot
  Qt install root. Defaults to C:\Qt\6.11.1\msvc2022_64.

.PARAMETER Reconfigure
  Delete build/CMakeCache.txt and configure from scratch.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File packaging\windows\build-release.ps1
#>
[CmdletBinding()]
param(
    [string]$VcVars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    [string]$QtRoot = "C:\Qt\6.11.1\msvc2022_64",
    [string]$CMakeBin = "C:\Qt\Tools\CMake_64\bin",
    [string]$NinjaBin = "C:\Qt\Tools\Ninja",
    [switch]$Reconfigure
)

$ErrorActionPreference = "Stop"

# --- Paths --------------------------------------------------------------
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$BuildDir = Join-Path $RepoRoot "build"
$IssFile  = Join-Path $PSScriptRoot "minNotes.iss"

Write-Host "Repo root : $RepoRoot"
Write-Host "Build dir : $BuildDir"

# --- Version (source of truth: root CMakeLists.txt project(VERSION x.y.z)) ---
$cml = Get-Content (Join-Path $RepoRoot "CMakeLists.txt") -Raw
if ($cml -notmatch 'project\(MinNotes[^\)]*?VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not parse VERSION from CMakeLists.txt"
}
$Version = $Matches[1]
Write-Host "Version   : $Version"

# --- Toolchain environment ---------------------------------------------
if (-not (Test-Path $VcVars)) { throw "vcvars64.bat not found at $VcVars (pass -VcVars)" }
Write-Host "Importing MSVC environment..."
cmd /c "`"$VcVars`" && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
}
$env:PATH = "$CMakeBin;$NinjaBin;$QtRoot\bin;$env:PATH"

# --- Configure (only if needed) ----------------------------------------
$cache = Join-Path $BuildDir "CMakeCache.txt"
if ($Reconfigure -and (Test-Path $cache)) { Remove-Item $cache -Force }
if (-not (Test-Path $cache)) {
    Write-Host "Configuring (Release)..."
    & cmake -S $RepoRoot -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QtRoot"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
}

# --- Build (POST_BUILD does windeployqt + DLL staging) -----------------
Write-Host "Building minNotes..."
& cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

if (-not (Test-Path (Join-Path $BuildDir "minNotes.exe"))) {
    throw "build\minNotes.exe missing after build"
}

# --- Inno Setup --------------------------------------------------------
$iscc = Get-ChildItem `
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe", `
    "C:\Program Files\Inno Setup 6\ISCC.exe" `
    -ErrorAction SilentlyContinue | Select-Object -First 1 -Expand FullName
if (-not $iscc) { throw "Inno Setup 6 (ISCC.exe) not found - install it to build the installer." }

Write-Host "Building installer (ISCC)..."
& $iscc "/DMyAppVersion=$Version" $IssFile
if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }

$out = Join-Path $PSScriptRoot "minNotes-$Version-x64.exe"
if (Test-Path $out) {
    Write-Host ""
    Write-Host "==> Installer: $out" -ForegroundColor Green
    Write-Host "    (UNSIGNED - Authenticode-sign + make WinSparkle appcast on macOS.)"
} else {
    throw "Installer not found at $out"
}
