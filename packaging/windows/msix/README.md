# minNotes on Windows — the Microsoft Store package

Since 2.0.6, Windows ships only through the **Microsoft Store**. The Store signs the MSIX on ingestion (no
Authenticode certificate, no SmartScreen prompt) and delivers updates, so the Store build has no self-updater:
`MINNOTES_STORE_BUILD` (the `store` preset) leaves WinSparkle out and drops **Help ▸ Check for Updates…**.
`docs/appcast-win.xml` stays frozen at 2.0.5, so the old Inno installs see "no update" rather than an error.
macOS is unchanged: signed DMG + Sparkle (`scripts/sign-and-notarize.sh`, `scripts/update_appcast.sh`).

The packaging is ported from QCView-Player (`installer/`); the certification lessons come from minRender
(`installer/STORE.md`).

## Layout

```
packaging/windows/msix/
├── CMakeLists.txt        the minnotes_msix / minnotes_msix_clean targets + the Partner Center identity
├── AppxManifest.xml.in   the manifest (configure_file @ONLY)
├── build_msix.ps1        stage → prune → C++ runtime → MakePri → MakeAppx → optional signtool
├── images/               tiles, generated from external/icons/minNotesWindows.svg (below)
└── dist/                 the packed .msix (gitignored)
```

Identity (Partner Center → Product management → Product identity), cache variables in `CMakeLists.txt`:
`cbkow.minNotes` / `CN=C64A1D41-EFAA-4EED-AE81-0C074867D4E9` / `cbkow` — the same publisher as QCView.
The MSIX version is `PROJECT_VERSION` + `.0`.

## Build

From a VS x64 developer shell (the script copies the MSVC C++ runtime out of `VCToolsRedistDir`), with
`Qt6_DIR=C:\Qt\6.11.1\msvc2022_64`:

```powershell
cmake --preset store
cmake --build --preset store --target minnotes_msix
```

Output: `packaging\windows\msix\dist\minNotes-<version>-x64.msix`, **unsigned** — the file Partner Center takes.
Use the `store` tree only: the script refuses a build directory that holds `WinSparkle.dll`.

## Signed sideload twin (testing before certification)

Sign a COPY with the family's dev certificate (same publisher CN, so it installs as the same package):

```powershell
Copy-Item dist\minNotes-<v>-x64.msix dist\minNotes-<v>-x64-sideload.msix
signtool sign /fd SHA256 /f <ABSOLUTE path to the dev .pfx> /p <password> /tr http://timestamp.digicert.com /td SHA256 dist\minNotes-<v>-x64-sideload.msix
```

(`build_msix.ps1` also signs in place when `MN_SIGNING_PFX` / `MN_SIGNING_PASSWORD` / `MN_TIMESTAMP_URL` are set.)
signtool's PFX import and `Add-AppxPackage` / `Remove-AppxPackage` fail in an SSH session — run them in the
logged-on desktop session. On the test machine: install the public `.cer` into **Local Machine → Trusted People**
once, then double-click the `-sideload.msix`.

## What the package does and doesn't need

- **Capabilities: `runFullTrust` only.** Everything minNotes writes under AppData (scratch working copies, the
  User Guide, the spelling dictionary, the single-instance lock) is private to the app, so the default MSIX
  file-system / registry virtualization is fine. Never add `unvirtualizedResources` — minRender 0.5.6.0 was denied
  for it.
- **Virtualization caveat, handled:** on a machine that never had the Inno build, the app's AppData files live in
  `%LOCALAPPDATA%\Packages\cbkow.minNotes_<id>\LocalCache` while the app sees them at the AppData path. A sealed
  package's media are extracted there, so `app/core/FileLinks.cpp` resolves the real location
  (`GetFinalPathNameByHandleW`) before handing a path to Explorer or the default app.
- **Coming from the Inno build:** an existing `%APPDATA%\minNotes` folder is used directly, and HKCU preferences
  read through, so settings and dictionary words carry over. Users should uninstall the Inno build (both register
  `.mnd` / `.mnpkg` / `minnotes://`).
- **Minimum OS: Windows 10 1809 (10.0.17763)**, like the Inno installer.
- No hidden Application entries and no execution alias (minRender's HeadlessAppBypass refusal).

## Submission

1. Build the unsigned MSIX from the release commit; Defender-scan it on the box
   (`MpCmdRun.exe -Scan -ScanType 3 -File <msix> -DisableRemediation`).
2. Partner Center → minNotes → new submission: upload the `.msix`, choose **Publish this submission manually**.
   Only one submission can be in flight per app.
3. Certification typically takes 1–3 business days. A denied package gets a version bump before resubmitting.
4. Listing basics: privacy policy <https://minnotes.app/privacy/>, website <https://minnotes.app>, support
   <https://github.com/cbkow/minNotes/issues>; `runFullTrust` justification: "minNotes is a desktop notes editor
   (Qt/C++) that opens and saves user-chosen documents and media anywhere on disk or on network shares; full trust
   is the standard Desktop Bridge entry point for a Win32 application."

## Per-release routine

1. Bump `project(MinNotes VERSION …)` in the root `CMakeLists.txt` (the MSIX version follows).
2. macOS: the usual DMG + Sparkle appcast.
3. Windows: build the Store MSIX (+ the sideload twin if the release touched packaging and needs a desktop test),
   Defender-scan, submit, publish when certified. No `update_appcast_win.sh` any more.

## Regenerating the tiles

```sh
magick -background none -density 1200 external/icons/minNotesWindows.svg -resize 1024x1024 /tmp/mn1024.png
D=packaging/windows/msix/images
for sz in 16 24 32 44 48 256; do
  magick /tmp/mn1024.png -resize ${sz}x${sz} $D/Square44x44Logo.targetsize-${sz}.png
  cp $D/Square44x44Logo.targetsize-${sz}.png $D/Square44x44Logo.targetsize-${sz}_altform-unplated.png
done
magick /tmp/mn1024.png -resize 50x50   $D/StoreLogo.png
magick /tmp/mn1024.png -resize 150x150 $D/Square150x150Logo.png
magick /tmp/mn1024.png -resize 44x44   $D/Square44x44Logo.png
magick /tmp/mn1024.png -resize 150x150 -background none -gravity center -extent 310x150 $D/Wide310x150Logo.png
```
