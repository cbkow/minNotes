# minNotes build recipes.

# Configure + build the dev (RelWithDebInfo) preset.
build:
    cmake --preset=debug
    cmake --build --preset=debug --parallel

# Configure + build the release preset.
release:
    cmake --preset=release
    cmake --build --preset=release --parallel

# Run the dev binary. MACOSX_BUNDLE makes the target a .app; run the inner
# executable directly (not `open`) so stdout/stderr land in this terminal.
run:
    ./build/debug/app/minNotes.app/Contents/MacOS/minNotes

# Remove build artifacts.
clean:
    rm -rf build
