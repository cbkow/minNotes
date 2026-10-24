# build_msix.ps1 -- package a Store build of minNotes (minNotes.exe + the tree its POST_BUILD steps stage next to it)
# as an MSIX. Ported from QCView-Player installer/msix/build_msix.ps1.
#
# Invocation (preferred -- via CMake, from a VS x64 dev shell so the C++ runtime can be found):
#     cmake --preset store
#     cmake --build --preset store --target minnotes_msix
# Output: packaging\windows\msix\dist\minNotes-<version>-x64.msix
#
# Signing
# -------
# Unset (the default) = an UNSIGNED package: the Microsoft Store artifact (Partner Center signs it on ingestion).
# Set these for a signed SIDELOAD package (the family's dev cert, same publisher CN as the manifest):
#     $env:MN_SIGNING_PFX      = "C:\absolute\path\to\dev.pfx"    # ABSOLUTE: this script runs from another cwd
#     $env:MN_SIGNING_PASSWORD = "..."
#     $env:MN_TIMESTAMP_URL    = "http://timestamp.digicert.com"   # optional
# signtool's PFX import fails in an SSH session (0x80090010) -- sign from the logged-on desktop.
#
# ASCII-only on purpose: Windows PowerShell 5.1 parses a BOM-less script in the system codepage.

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $BuildDir,
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $ManifestPath,
    [Parameter()]          [string] $ImagesDir = (Join-Path $PSScriptRoot "images"),
    [Parameter()]          [string] $OutputDir = (Join-Path $PSScriptRoot "dist"),
    # Keep everything windeployqt deployed (see the prune step).
    [Parameter()]          [switch] $NoPrune
)

$ErrorActionPreference = "Stop"

# --- SDK tools --------------------------------------------------------------
function Find-SdkTool {
    param([string] $name)
    foreach ($root in @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "${env:ProgramFiles}\Windows Kits\10\bin")) {
        if (-not (Test-Path $root)) { continue }
        # Newest SDK first (the bin dir holds one folder per SDK version).
        $hits = Get-ChildItem -Path $root -Recurse -Filter $name -ErrorAction SilentlyContinue `
            | Where-Object { $_.FullName -match "\\x64\\" } | Sort-Object FullName -Descending
        if ($hits) { return ($hits | Select-Object -First 1).FullName }
    }
    return $null
}
$MakeAppx = Find-SdkTool "MakeAppx.exe"
$MakePri  = Find-SdkTool "MakePri.exe"
$SignTool = Find-SdkTool "signtool.exe"
if (-not $MakeAppx) { throw "MakeAppx.exe not found. Install the Windows 10/11 SDK." }
if (-not $MakePri)  { throw "MakePri.exe not found. Install the Windows 10/11 SDK." }
Write-Host "MakeAppx : $MakeAppx"
Write-Host "MakePri  : $MakePri"

# --- Inputs -----------------------------------------------------------------
$exe = Join-Path $BuildDir "minNotes.exe"
if (-not (Test-Path $exe)) { throw "minNotes.exe not found at $exe. Build first." }
if (-not (Test-Path (Join-Path $BuildDir "Qt6Core.dll"))) {
    throw "Qt6Core.dll missing from $BuildDir -- the windeployqt POST_BUILD step did not run."
}
if (-not (Test-Path $ManifestPath)) { throw "AppxManifest.xml not found at $ManifestPath. Re-run CMake configure." }
# The Store owns updates: a package carrying the self-updater is the wrong build (configure with the store preset).
if (Test-Path (Join-Path $BuildDir "WinSparkle.dll")) {
    throw "WinSparkle.dll is in $BuildDir -- not a Store build. Use a fresh tree: cmake --preset store."
}

# --- Stage ------------------------------------------------------------------
$staging = Join-Path $PSScriptRoot "staging"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Path $staging | Out-Null
Write-Host "Staging into $staging ..."

# The same set the Inno installer packed: the exe, every DLL beside it, the licence texts and the Qt plugin / QML trees.
Copy-Item -Path $exe -Destination $staging
Get-ChildItem -Path $BuildDir -Filter "*.dll" | Copy-Item -Destination $staging
foreach ($subdir in @("Licenses", "qml", "platforms", "imageformats", "iconengines", "networkinformation",
                       "generic", "sqldrivers", "tls", "styles")) {
    $src = Join-Path $BuildDir $subdir
    if (Test-Path $src) { Copy-Item -Recurse -Path $src -Destination (Join-Path $staging $subdir) }
}

# The MSVC C++ runtime, app-local: the build's windeployqt runs with --no-compiler-runtime (the Inno installs leaned on
# a system-wide VC++ redistributable), and a packaged app must not. VCToolsRedistDir comes from the VS dev shell.
$crt = $null
if ($env:VCToolsRedistDir) {
    $crt = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" `
        -ErrorAction SilentlyContinue | Select-Object -First 1
}
if (-not $crt) { throw "The MSVC CRT redist folder was not found (VCToolsRedistDir='$env:VCToolsRedistDir'). Run from a VS x64 dev shell." }
Write-Host "C++ runtime: $($crt.FullName)"
Get-ChildItem -Path $crt.FullName -Filter "*.dll" | Copy-Item -Destination $staging -Force

# --- Prune what windeployqt deploys and minNotes never loads ------------------
#   qmltooling / QtQuick\tooling   the QML debugger and designer support
#   the Controls styles            the app uses Fusion (main.cpp) and Basic; the other styles + their impl DLLs go
if (-not $NoPrune) {
    $prune = @(
        "Qt6QuickControls2FluentWinUI3StyleImpl.dll",
        "Qt6QuickControls2Imagine.dll", "Qt6QuickControls2ImagineStyleImpl.dll",
        "Qt6QuickControls2Material.dll", "Qt6QuickControls2MaterialStyleImpl.dll",
        "Qt6QuickControls2Universal.dll", "Qt6QuickControls2UniversalStyleImpl.dll",
        "Qt6QuickControls2WindowsStyleImpl.dll",
        "qml\QtQuick\tooling",
        "qml\QtQuick\Controls\FluentWinUI3", "qml\QtQuick\Controls\Imagine",
        "qml\QtQuick\Controls\Material", "qml\QtQuick\Controls\Universal",
        "qml\QtQuick\Controls\Windows"
    )
    $removed = 0L
    foreach ($rel in $prune) {
        $p = Join-Path $staging $rel
        if (-not (Test-Path $p)) { continue }
        $item = Get-Item $p
        if ($item -is [System.IO.DirectoryInfo]) {
            $sum = (Get-ChildItem -Path $p -Recurse -File -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
            if ($sum) { $removed += $sum }
        } else { $removed += $item.Length }
        Remove-Item -Recurse -Force $p
    }
    Write-Host ("Pruned {0:N1} MB the app never loads (-NoPrune keeps it)" -f ($removed / 1MB))
}

# --- Manifest, tiles, licence ------------------------------------------------
Copy-Item -Path $ManifestPath -Destination (Join-Path $staging "AppxManifest.xml")
Copy-Item -Recurse -Path $ImagesDir -Destination (Join-Path $staging "images")
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
$license = Join-Path $repoRoot "LICENSE"
if (Test-Path $license) { Copy-Item -Path $license -Destination $staging }

# --- resources.pri (the target-size / unplated qualifiers on the tile images) --
Write-Host "Generating resources.pri ..."
$priConfig = Join-Path $staging "priconfig.xml"
& $MakePri createconfig /cf $priConfig /dq "en-us" /o | Out-Null
& $MakePri new /pr $staging /cf $priConfig /mn (Join-Path $staging "AppxManifest.xml") /of (Join-Path $staging "resources.pri") /o
if ($LASTEXITCODE -ne 0) { throw "MakePri new failed (exit $LASTEXITCODE)" }
Remove-Item -Path $priConfig

# --- Pack -------------------------------------------------------------------
if (-not (Test-Path $OutputDir)) { New-Item -ItemType Directory -Path $OutputDir | Out-Null }
$output = Join-Path $OutputDir "minNotes-${Version}-x64.msix"
if (Test-Path $output) { Remove-Item -Path $output }
Write-Host "Packing $output ..."
& $MakeAppx pack /d $staging /p $output /o
if ($LASTEXITCODE -ne 0) { throw "MakeAppx pack failed (exit $LASTEXITCODE)" }
Remove-Item -Recurse -Force $staging

# --- Sign (sideload only) -----------------------------------------------------
$pfx = $env:MN_SIGNING_PFX
if ($pfx) {
    if (-not (Test-Path $pfx)) { throw "MN_SIGNING_PFX='$pfx' does not exist (it must be an ABSOLUTE path)." }
    if (-not $SignTool) { throw "MN_SIGNING_PFX is set but signtool.exe was not found." }
    Write-Host "Signing $output ..."
    $signArgs = @("sign", "/fd", "SHA256", "/f", $pfx)
    if ($env:MN_SIGNING_PASSWORD) { $signArgs += @("/p", $env:MN_SIGNING_PASSWORD) }
    if ($env:MN_TIMESTAMP_URL)    { $signArgs += @("/tr", $env:MN_TIMESTAMP_URL, "/td", "SHA256") }
    $signArgs += $output
    & $SignTool @signArgs
    if ($LASTEXITCODE -ne 0) { throw "signtool sign failed (exit $LASTEXITCODE)" }
} else {
    Write-Host "MN_SIGNING_PFX not set -- UNSIGNED package (the Store artifact)."
}

Write-Host ""
Write-Host "SUCCESS: $output"
