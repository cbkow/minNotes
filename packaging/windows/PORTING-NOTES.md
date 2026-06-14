# minNotes — Windows port notes (handoff)

What the macOS v0.1.1 release work assumes, and the matching Windows pieces that
still need building. The app core (Qt6/QML/C++20, SQLite store, the video/audio
engine) is already cross-platform — the gaps are **OS integration + packaging +
updates**. QCView-Player is the sibling reference for everything here; paths
below point at its implementation. Note where minNotes intends to **diverge**
from QCView (QCView ships Microsoft Store/MSIX with no in-app updater; minNotes
does self-hosted Sparkle on macOS and probably wants the Windows equivalent).

The macOS side is all `if(APPLE)`-guarded — **nothing here is blocked by the
mac work, and the mac work won't break the Windows build.** `main.cpp`'s argv
handling is already cross-platform.

---

## The contract the mac side already set (keep these identical on Windows)

| Thing | Value | Where (mac) |
|---|---|---|
| App / product name | **minNotes** | bundle `minNotes.app` |
| Identifier | **app.minnotes** | `CFBundleIdentifier` |
| URI scheme | **`minnotes://`** — `minnotes:///abs/path/doc.mndb` opens that doc | `Info.plist.in` CFBundleURLTypes |
| Document extension | **`.mndb`** (SQLite-backed) | `BlockModel`/`Main.qml` |
| Document UTI (mac) | `app.minnotes.mndb` | `Info.plist.in` UTExportedTypeDeclarations |
| Update feed | `https://minnotes.app/appcast.xml` (GitHub Pages from `docs/`) | `Info.plist.in` SUFeedURL |
| Release asset name | **`minNotes-MacOS.dmg`** (stable) → Windows: `minNotes-Setup.exe` or `.msix` | `scripts/sign-and-notarize.sh` |
| Version | `project(MinNotes VERSION x.y.z)` drives everything | root `CMakeLists.txt` |

Version-match rule (from `scripts/RELEASE.md`): CMake VERSION == git tag minus
`v` == appcast `sparkle:version` == the packaged binary's version. Holds for
Windows too.

---

## 1. CLI / file / URI open  ✅ done — **single-instance intentionally skipped**

- **Done (cross-platform):** `app/main.cpp` `resolveAndOpen()` already maps a
  file path, `file://` URL, or `minnotes://` deep link to `openDocument()`, and
  loops `app.arguments()` at startup. On Windows the file path / URI arrives as
  **argv** (not a `QFileOpenEvent` — that's macOS-only; `MinNotesApplication`'s
  handler simply never fires on Windows, which is fine).
- **Decision: NO single-instance for this app.** minNotes deliberately ships
  without `QLocalServer`/named-pipe single-instance handoff. Consequence to know:
  - **Windows:** double-clicking a `.mndb` (or following a `minnotes://` link)
    while minNotes is already running **launches a new process/window** for that
    document. That's the intended behavior — each open is its own instance.
  - **macOS:** the OS makes the `.app` single-instance for free (Launch Services
    routes the open to the already-running app as a `QFileOpenEvent` rather than
    launching a second copy), so a second open lands in the running instance.
  - This is a behavioral asymmetry between the platforms, accepted by design — no
    `QLocalServer` work to do. (Do **not** copy QCView's single-instance code.)

## 2. URI scheme + file association registration  ❌ not started

Two valid paths — **decide with the updater/packaging choice (§4/§6)**:

- **Installer + registry (NSIS/Inno)** — for self-hosted distribution (pairs with
  WinSparkle, mirrors the mac self-hosted model). The installer writes:
  - URI scheme: `HKCR\minnotes` → `URL Protocol`, `shell\open\command` =
    `"<install>\minNotes.exe" "%1"`.
  - File type: `HKCR\.mndb` → ProgID `minNotes.Document`; `HKCR\minNotes.Document`
    with `DefaultIcon` = `"<install>\minNotes.exe",0` and `shell\open\command` =
    `"<install>\minNotes.exe" "%1"`.
- **MSIX manifest** — for Microsoft Store (QCView's path). No registry writes;
  `AppxManifest.xml` declares `windows.protocol` (Name=`minnotes`) and
  `windows.fileTypeAssociation` (`.mndb`). Reference:
  `QCView-Player/installer/msix/AppxManifest.xml.in`.

## 3. App icon  ❌ not started (source art ready)

- Source: `external/icons/minNotesWindows.svg` already exists (committed). Unlike
  macOS, Windows has **no system squircle/mortise** — the `.ico` art should bake
  in its own background/rounding (QCView ships `qcview.png` with the squircle
  baked, separate from the transparent mac art). Confirm `minNotesWindows.svg`
  is the baked variant.
- Steps (mirror the mac master-render pattern in `packaging/macos/README.md`):
  1. Render a committed PNG master, then build a multi-res `.ico`:
     `magick minNotesWindows.png -define icon:auto-resize=256,128,96,64,48,32,24,16 packaging/windows/minNotes.ico`
  2. Add `packaging/windows/minNotes.rc` with `IDI_ICON1 ICON "minNotes.ico"`.
  3. Add the `.rc` to the executable sources under `if(WIN32)` in
     `app/CMakeLists.txt` (MSVC's linker embeds it; Explorer/taskbar/Alt-Tab read
     the first ICON). Optionally `app.setWindowIcon()` at runtime for the
     title-bar icon. Reference: `QCView-Player/packaging/windows/qcview.rc`.

## 4. Auto-update  ❌ not started — **key decision**

QCView punts to the **Microsoft Store** (its Windows updater stub is a no-op).
minNotes does self-hosted Sparkle on macOS, so the natural Windows mirror is:

- **Recommended: WinSparkle** — Sparkle-for-Windows. Reads a Sparkle-style
  appcast (RSS + `sparkle:` namespace) and supports `sparkle:edSignature`
  (Ed25519) — i.e. it can reuse the **same EdDSA key** we already use for mac
  (`+Xlrad3W…`, in the login Keychain). Wire `win_sparkle_set_appcast_url()` +
  `win_sparkle_set_eddsa_public_key()` + `win_sparkle_init()` at launch, and a
  File ▸ Check for Updates calling `win_sparkle_check_update_with_ui()`. This is
  the Windows counterpart to `app/sparkle_updater_macos.mm` /
  `SPUStandardUpdaterController`. WinSparkle downloads + runs your installer
  (so it pairs with NSIS/Inno, **not** MSIX).
  - **Separate appcast:** keep a `docs/appcast-win.xml` (or add `sparkle:os="windows"`
    to items in the shared feed) — the enclosure points at the Windows installer,
    not the `.dmg`. `scripts/update_appcast.sh` is mac-specific; clone it for win.
- **Alternative: Microsoft Store / MSIX** — Microsoft signs + delivers updates,
  no in-app updater (delete the Check-for-Updates item on Windows). Simpler, but
  diverges from the self-hosted model and the GitHub-release flow.

## 5. Packaging  ❌ not started (coupled to §4)

- **WinSparkle path → NSIS or Inno Setup** installer: install to Program Files,
  Start-Menu shortcut, write the §2 registry keys, bundle Qt via `windeployqt`
  (+ the vendored FFmpeg DLLs — see §7), uninstaller. Output `minNotes-Setup.exe`,
  uploaded to the GitHub release like the `.dmg`.
- **Store path → MSIX**: copy `QCView-Player/installer/msix/` wholesale
  (`AppxManifest.xml.in`, `build_msix.ps1` running `windeployqt` → `MakePri` →
  `MakeAppx`), swap Identity/Publisher/protocol/file-type for minNotes.

## 6. Code signing  ❌ not started

- Authenticode via `signtool.exe` (Windows 10/11 SDK) on the installer/exe (and
  the MSIX if that path). Reference: `QCView-Player/installer/msix/build_msix.ps1`
  (`signtool sign /fd SHA256 /f <pfx> /tr <timestamp> /td SHA256`).
- **Separate cert from macOS** — needs a Windows code-signing certificate.
  Note SmartScreen reputation: a standard OV cert builds reputation slowly; an
  **EV cert** gets instant SmartScreen trust. QCView used a self-signed dev PFX
  for sideload + Store central signing — for self-hosted distribution you'll want
  a real OV/EV cert.

## 7. Build / runtime details  ⚠️ partial

- **`WIN32_EXECUTABLE TRUE`** on the target (no console window on launch). QCView
  sets it; minNotes does **not** yet — add under the Windows config. Because that
  hides stdout, consider a file-logger Qt message handler (QCView tees to
  `qcview-log.txt`) if you want logs.
- **FFmpeg DLLs:** the Windows hwaccel path (`app/player/`: Vulkan + D3D11 +
  WASAPI) is already wired in `app/CMakeLists.txt`'s `elseif(WIN32)` block and
  expects the Vulkan SDK + `external/ffmpeg` Windows binaries (`lib` + `dll`).
  `windeployqt` won't copy the FFmpeg DLLs — the installer/MSIX staging must copy
  them next to the exe (the Windows analog of the mac `bundle_dylibs.sh` step).
- **Deployment floor:** mac is 13.0; pick the Windows floor (QCView targets
  Win10+; D3D11/Vulkan path needs a recent enough GPU stack).
- **`qt.conf` / plugin paths:** `windeployqt` handles this; verify the
  `sqldrivers/qsqlite` plugin ships (the document store needs it — same concern
  as the mac `libqsqlite` we keep in `sign-and-notarize.sh`).

---

## Suggested order

1. **Decide updater + packaging** (§4/§6) — WinSparkle+NSIS (self-hosted, mirrors
   mac) vs MSIX/Store. Everything downstream forks here.
2. **Icon** (§3) + **`WIN32_EXECUTABLE`** (§7) — quick wins.
3. **Registry/manifest** (§2) via the chosen installer.
4. **FFmpeg DLL staging** (§7) + **signing** (§6).
5. Windows appcast + a `scripts/` Windows release driver mirroring
   `sign-and-notarize.sh` / `update_appcast.sh`.

(Single-instance is intentionally **not** on this list — see §1.)

Until Windows is working end-to-end, this stays a dry run — the real shared
release is **v0.1.2** (per the mac runbook).
