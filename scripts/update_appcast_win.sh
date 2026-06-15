#!/usr/bin/env bash
# Add a release to the WinSparkle appcast (docs/appcast-win.xml).
#
# The Windows counterpart of update_appcast.sh. The Windows binary points at
# https://minnotes.app/appcast-win.xml (app/winsparkle_updater_windows.cpp), a
# SEPARATE feed whose enclosure is the Windows installer, not the .dmg.
# WinSparkle verifies the SAME EdDSA key as macOS Sparkle (sign_update output
# format is identical), so we reuse external/Sparkle/bin/sign_update + the
# Keychain key.
#
# Everything except the release notes is derived:
#   version  → CMakeLists.txt project(VERSION)  (or arg 1)
#   tag      → v<version>
#   EXE      → packaging/windows/minNotes-<version>-x64.exe (or arg 2)
#   URL      → github.com/<repo>/releases/download/v<version>/minNotes-<version>-x64.exe
#   sig+len  → external/Sparkle/bin/sign_update <exe>  (uses the Keychain key)
#   date     → date -R
#
# Run AFTER the GitHub release tag v<version> exists with the EXE uploaded under
# its exact name. Then review + commit docs/appcast-win.xml to publish.
#
# Usage:
#   scripts/update_appcast_win.sh [VERSION] [EXE_PATH] [NOTES_HTML_FILE]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="cbkow/minNotes"
APPCAST="$REPO_ROOT/docs/appcast-win.xml"
SIGN_UPDATE="$REPO_ROOT/external/Sparkle/bin/sign_update"
SENTINEL="<!-- @@APPCAST_INSERT@@ -->"

VERSION="${1:-}"
NOTES_FILE="${3:-}"

# --- Derive the version from CMakeLists if not given -----------------------
if [ -z "$VERSION" ]; then
    VERSION="$(awk '/^project\(MinNotes/{f=1} f&&/VERSION/{print $2; exit}' \
                   "$REPO_ROOT/CMakeLists.txt")"
fi
[ -n "$VERSION" ] || { echo "ERROR: could not determine version" >&2; exit 1; }
TAG="v$VERSION"
ASSET="minNotes-$VERSION-x64.exe"
EXE="${2:-$REPO_ROOT/packaging/windows/$ASSET}"

# --- Sanity checks ---------------------------------------------------------
[ -f "$EXE" ]        || { echo "ERROR: installer not found: $EXE" >&2; exit 1; }
[ -x "$SIGN_UPDATE" ] || { echo "ERROR: $SIGN_UPDATE missing — run scripts/fetch_sparkle.sh" >&2; exit 1; }
grep -q "$SENTINEL" "$APPCAST" || { echo "ERROR: insertion marker missing in $APPCAST" >&2; exit 1; }

if grep -q "<sparkle:version>$VERSION</sparkle:version>" "$APPCAST"; then
    echo "appcast-win already has an item for $VERSION — nothing to do."
    exit 0
fi

# --- Sign the EXE (EdDSA): sign_update prints
#     sparkle:edSignature="…" length="…"
SIG_LINE="$("$SIGN_UPDATE" "$EXE")"
ED_SIG="$(printf '%s' "$SIG_LINE" | sed -n 's/.*sparkle:edSignature="\([^"]*\)".*/\1/p')"
LENGTH="$(printf '%s' "$SIG_LINE" | sed -n 's/.*length="\([^"]*\)".*/\1/p')"
[ -n "$ED_SIG" ] && [ -n "$LENGTH" ] || {
    echo "ERROR: could not parse sign_update output: $SIG_LINE" >&2; exit 1; }

PUBDATE="$(date -R)"
URL="https://github.com/$REPO/releases/download/$TAG/$ASSET"

if [ -n "$NOTES_FILE" ] && [ -f "$NOTES_FILE" ]; then
    NOTES="$(cat "$NOTES_FILE")"
else
    NOTES="      <ul><li>See the release notes at https://github.com/$REPO/releases/tag/$TAG</li></ul>"
fi

# Build the item in a temp file (multi-line — fed to sed's `r` below).
ITEM_FILE="$(mktemp)"
cat > "$ITEM_FILE" <<EOF
    <item>
      <title>Version $VERSION</title>
      <link>https://minnotes.app/</link>
      <sparkle:version>$VERSION</sparkle:version>
      <sparkle:shortVersionString>$VERSION</sparkle:shortVersionString>
      <sparkle:os>windows</sparkle:os>
      <description><![CDATA[
$NOTES
      ]]></description>
      <pubDate>$PUBDATE</pubDate>
      <enclosure
        url="$URL"
        sparkle:edSignature="$ED_SIG"
        length="$LENGTH"
        type="application/octet-stream" />
    </item>
EOF

# Insert the item right after the marker line (newest-first). sed's `r` reads
# the file in after the matched line — handles multi-line cleanly.
TMP="$(mktemp)"
sed "/APPCAST_INSERT/r $ITEM_FILE" "$APPCAST" > "$TMP"
mv "$TMP" "$APPCAST"
rm -f "$ITEM_FILE"

echo "appcast-win: added $VERSION (length=$LENGTH)"
echo "  enclosure: $URL"
echo "  → review docs/appcast-win.xml, then commit + push to publish."
