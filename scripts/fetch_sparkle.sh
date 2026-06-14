#!/usr/bin/env bash
# Fetch the prebuilt Sparkle.framework + release tools into external/Sparkle/.
#
# Sparkle is vendored (not built): we download the official notarized release
# tarball, verify its SHA-256, and extract the framework + bin/ tools. Like the
# rest of external/, the result is gitignored — run this once on a fresh
# checkout before building the macOS app.
#
# The framework ships pre-signed by the Sparkle project; sign-and-notarize.sh
# re-signs it (and its nested XPC services / Autoupdate / Updater.app) with our
# Developer ID. bin/generate_keys and bin/sign_update are used for the EdDSA
# update-signing key and per-release appcast signatures.
set -euo pipefail

SPARKLE_VERSION="2.9.2"
SPARKLE_SHA256="1cb340cbbef04c6c0d162078610c25e2221031d794a3449d89f2f56f4df77c95"
SPARKLE_URL="https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$REPO_ROOT/external/Sparkle"
TARBALL="$(mktemp -t sparkle.XXXXXX).tar.xz"
EXTRACT="$(mktemp -d -t sparkle_extract.XXXXXX)"
trap 'rm -rf "$TARBALL" "$EXTRACT"' EXIT

if [ -d "$DEST/Sparkle.framework" ]; then
    echo "[sparkle] external/Sparkle/Sparkle.framework already present — skipping."
    echo "[sparkle] (delete external/Sparkle to force a re-fetch.)"
    exit 0
fi

echo "[sparkle] Downloading Sparkle ${SPARKLE_VERSION}..."
curl -fsSL --max-time 180 -o "$TARBALL" "$SPARKLE_URL"

echo "[sparkle] Verifying SHA-256..."
ACTUAL="$(shasum -a 256 "$TARBALL" | awk '{print $1}')"
if [ "$ACTUAL" != "$SPARKLE_SHA256" ]; then
    echo "[sparkle] ERROR: checksum mismatch" >&2
    echo "  expected $SPARKLE_SHA256" >&2
    echo "  actual   $ACTUAL" >&2
    exit 1
fi

echo "[sparkle] Extracting..."
tar -xJf "$TARBALL" -C "$EXTRACT"

mkdir -p "$DEST"
cp -R "$EXTRACT/Sparkle.framework" "$DEST/"
cp -R "$EXTRACT/bin" "$DEST/"
cp "$EXTRACT/LICENSE" "$DEST/LICENSE"

echo "[sparkle] Installed → $DEST"
echo "[sparkle]   Sparkle.framework + bin/{generate_keys,sign_update,...}"
