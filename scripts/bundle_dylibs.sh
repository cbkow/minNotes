#!/bin/bash
set -euo pipefail

# Recursively bundle the non-system dylibs and frameworks that qcview
# (and its transitive deps) reference into Contents/Frameworks/.
# Resolves @rpath against LC_RPATH entries + fallback search paths,
# handles both .dylib files and .framework directories, then rewrites
# every install name + cross-ref so the .app is self-contained.
#
# Intended to run AFTER macdeployqt (which handles Qt frameworks +
# plugins + QML modules properly). This script is the safety net for
# everything macdeployqt doesn't touch — primarily the vendored FFmpeg
# dylibs in external/ffmpeg/lib (libav*/libsw*) and KSyntaxHighlighting
# in external/kf6/lib, plus any /opt/homebrew leftovers they pull in.
#
# Also performs a minos-rewrite pass at the end so any bundled dylib
# whose LC_BUILD_VERSION says minos > deployment target gets patched
# down. Homebrew commonly builds with the build-machine's OS as minos,
# which would otherwise make dyld refuse to load on older macOS even
# when the actual code uses no APIs newer than our deployment target.
#
# Usage: bundle_dylibs.sh <app_bundle>
#
# Idempotent: safe to re-run on an already-bundled app.

APP_BUNDLE="${1:-}"
if [ -z "$APP_BUNDLE" ] || [ ! -d "$APP_BUNDLE" ]; then
    echo "Usage: bundle_dylibs.sh <app_bundle>"
    exit 1
fi

EXE="$APP_BUNDLE/Contents/MacOS/minNotes"
FRAMEWORKS="$APP_BUNDLE/Contents/Frameworks"
[ -f "$EXE" ] || { echo "ERROR: executable missing at $EXE"; exit 1; }
mkdir -p "$FRAMEWORKS"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Resolved absolute-path prefixes we'll copy into Frameworks/.
# Qt online installer (/Users/chris/Qt) is intentionally excluded —
# macdeployqt handles those.
should_bundle() {
    case "$1" in
        /opt/homebrew/*|/usr/local/*) return 0 ;;
        "$REPO_ROOT"/external/ffmpeg/*) return 0 ;;   # vendored FFmpeg dylibs
        "$REPO_ROOT"/external/kf6/*)    return 0 ;;   # KSyntaxHighlighting
        *) return 1 ;;
    esac
}

# Fallback search paths for @rpath resolution. macdeployqt strips
# per-machine LC_RPATH entries from the binary, so a bare @rpath/foo
# wouldn't resolve via the binary's rpath list alone after that runs.
RPATH_FALLBACK=(
    "$REPO_ROOT/external/ffmpeg/lib"
    "$REPO_ROOT/external/kf6/lib"
    "/opt/homebrew/lib"
)

# LC_RPATH path values from a Mach-O file.
rpaths_of() {
    otool -l "$1" 2>/dev/null | awk '
        /LC_RPATH/{f=1; next}
        f && $1=="path"{print $2; f=0}
    '
}

# LC_LOAD_DYLIB list. Skips the file's own ID line that otool prints
# first when called on a dylib.
deps_of() {
    otool -L "$1" 2>/dev/null | tail -n +2 | awk '{print $1}' \
        | grep -v '^$' | grep -v "^$1:$" || true
}

# Given a dep string + the file that links it, return the absolute
# on-disk path. Empty stdout means unresolvable. Always returns 0 so
# `resolved=$(resolve_dep ...)` doesn't trip `set -e` on lookups that
# legitimately miss (e.g., system frameworks live in the dyld shared
# cache, not on disk).
resolve_dep() {
    local dep="$1"
    local requester="$2"
    local req_dir
    req_dir="$(dirname "$requester")"

    case "$dep" in
        @rpath/*)
            local suffix="${dep#@rpath/}"
            local rp
            while IFS= read -r rp; do
                [ -z "$rp" ] && continue
                rp="${rp//@executable_path/$req_dir}"
                rp="${rp//@loader_path/$req_dir}"
                if [ -e "$rp/$suffix" ]; then echo "$rp/$suffix"; return 0; fi
            done < <(rpaths_of "$requester")
            for rp in "${RPATH_FALLBACK[@]}"; do
                if [ -e "$rp/$suffix" ]; then echo "$rp/$suffix"; return 0; fi
            done
            ;;
        @executable_path/*)
            local p="${dep#@executable_path/}"
            if [ -e "$req_dir/$p" ]; then echo "$req_dir/$p"; fi
            ;;
        @loader_path/*)
            local p="${dep#@loader_path/}"
            if [ -e "$req_dir/$p" ]; then echo "$req_dir/$p"; fi
            ;;
        /*)
            if [ -e "$dep" ]; then echo "$dep"; fi
            ;;
    esac
    return 0
}

# Source root to copy. Framework refs like
#   /foo/Baz.framework/Versions/A/Baz  → /foo/Baz.framework
# Plain dylibs stay as-is.
copy_root_of() {
    case "$1" in
        */*.framework/*) echo "${1%.framework/*}.framework" ;;
        *) echo "$1" ;;
    esac
}

# Given a copy destination + the original dep, return the Mach-O file
# inside it (for further dep walking). Frameworks: <root>/Versions/X/Foo.
target_macho_of() {
    local root="$1"
    local original="$2"
    case "$original" in
        */*.framework/*)
            local rel="${original#*.framework/}"
            echo "$root/$rel"
            ;;
        *) echo "$root" ;;
    esac
}

# All Mach-O files we've bundled — top-level .dylibs + framework
# executables (skip Resources/, Headers/, Helpers/ binaries that are
# just symlinks or non-Mach-O assets).
bundled_machos() {
    for d in "$FRAMEWORKS"/*.dylib; do
        # Real files only — skip the major-soname alias symlinks (e.g.
        # libavutil.59.dylib -> libavutil.59.39.100.dylib). install_name_tool /
        # vtool must touch each real image exactly once, not through an alias.
        [ -f "$d" ] && [ ! -L "$d" ] && echo "$d"
    done
    # Sparkle.framework is vendored prebuilt + notarized by the Sparkle
    # project, already uses @rpath ids and a 10.13 minos. It is EXCLUDED
    # here: install_name_tool / vtool would rewrite (and thus corrupt) its
    # signed binaries + nested XPC/Autoupdate/Updater helpers. It's signed
    # as-is, inside-out, by sign-and-notarize.sh.
    find "$FRAMEWORKS" -type f -path "*.framework/Versions/*" \
        ! -path "*/Resources/*" ! -path "*/Headers/*" \
        ! -path "*/Helpers/*" ! -path "*/Sparkle.framework/*" 2>/dev/null \
        | while IFS= read -r f; do
            file -b "$f" 2>/dev/null | grep -q "Mach-O" && echo "$f"
        done
}

# =========================================================================
# Walk: collect everything we need to bundle, copying it in as we go.
#
# Seeds the queue with the executable AND every Mach-O macdeployqt may
# have already placed in Frameworks/. Without that seed, deps of pre-
# existing libs get missed — e.g. libwebp arrives via macdeployqt with a
# transitive dep on libsharpyuv, but the walker would skip libwebp
# because Frameworks/libwebp.7.dylib already exists, and libsharpyuv
# would never be discovered. Result: dyld fails to launch.
#
# A visited-set prevents revisiting (macOS bash 3.2 has no associative
# arrays; we use a single-line word-set + case-match dedup).
# =========================================================================
echo "[bundle] Walking dependency tree from $EXE"

visited=" "
is_visited() {
    case "$visited" in *" $1 "*) return 0;; *) return 1;; esac
}

queue=("$EXE")
while IFS= read -r m; do
    [ -e "$m" ] && queue+=("$m")
done < <(bundled_machos)

while [ ${#queue[@]} -gt 0 ]; do
    file="${queue[0]}"
    queue=("${queue[@]:1}")
    [ -e "$file" ] || continue
    is_visited "$file" && continue
    visited="$visited$file "
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        resolved="$(resolve_dep "$dep" "$file")"
        [ -z "$resolved" ] && continue
        should_bundle "$resolved" || continue
        src_root="$(copy_root_of "$resolved")"
        base="$(basename "$src_root")"
        dst_root="$FRAMEWORKS/$base"
        if [ -d "$src_root" ]; then
            # Framework: preserve internal Versions/Current symlinks.
            if [ ! -e "$dst_root" ]; then
                cp -R "$src_root" "$dst_root"
                chmod -R u+w "$dst_root"
                echo "[bundle]   + $base"
            fi
        else
            # Dylib. FFmpeg-style trees ship libX.MAJOR.dylib as a SYMLINK to
            # the real libX.MAJOR.MINOR.PATCH.dylib, and different libraries
            # reference one or the other name (the exe links the full version;
            # the libs link each other by the major soname). Dereferencing each
            # name with `cp -L` would produce TWO real files with identical code
            # but distinct LC_IDs → dyld's two-level namespace binds symbols
            # against the wrong copy and aborts at launch (Symbol not found:
            # _av_opt_set_dict). Instead: copy the REAL file once under its REAL
            # basename, and materialize the requested alias as a symlink — so
            # every referenced name resolves to the SAME single image.
            real="$(cd "$(dirname "$src_root")" && readlink -f "$(basename "$src_root")")"
            real_base="$(basename "$real")"
            if [ ! -e "$FRAMEWORKS/$real_base" ]; then
                cp "$real" "$FRAMEWORKS/$real_base"
                chmod u+w "$FRAMEWORKS/$real_base"
                echo "[bundle]   + $real_base"
            fi
            if [ "$base" != "$real_base" ] && [ ! -e "$dst_root" ]; then
                ln -s "$real_base" "$dst_root"     # alias → real file (one image)
                echo "[bundle]   + $base -> $real_base (symlink)"
            fi
            dst_root="$FRAMEWORKS/$real_base"        # walk the real file
        fi
        # Always walk the in-bundle target, whether we just copied it or
        # macdeployqt did. Visited-set keeps us out of cycles.
        queue+=("$(target_macho_of "$dst_root" "$dep")")
    done < <(deps_of "$file")
done

# codesign refuses xattrs (com.apple.FinderInfo / com.apple.provenance)
xattr -cr "$FRAMEWORKS"

# =========================================================================
# Normalize duplicate dylib aliases (symlink → canonical).
#
# A vendored lib's internal cross-refs use the MAJOR soname
# (@rpath/libavcodec.61.dylib), but the vendored build's baked rpath no longer
# resolves at deploy time — so macdeployqt falls back to Homebrew and copies a
# SECOND, incompatible libavcodec.61.dylib (universal x86_64+arm64,
# @loader_path deps) next to our vendored arm64 libavcodec.61.19.100.dylib.
# Two images with the same code but different ids/ABI break dyld's two-level
# namespace → "Symbol not found: _av_opt_set_dict" at launch.
#
# Collapse each family: the fullest-versioned real file is canonical (our
# vendored full-version name always beats Homebrew's bare major soname); any
# shorter-named real duplicate is deleted and replaced with a symlink to it,
# so every @rpath/libX.* name resolves to exactly ONE image.
echo "[bundle] Normalizing duplicate dylib aliases (symlink → canonical)..."
for real in "$FRAMEWORKS"/*.dylib; do
    [ -f "$real" ] && [ ! -L "$real" ] || continue
    rb="$(basename "$real")"
    stem="${rb%%.*}"                       # libavcodec.61.19.100.dylib -> libavcodec
    canon="$rb"
    for cand in "$FRAMEWORKS/$stem".*.dylib; do
        [ -f "$cand" ] && [ ! -L "$cand" ] || continue
        cb="$(basename "$cand")"
        [ ${#cb} -gt ${#canon} ] && canon="$cb"
    done
    if [ "$rb" != "$canon" ]; then
        rm -f "$FRAMEWORKS/$rb"
        ln -s "$canon" "$FRAMEWORKS/$rb"
        echo "[bundle]   $rb -> $canon (collapsed duplicate to symlink)"
    fi
done

# =========================================================================
# Rewrite: every bundled Mach-O's ID + cross-refs → @rpath/<bundle-name>.
# =========================================================================
rewrite_dep() {
    local macho="$1"
    local dep="$2"
    case "$dep" in
        /*)
            should_bundle "$dep" || return 0
            local src_root base
            src_root="$(copy_root_of "$dep")"
            base="$(basename "$src_root")"
            case "$dep" in
                */*.framework/*)
                    local suffix="${dep#*.framework/}"
                    install_name_tool -change "$dep" \
                        "@rpath/$base/$suffix" "$macho" 2>/dev/null || true
                    ;;
                *)
                    install_name_tool -change "$dep" \
                        "@rpath/$base" "$macho" 2>/dev/null || true
                    ;;
            esac
            ;;
    esac
}

echo "[bundle] Rewriting install names..."
while IFS= read -r macho; do
    [ -e "$macho" ] || continue
    rel_from_fw="${macho#$FRAMEWORKS/}"
    install_name_tool -id "@rpath/$rel_from_fw" "$macho" 2>/dev/null || true
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        rewrite_dep "$macho" "$dep"
    done < <(deps_of "$macho")
done < <(bundled_machos)

echo "[bundle] Rewriting executable references..."
while IFS= read -r dep; do
    [ -z "$dep" ] && continue
    rewrite_dep "$EXE" "$dep"
done < <(deps_of "$EXE")

# Ensure @executable_path/../Frameworks rpath is present.
if ! rpaths_of "$EXE" | grep -q "^@executable_path/../Frameworks$"; then
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$EXE"
    echo "[bundle] Added LC_RPATH @executable_path/../Frameworks"
fi

# =========================================================================
# minos rewrite: any bundled Mach-O whose LC_BUILD_VERSION minos exceeds
# our deployment target gets patched down with vtool. Homebrew builds X11
# / libxcb / libXau / libXdmcp etc. with the build-machine OS as minos
# even though their code uses no APIs newer than libSystem. dyld checks
# minos at load time, so without this rewrite the .app fails to launch on
# macOS < build-machine OS.
#
# Safe because: codesign re-seals everything after bundle_dylibs.sh, and
# we only touch LC_BUILD_VERSION metadata — not any code. If a dylib
# genuinely uses an API newer than the deployment target, it traps at
# the call site regardless of the minos tag.
#
# Must match CMAKE_OSX_DEPLOYMENT_TARGET in the root CMakeLists.txt.
# =========================================================================
DEPLOYMENT_TARGET="13.0"
echo "[bundle] Rewriting LC_BUILD_VERSION minos -> $DEPLOYMENT_TARGET..."
while IFS= read -r macho; do
    [ -e "$macho" ] || continue
    current_minos="$(otool -l "$macho" 2>/dev/null \
        | awk '/LC_BUILD_VERSION/{f=1} f && /minos/{print $2; exit}')"
    [ -z "$current_minos" ] && continue
    needs_rewrite="$(awk -v a="$current_minos" -v b="$DEPLOYMENT_TARGET" \
        'BEGIN{print (a+0 > b+0) ? "yes" : "no"}')"
    if [ "$needs_rewrite" = "yes" ]; then
        current_sdk="$(otool -l "$macho" 2>/dev/null \
            | awk '/LC_BUILD_VERSION/{f=1} f && /sdk/{print $2; exit}')"
        [ -z "$current_sdk" ] && current_sdk="$DEPLOYMENT_TARGET"
        tmp="$macho.minospatched"
        xcrun vtool -set-build-version macos \
            "$DEPLOYMENT_TARGET" "$current_sdk" \
            -replace -output "$tmp" "$macho" >/dev/null
        # Preserve executable bit on the patched file.
        chmod u+wx "$tmp"
        mv "$tmp" "$macho"
        echo "[bundle]   ${macho#$FRAMEWORKS/}: minos $current_minos -> $DEPLOYMENT_TARGET"
    fi
done < <(bundled_machos)

# Sanity: any leftover absolute references that shouldn't be there.
LEFTOVERS="$(
    {
        otool -L "$EXE" | tail -n +2 | awk '{print $1}'
        while IFS= read -r m; do
            otool -L "$m" | tail -n +2 | awk '{print $1}'
        done < <(bundled_machos)
    } | grep -E '^(/opt/homebrew|/usr/local|/Users/)' | sort -u || true
)"
if [ -n "$LEFTOVERS" ]; then
    echo "[bundle] WARNING: leftover absolute references —"
    echo "$LEFTOVERS" | sed 's/^/    /'
    exit 1
fi

# Final report.
DYLIB_COUNT="$(find "$FRAMEWORKS" -maxdepth 1 -name "*.dylib" -type f 2>/dev/null | wc -l | tr -d ' ')"
FW_COUNT="$(find "$FRAMEWORKS" -maxdepth 1 -name "*.framework" -type d 2>/dev/null | wc -l | tr -d ' ')"
SIZE="$(du -sh "$FRAMEWORKS" 2>/dev/null | awk '{print $1}')"
echo "[bundle] Done — $DYLIB_COUNT dylibs + $FW_COUNT frameworks, $SIZE in $FRAMEWORKS"
