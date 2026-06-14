#!/bin/bash
set -euo pipefail

# minNotes macOS — full release pipeline.
# Sign → DMG → notarize → staple → Gatekeeper assess.
# Adapted from QCView-Player/scripts/sign-and-notarize.sh.
#
# Usage:
#   ./scripts/sign-and-notarize.sh [build-dir]   # default: build/release
#
# Prerequisites:
#   - Developer ID Application certificate in Keychain
#   - notarytool keychain profile named "AC_PASSWORD"
#     (`xcrun notarytool history --keychain-profile AC_PASSWORD`)
#   - A release build at <build-dir>/app/minNotes.app
#   - Sparkle vendored at external/Sparkle (scripts/fetch_sparkle.sh)

BUILD_DIR="${1:-build/release}"
APP_BUNDLE="$BUILD_DIR/app/minNotes.app"
ENTITLEMENTS="packaging/macos/entitlements.plist"
SIGNING_IDENTITY="Developer ID Application: Christopher Bialkowski (5Z4S9VHV56)"
NOTARY_PROFILE="AC_PASSWORD"
MACDEPLOYQT="/Users/chris/Qt/6.11.1/macos/bin/macdeployqt"
QML_SOURCE_DIR="app/qml"

VERSION="$(plutil -extract CFBundleShortVersionString raw \
                  "$APP_BUNDLE/Contents/Info.plist" 2>/dev/null || echo "0.0.0")"
# Stable filename so GitHub release "latest/download" links stay valid across
# versions; the real version lives inside (CFBundleShortVersionString).
DMG_NAME="minNotes-MacOS.dmg"
DMG_PATH="$BUILD_DIR/$DMG_NAME"

echo "=== minNotes macOS — sign + DMG + notarize + staple ==="
echo "App:      $APP_BUNDLE"
echo "Version:  $VERSION"
echo "DMG:      $DMG_PATH"
echo "Identity: $SIGNING_IDENTITY"
echo ""

# Verify prerequisites
[ -d "$APP_BUNDLE" ]      || { echo "ERROR: bundle missing at $APP_BUNDLE"; exit 1; }
[ -f "$ENTITLEMENTS" ]    || { echo "ERROR: entitlements missing at $ENTITLEMENTS"; exit 1; }
[ -x "$MACDEPLOYQT" ]     || { echo "ERROR: macdeployqt missing at $MACDEPLOYQT"; exit 1; }
[ -d "$QML_SOURCE_DIR" ]  || { echo "ERROR: QML source dir missing at $QML_SOURCE_DIR"; exit 1; }
security find-identity -v -p codesigning | grep -q "Developer ID Application" \
    || { echo "ERROR: no Developer ID Application certificate"; exit 1; }
xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1 \
    || { echo "ERROR: notarytool keychain profile '$NOTARY_PROFILE' not set up"; exit 1; }

# =========================================================================
# Step 0: scrub bundle — codesign refuses xattrs / .DS_Store and any
# non-Mach-O file in Contents/MacOS/ (only the binary belongs there).
# =========================================================================
echo "[0/9] Cleaning bundle..."
find "$APP_BUNDLE" -name ".DS_Store" -delete 2>/dev/null || true
find "$APP_BUNDLE/Contents/MacOS" -mindepth 1 \
    ! -name "minNotes" -delete 2>/dev/null || true
xattr -cr "$APP_BUNDLE"

# =========================================================================
# Step 0.25: bundle Qt frameworks + plugins + QML modules with macdeployqt.
# Run BEFORE bundle_dylibs.sh — macdeployqt understands Qt's nested PlugIns/
# + Resources/qml/ layout, rewrites install_names to @rpath, and pulls in the
# QML modules the app imports. -qmldir scans the .qml for `import` lines.
# =========================================================================
echo "[0.25/9] Bundling Qt frameworks/plugins/QML modules (macdeployqt)..."
"$MACDEPLOYQT" "$APP_BUNDLE" \
    -qmldir="$QML_SOURCE_DIR" \
    -no-codesign \
    -no-strip \
    -verbose=2 2>&1 | tail -20

# minNotes IS a SQLite app (the document store) — KEEP libqsqlite. macdeployqt
# also speculatively copies the ODBC / Mimer / PostgreSQL drivers, which carry
# hard refs to libs that don't exist on user machines (/opt/homebrew/opt/...).
# Drop only those three so the bundle stays self-contained.
if [ -d "$APP_BUNDLE/Contents/PlugIns/sqldrivers" ]; then
    echo "  Pruning unused SQL drivers (keeping libqsqlite)..."
    rm -f "$APP_BUNDLE/Contents/PlugIns/sqldrivers/libqsqlmimer.dylib"
    rm -f "$APP_BUNDLE/Contents/PlugIns/sqldrivers/libqsqlodbc.dylib"
    rm -f "$APP_BUNDLE/Contents/PlugIns/sqldrivers/libqsqlpsql.dylib"
fi

# =========================================================================
# Step 0.5: bundle the remaining non-Qt dylibs (vendored FFmpeg +
# KSyntaxHighlighting) into Frameworks/ and rewrite their install names.
# =========================================================================
echo "[0.5/9] Bundling vendored dylibs (FFmpeg + KF6)..."
"$(dirname "$0")/bundle_dylibs.sh" "$APP_BUNDLE"

# =========================================================================
# Step 1: strip prior signatures (idempotency on re-run)
# =========================================================================
echo "[1/9] Stripping prior signatures..."
codesign --remove-signature "$APP_BUNDLE" 2>/dev/null || true
while IFS= read -r f; do
    codesign --remove-signature "$f" 2>/dev/null || true
done < <({
    find "$APP_BUNDLE/Contents/Frameworks" -maxdepth 1 -name "*.dylib" -type f 2>/dev/null
    find "$APP_BUNDLE/Contents/Frameworks" -name "*.framework" -type d 2>/dev/null
    find "$APP_BUNDLE/Contents/PlugIns" -name "*.dylib" -type f 2>/dev/null
})

# =========================================================================
# Step 1.5: sign Sparkle.framework's nested code inside-out (deepest-first)
# or notarization fails on the unsigned XPC/Autoupdate/Updater helpers.
# =========================================================================
SPARKLE_FW="$APP_BUNDLE/Contents/Frameworks/Sparkle.framework"
if [ -d "$SPARKLE_FW" ]; then
    echo "[1.5/9] Signing Sparkle nested helpers (inside-out)..."
    SP_SIGN_COUNT=0
    sparkle_sign() {
        codesign --force --sign "$SIGNING_IDENTITY" \
            --timestamp --options runtime "$1"
        SP_SIGN_COUNT=$((SP_SIGN_COUNT + 1))
    }
    while IFS= read -r x; do sparkle_sign "$x"; done \
        < <(find "$SPARKLE_FW"/Versions/*/XPCServices -maxdepth 1 -name "*.xpc" -type d 2>/dev/null)
    while IFS= read -r a; do sparkle_sign "$a"; done \
        < <(find "$SPARKLE_FW"/Versions/* -maxdepth 1 -name "Updater.app" -type d 2>/dev/null)
    while IFS= read -r u; do sparkle_sign "$u"; done \
        < <(find "$SPARKLE_FW"/Versions/* -maxdepth 1 -name "Autoupdate" -type f 2>/dev/null)
    echo "  Signed $SP_SIGN_COUNT Sparkle helper(s)"
fi

# =========================================================================
# Step 2: sign Frameworks/ + PlugIns/ inside-out.
# =========================================================================
echo "[2/9] Signing Frameworks + PlugIns..."
SIGN_COUNT=0
sign_one() {
    codesign --force --sign "$SIGNING_IDENTITY" \
        --timestamp --options runtime "$1"
    SIGN_COUNT=$((SIGN_COUNT + 1))
}
while IFS= read -r f; do sign_one "$f"; done \
    < <(find "$APP_BUNDLE/Contents/Frameworks" -maxdepth 1 -name "*.dylib" -type f 2>/dev/null)
while IFS= read -r f; do sign_one "$f"; done \
    < <(find "$APP_BUNDLE/Contents/Frameworks" -maxdepth 1 -name "*.framework" -type d 2>/dev/null)
while IFS= read -r f; do sign_one "$f"; done \
    < <(find "$APP_BUNDLE/Contents/PlugIns" -name "*.dylib" -type f 2>/dev/null)
echo "  Signed $SIGN_COUNT nested items"

# =========================================================================
# Step 3: sign the bundle with entitlements + hardened runtime
# =========================================================================
echo "[3/9] Signing app bundle..."
codesign --force --sign "$SIGNING_IDENTITY" \
    --timestamp \
    --options runtime \
    --entitlements "$ENTITLEMENTS" \
    "$APP_BUNDLE"

# =========================================================================
# Step 4: verify signature
# =========================================================================
echo "[4/9] Verifying signature..."
codesign --verify --deep --strict --verbose=2 "$APP_BUNDLE" 2>&1 | tail -3
echo "  Signature valid"

# =========================================================================
# notarize_artifact <path> — submit, --wait, parse status, dump log on fail.
# =========================================================================
notarize_artifact() {
    local ARTIFACT="$1"
    local OUT
    OUT="$(xcrun notarytool submit "$ARTIFACT" \
                 --keychain-profile "$NOTARY_PROFILE" \
                 --wait \
                 --output-format plist 2>&1)"
    echo "$OUT" | tail -20

    local SID STATUS
    SID="$(echo "$OUT" | plutil -extract id raw - 2>/dev/null || echo "")"
    STATUS="$(echo "$OUT" | plutil -extract status raw - 2>/dev/null || echo "Unknown")"

    if [ "$STATUS" != "Accepted" ]; then
        echo "ERROR: notarization failed for $ARTIFACT (status='$STATUS')"
        if [ -n "$SID" ]; then
            echo "Fetching log for $SID..."
            xcrun notarytool log "$SID" \
                --keychain-profile "$NOTARY_PROFILE" 2>&1 | head -40
        fi
        exit 1
    fi
    echo "  Notarized (id=$SID)"
}

# =========================================================================
# Step 5–7: notarize the .app FIRST, then staple it (so it verifies offline
# even after the user drags it out of the DMG). notarytool can't take a raw
# .app — ditto-zip it (plain `zip` corrupts bundles).
# =========================================================================
echo "[5/9] Zipping .app for notarization..."
APP_ZIP="$BUILD_DIR/minnotes.zip"
rm -f "$APP_ZIP"
ditto -c -k --keepParent "$APP_BUNDLE" "$APP_ZIP"
echo "  Wrote $APP_ZIP ($(du -h "$APP_ZIP" | awk '{print $1}'))"

echo "[6/9] Notarizing .app (this takes a few minutes)..."
notarize_artifact "$APP_ZIP"
rm -f "$APP_ZIP"

echo "[7/9] Stapling notarization ticket to .app..."
xcrun stapler staple "$APP_BUNDLE"
xcrun stapler validate "$APP_BUNDLE"

# =========================================================================
# Step 8: build DMG from the (stapled) .app, sign + notarize + staple it.
# =========================================================================
echo "[8/9] Creating + signing + notarizing DMG..."
rm -f "$DMG_PATH"
STAGING_DIR="$(mktemp -d)"
cp -R "$APP_BUNDLE" "$STAGING_DIR/"
ln -s /Applications "$STAGING_DIR/Applications"

hdiutil create -volname "minNotes" \
    -srcfolder "$STAGING_DIR" \
    -ov -format UDZO \
    "$DMG_PATH"
rm -rf "$STAGING_DIR"

codesign --force --sign "$SIGNING_IDENTITY" --timestamp "$DMG_PATH"
echo "  Created + signed $DMG_PATH"

notarize_artifact "$DMG_PATH"

xcrun stapler staple "$DMG_PATH"
xcrun stapler validate "$DMG_PATH"

# =========================================================================
# Step 9: Gatekeeper assessment — final go/no-go
# =========================================================================
echo "[9/9] Gatekeeper assessment..."
spctl --assess --type open --context context:primary-signature --verbose "$DMG_PATH" 2>&1 || {
    echo "WARNING: spctl assessment did not pass cleanly"
    exit 1
}

echo ""
echo "=== Done ==="
echo "Signed + notarized DMG: $DMG_PATH"
echo "(.app inside is also stapled — launches offline after drag-to-/Applications.)"
echo ""
echo "Next, to ship the auto-update:"
echo "  1. Create GitHub release tag v$VERSION and upload $DMG_PATH as the"
echo "     asset named minNotes-MacOS.dmg."
echo "  2. scripts/update_appcast.sh   # signs the DMG + adds the appcast item"
echo "  3. Commit + push docs/appcast.xml (publishes at minnotes.app/appcast.xml)."
