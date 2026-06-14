# Build KSyntaxHighlighting (KDE) + its ECM dependency from source into
# external/kf6, which the root CMakeLists find_package()s. Run once after clone.
# Windows counterpart to build-ksyntax.sh. Requires: git, the Qt 6.11 install,
# and an MSVC toolchain (cl.exe via vcvars64). Uses Qt's bundled cmake + ninja.
#
#   pwsh -File external\build-ksyntax.ps1
#
# Overridable via env: QT_PREFIX, KF_TAG, VS_VCVARS, CMAKE_EXE, NINJA_EXE.
$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$pfx  = Join-Path $here "kf6"
$src  = Join-Path $here "src"

$qt      = if ($env:QT_PREFIX) { $env:QT_PREFIX } else { "C:\Qt\6.11.1\msvc2022_64" }
$tag     = if ($env:KF_TAG)    { $env:KF_TAG }    else { "v6.14.0" }  # 6.14 fixes a UDL clash with Qt 6.11
$cmake   = if ($env:CMAKE_EXE) { $env:CMAKE_EXE } else { "C:\Qt\Tools\CMake_64\bin\cmake.exe" }
$ninja   = if ($env:NINJA_EXE) { $env:NINJA_EXE } else { "C:\Qt\Tools\Ninja\ninja.exe" }
$vcvars  = if ($env:VS_VCVARS) { $env:VS_VCVARS } else { "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" }
# KSyntaxHighlighting generates code from its syntax XML at build time via Perl.
# macOS ships perl; on Windows reuse the one bundled with Git for Windows.
$perl    = if ($env:PERL_EXE)  { $env:PERL_EXE }  else { "C:\Program Files\Git\usr\bin\perl.exe" }
if (-not (Test-Path $perl)) { throw "Perl not found: $perl (set PERL_EXE; KSyntaxHighlighting needs it)" }
$perlFwd  = $perl  -replace '\\','/'
$ninjaFwd = $ninja -replace '\\','/'

foreach ($p in @($cmake, $ninja, $vcvars)) {
    if (-not (Test-Path $p)) { throw "Required tool not found: $p" }
}
if (-not (Test-Path $qt)) { throw "Qt prefix not found: $qt (set QT_PREFIX)" }

if (Test-Path $pfx) { Remove-Item -Recurse -Force $pfx }
if (Test-Path $src) { Remove-Item -Recurse -Force $src }
New-Item -ItemType Directory -Force -Path $pfx, $src | Out-Null

# Run a cmake/ninja command line inside the MSVC dev environment (cl on PATH).
# Written to a temp .bat to avoid fragile && / quoting / %PATH%-expansion in a
# single cmd line. Qt's bin is prepended to PATH so build-time helper exes
# (katehighlightingindexer) can load Qt6Core.dll — without it ninja's indexer
# step dies with a DLL-not-found.
function Invoke-MsvcBuild([string]$cmdline) {
    $bat = [System.IO.Path]::GetTempFileName() + ".bat"
    @(
        "@echo off",
        "call `"$vcvars`" || exit /b 1",
        "set `"PATH=$qt\bin;%PATH%`"",
        $cmdline
    ) | Set-Content -Path $bat -Encoding ascii
    try { & cmd.exe /c $bat; $code = $LASTEXITCODE }
    finally { Remove-Item $bat -ErrorAction SilentlyContinue }
    if ($code -ne 0) { throw "Build step failed (exit $code): $cmdline" }
}

Push-Location $src
try {
    # --- extra-cmake-modules (ECM) ---
    git clone --depth 1 --branch $tag https://invent.kde.org/frameworks/extra-cmake-modules.git ecm
    Invoke-MsvcBuild "`"$cmake`" -S ecm -B ecm-build -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninjaFwd`" -DCMAKE_INSTALL_PREFIX=`"$pfx`" -DBUILD_TESTING=OFF"
    Invoke-MsvcBuild "`"$cmake`" --build ecm-build --target install"

    # --- syntax-highlighting ---
    git clone --depth 1 --branch $tag https://invent.kde.org/frameworks/syntax-highlighting.git ksynt
    Invoke-MsvcBuild "`"$cmake`" -S ksynt -B ksynt-build -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninjaFwd`" -DCMAKE_INSTALL_PREFIX=`"$pfx`" -DCMAKE_PREFIX_PATH=`"$pfx;$qt`" -DPERL_EXECUTABLE=`"$perlFwd`" -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release"
    Invoke-MsvcBuild "`"$cmake`" --build ksynt-build --parallel"
    Invoke-MsvcBuild "`"$cmake`" --build ksynt-build --target install"
}
finally {
    Pop-Location
}

Write-Host "KSyntaxHighlighting installed to $pfx"
