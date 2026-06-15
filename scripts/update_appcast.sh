#!/usr/bin/env bash
# Add a release to the Sparkle appcast (docs/appcast.xml).
#
# Everything except the EdDSA signature and the release notes is derived:
#   version  → CMakeLists.txt project(VERSION)  (or arg 1)
#   tag      → v<version>
#   DMG      → build/release/minNotes-<version>-macOS.dmg (or arg 2)
#   URL      → github.com/<repo>/releases/download/v<version>/minNotes-<version>-macOS.dmg
#   sig+len  → external/Sparkle/bin/sign_update <dmg>  (uses the Keychain key)
#   date     → date -R
#
# Run AFTER sign-and-notarize.sh has produced the notarized DMG and you've
# created the GitHub release tag v<version> with that DMG uploaded. Then review
# + commit docs/appcast.xml to publish at minnotes.app/appcast.xml.
#
# Usage:
#   scripts/update_appcast.sh [VERSION] [DMG_PATH] [NOTES_HTML_FILE]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="cbkow/minNotes"
APPCAST="$REPO_ROOT/docs/appcast.xml"
SIGN_UPDATE="$REPO_ROOT/external/Sparkle/bin/sign_update"
SENTINEL="<!-- @@APPCAST_INSERT@@ -->"

VERSION="${1:-}"
DMG="${2:-}"
NOTES_FILE="${3:-}"

# --- Derive the version from CMakeLists if not given -----------------------
if [ -z "$VERSION" ]; then
    VERSION="$(awk '/^project\(MinNotes/{f=1} f&&/VERSION/{print $2; exit}' \
                   "$REPO_ROOT/CMakeLists.txt")"
fi
[ -n "$VERSION" ] || { echo "ERROR: could not determine version" >&2; exit 1; }
TAG="v$VERSION"
# Default DMG path depends on VERSION (versioned asset name).
DMG="${DMG:-$REPO_ROOT/build/release/minNotes-$VERSION-macOS.dmg}"

# --- Sanity checks ---------------------------------------------------------
[ -f "$DMG" ]        || { echo "ERROR: DMG not found: $DMG" >&2; exit 1; }
[ -x "$SIGN_UPDATE" ] || { echo "ERROR: $SIGN_UPDATE missing — run scripts/fetch_sparkle.sh" >&2; exit 1; }
grep -q "$SENTINEL" "$APPCAST" || { echo "ERROR: insertion marker missing in $APPCAST" >&2; exit 1; }

if grep -q "<sparkle:version>$VERSION</sparkle:version>" "$APPCAST"; then
    echo "appcast already has an item for $VERSION — nothing to do."
    exit 0
fi

# --- Sign the DMG (EdDSA): sign_update prints
#     sparkle:edSignature="…" length="…"
SIG_LINE="$("$SIGN_UPDATE" "$DMG")"
ED_SIG="$(printf '%s' "$SIG_LINE" | sed -n 's/.*sparkle:edSignature="\([^"]*\)".*/\1/p')"
LENGTH="$(printf '%s' "$SIG_LINE" | sed -n 's/.*length="\([^"]*\)".*/\1/p')"
[ -n "$ED_SIG" ] && [ -n "$LENGTH" ] || {
    echo "ERROR: could not parse sign_update output: $SIG_LINE" >&2; exit 1; }

PUBDATE="$(date -R)"
URL="https://github.com/$REPO/releases/download/$TAG/minNotes-$VERSION-macOS.dmg"

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
      <sparkle:minimumSystemVersion>13.0</sparkle:minimumSystemVersion>
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

echo "appcast: added $VERSION (length=$LENGTH)"
echo "  enclosure: $URL"
echo "  → review docs/appcast.xml, then commit + push to publish."
