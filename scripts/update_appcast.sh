#!/usr/bin/env bash
# Add a release to the Sparkle appcast (docs/appcast.xml).
#
# docs/ IS the minnotes.app site (GitHub Pages, CNAME in docs/CNAME), so the appcast is published by committing this
# file's change and pushing main. Sparkle fetches https://minnotes.app/appcast.xml and verifies each download against
# SUPublicEDKey in the application's Info.plist, so the signature must come from the matching private key - the one in
# the login keychain, which sign_update uses by default.
#
# Run it AFTER the notarized DMG is attached to the GitHub release v<version>: the enclosure's address points at the
# release asset. The DMG argument is the local file: its bytes are what is signed and measured.
#
# sign_update is Sparkle's tool. minNotes 3 is built in the minplatform repository, whose tools/sparkle/fetch.sh puts it
# in external/sparkle/bin; this script looks for a minplatform checkout beside this one, or takes the path in SIGN_UPDATE.
#
# Usage:
#   scripts/update_appcast.sh <version> <path/to/minNotes-<version>-macOS.dmg> [NOTES_HTML_FILE]
#   SIGN_UPDATE=/path/to/sign_update scripts/update_appcast.sh 3.0.0 minNotes-3.0.0-macOS.dmg
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="cbkow/minNotes"
APPCAST="$REPO_ROOT/docs/appcast.xml"
SIGN_UPDATE="${SIGN_UPDATE:-$REPO_ROOT/../minplatform/external/sparkle/bin/sign_update}"
SENTINEL="<!-- @@APPCAST_INSERT@@ -->"

VERSION="${1:-}"
DMG="${2:-}"
NOTES_FILE="${3:-}"
[ -n "$VERSION" ] && [ -n "$DMG" ] || { echo "usage: $0 <version> <path/to/minNotes-<version>-macOS.dmg> [NOTES_HTML_FILE]" >&2; exit 1; }
TAG="v$VERSION"

# --- Sanity checks ---------------------------------------------------------
[ -f "$DMG" ]        || { echo "ERROR: DMG not found: $DMG" >&2; exit 1; }
[ -x "$SIGN_UPDATE" ] || { echo "ERROR: no sign_update at $SIGN_UPDATE (minplatform: tools/sparkle/fetch.sh, or set SIGN_UPDATE)" >&2; exit 1; }
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
