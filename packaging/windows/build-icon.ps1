# Build the icontool helper and render external/icons/minNotesWindows.svg into
# packaging/windows/minNotes.ico (+ minNotesWindows.png master). Run once after
# the SVG changes. Requires Qt 6.11 (with the Svg module) + an MSVC toolchain.
#
#   pwsh -File packaging\windows\build-icon.ps1
$ErrorActionPreference = "Stop"
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path           # packaging\windows
$root  = Resolve-Path (Join-Path $here "..\..")                    # repo root
$svg   = Join-Path $root "external\icons\minNotesWindows.svg"
$ico   = Join-Path $here "minNotes.ico"
$png   = Join-Path $here "minNotesWindows.png"
$tool  = Join-Path $here "icontool"
$bld   = Join-Path $tool  "build"

$qt     = if ($env:QT_PREFIX) { $env:QT_PREFIX } else { "C:\Qt\6.11.1\msvc2022_64" }
$cmake  = if ($env:CMAKE_EXE) { $env:CMAKE_EXE } else { "C:\Qt\Tools\CMake_64\bin\cmake.exe" }
$ninja  = if ($env:NINJA_EXE) { $env:NINJA_EXE } else { "C:\Qt\Tools\Ninja\ninja.exe" }
$vcvars = if ($env:VS_VCVARS) { $env:VS_VCVARS } else { "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" }
foreach ($p in @($svg, $cmake, $ninja, $vcvars)) { if (-not (Test-Path $p)) { throw "not found: $p" } }
$ninjaFwd = $ninja -replace '\\','/'

function Invoke-MsvcBuild([string]$cmdline) {
    $bat = [System.IO.Path]::GetTempFileName() + ".bat"
    @("@echo off","call `"$vcvars`" || exit /b 1","set `"PATH=$qt\bin;%PATH%`"",$cmdline) |
        Set-Content -Path $bat -Encoding ascii
    try { & cmd.exe /c $bat; $code = $LASTEXITCODE } finally { Remove-Item $bat -ErrorAction SilentlyContinue }
    if ($code -ne 0) { throw "build step failed (exit $code): $cmdline" }
}

Invoke-MsvcBuild "`"$cmake`" -S `"$tool`" -B `"$bld`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninjaFwd`" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=`"$qt`""
Invoke-MsvcBuild "`"$cmake`" --build `"$bld`""

# Run the tool (Qt bin on PATH for its DLLs; offscreen platform so no display needed).
$env:PATH = "$qt\bin;$env:PATH"
$env:QT_QPA_PLATFORM = "offscreen"
& "$bld\icontool.exe" $svg $ico $png
if ($LASTEXITCODE -ne 0) { throw "icontool failed (exit $LASTEXITCODE)" }
Write-Host "icon: $ico"
Write-Host "master: $png"
