# minNotes — macOS Build & Release Runbook

The full flow: build → sign → notarize → GitHub release → Sparkle appcast →
in-place auto-update. Adapted from QCView's runbook.

> **Status:** the release *flow* is wired and verified to build a correct,
> Sparkle-enabled `.app` (v0.1.1). We do **not** ship a real Sparkle update
> until **v0.1.2**, once the Windows port is working too. Until then this is a
> dry-run / dogfood path.

---

## 0. One-time machine setup

- **Qt** 6.11.1 at `/Users/chris/Qt/6.11.1/macos`.
- **Developer ID Application** cert in the login keychain:
  `Developer ID Application: Christopher Bialkowski (5Z4S9VHV56)`
  (`security find-identity -v -p codesigning`).
- **Notary profile** `AC_PASSWORD` stored for notarytool
  (`xcrun notarytool history --keychain-profile AC_PASSWORD`).
- **Full Xcode** (not just Command Line Tools) — `xcrun actool` compiles the
  macOS 26 layered `.icon`. Without it the build still works (legacy `.icns`).
- **Sparkle** vendored once per fresh checkout:
  `scripts/fetch_sparkle.sh` → `external/Sparkle/` (gitignored). The CMake bundle
  block guards on its presence; without it the app builds with auto-update
  disabled (no-op shim).
- **rsvg-convert + ImageMagick** (`brew install librsvg imagemagick`) — only
  needed to *regenerate* the icon master (see `packaging/macos/README.md`); the
  committed `packaging/macos/minNotesMacOS.png` is enough to build.

### Sparkle EdDSA signing key (generate once)

The appcast is EdDSA-signed. Generate the keypair once and keep the **private**
key in the login Keychain (a base64 backup in your vault); commit the **public**
key into `packaging/macos/Info.plist.in`.

```sh
scripts/fetch_sparkle.sh                       # vendors external/Sparkle/bin/*
external/Sparkle/bin/generate_keys             # stores private key in Keychain,
                                               # prints the public key
```

Paste the printed public key into `packaging/macos/Info.plist.in`, replacing
`REPLACE_WITH_ED25519_PUBLIC_KEY` in the `SUPublicEDKey` value, then rebuild.
(Recover later with `generate_keys -p` to reprint the public key, or
`generate_keys -f sparkle_private_key.txt` to import a backup.)

## The naming contract (must hold or URLs 404)

- Git tag = **`vX.Y.Z`** (e.g. `v0.1.2`) = the CMake `project(VERSION)` with a `v`.
- Release asset = **exactly `minNotes-MacOS.dmg`** (stable, non-versioned).
- Per release these must all match: CMake `VERSION` == tag-minus-`v` ==
  appcast `sparkle:version` == bundle `CFBundleShortVersionString`.
- Sparkle does **no discovery** — the appcast `<enclosure url>` is the only
  source of truth. The appcast lives at `https://minnotes.app/appcast.xml`
  (GitHub Pages from `docs/`, `SUFeedURL` in Info.plist).

## 1. Build (release)

```sh
just release          # cmake --preset=release && build
# → build/release/app/minNotes.app
```

## 2. Sign + notarize + DMG

```sh
scripts/sign-and-notarize.sh            # defaults to build/release
# → build/release/minNotes-MacOS.dmg  (signed, notarized, stapled)
```

Pipeline: scrub bundle → macdeployqt (Qt frameworks/plugins/QML, keeps
`libqsqlite`, prunes ODBC/Mimer/PSQL) → `bundle_dylibs.sh` (FFmpeg + KF6 into
Frameworks/, install-name rewrite, minos→13.0) → sign Sparkle helpers
inside-out → sign Frameworks/PlugIns → sign app with entitlements + hardened
runtime → notarize the `.app` → staple → build/sign/notarize/staple the DMG →
Gatekeeper assess.

## 3. GitHub release

Create tag `v<version>` and upload `minNotes-MacOS.dmg` as the asset (exact
name). The site Download button uses `/releases/latest/download/minNotes-MacOS.dmg`.

## 4. Appcast (publishes the auto-update)

```sh
scripts/update_appcast.sh               # signs the DMG, inserts the <item>
git add docs/appcast.xml && git commit && git push   # publishes
```

Existing installs poll `SUFeedURL` daily (or via File ▸ Check for Updates…),
verify the EdDSA signature against `SUPublicEDKey`, download, and install in
place.
