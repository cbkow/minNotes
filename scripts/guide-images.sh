#!/usr/bin/env bash
# The User Guide's screenshots (2026-10-22): the docs images the guide's sections reference, downscaled to at most
# 1600 px on the long side into app/guide/images/ (bundled as Qt resources; the site keeps the originals). Run after
# a docs screenshot changes:  scripts/guide-images.sh   (macOS: sips).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/app/guide/images"
mkdir -p "$OUT"
for f in basics canvas editing tables media annotations spelling import export packages shortcuts; do
    grep -o '!\[[^]]*\]([^)]*)' "$ROOT/docs/$f.md" | sed 's/.*(\(.*\))/\1/' || true   # (a section without images is fine)
done | sort -u | while read -r rel; do
    src="$ROOT/docs/$rel"; [ -f "$src" ] || { echo "missing $rel"; continue; }
    dst="$OUT/$(basename "$rel")"
    cp "$src" "$dst"
    sips -Z 1600 "$dst" >/dev/null
    echo "$(basename "$rel") $(stat -f %z "$dst")"
done
