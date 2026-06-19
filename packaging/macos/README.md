# macOS packaging assets

- **`Info.plist.in`** — bundle template (configured by `app/CMakeLists.txt`).
  Declares the `app.minnotes` bundle id, the `.mndb` document type +
  `app.minnotes.mndb` exported UTI, the `minnotes://` URL scheme, and the
  Sparkle `SU*` keys. `SUPublicEDKey` is a placeholder until you generate the
  EdDSA key with Sparkle's `generate_keys` (`external/Sparkle/bin/`).
- **`entitlements.plist`** — hardened-runtime entitlements for Developer ID
  signing (library-validation off, network client, user-selected file access).
- **`minNotesMacOS.png`** — the committed 1024² icon master. The CMake build
  renders the legacy `.icns` (macOS 13–25) from it via `sips`/`iconutil`. The
  source art + the layered `.icon` bundle are committed under `external/icons/`,
  so both the legacy `.icns` and the macOS-26 `Assets.car` are reproducible from
  a clean clone.

## Regenerating the icon master

The master is rendered from the (gitignored) upstream art at
`external/icons/minNotesMacOS.svg`. ImageMagick mishandles the SVG's rotated
rects, so rasterize with **rsvg-convert** first, then center on a transparent
square with margin:

```sh
rsvg-convert -a -w 800 -h 800 external/icons/minNotesMacOS.svg -o /tmp/mn_art.png
magick /tmp/mn_art.png -background none -gravity center -extent 1024x1024 \
    packaging/macos/minNotesMacOS.png
```

## macOS 26 layered icon

`external/icons/minNotesMacOS.icon` (Icon Composer bundle, committed) is
compiled to `Assets.car` by `xcrun actool` when present (needs full Xcode).
macOS 26+ then applies its own squircle + Liquid Glass via `CFBundleIconName`.
The build skips it gracefully (legacy `.icns` only) when the `.icon` or actool
is absent.
