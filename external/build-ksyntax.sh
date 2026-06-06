#!/usr/bin/env bash
# Build KSyntaxHighlighting (KDE) + its ECM dependency from source into
# external/kf6, which the root CMakeLists find_package()s. Run once after clone.
# Requires: git, cmake, ninja, and the Qt 6.11 install used by the project.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PFX="$HERE/kf6"
SRC="$HERE/src"
QT="${QT_PREFIX:-$HOME/Qt/6.11.1/macos}"
TAG="${KF_TAG:-v6.14.0}"   # 6.14 fixes a UDL clash with Qt 6.11
rm -rf "$PFX" "$SRC"; mkdir -p "$PFX" "$SRC"; cd "$SRC"
git clone --depth 1 --branch "$TAG" https://invent.kde.org/frameworks/extra-cmake-modules.git ecm
cmake -S ecm -B ecm-build -G Ninja -DCMAKE_INSTALL_PREFIX="$PFX" -DBUILD_TESTING=OFF
cmake --build ecm-build --target install
git clone --depth 1 --branch "$TAG" https://invent.kde.org/frameworks/syntax-highlighting.git ksynt
cmake -S ksynt -B ksynt-build -G Ninja -DCMAKE_INSTALL_PREFIX="$PFX" \
  -DCMAKE_PREFIX_PATH="$PFX;$QT" -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build ksynt-build --parallel
cmake --build ksynt-build --target install
echo "KSyntaxHighlighting installed to $PFX"
