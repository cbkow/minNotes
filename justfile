# minNotes build recipes.

# Configure + build the dev (RelWithDebInfo) preset.
build:
    cmake --preset=debug
    cmake --build --preset=debug --parallel

# Configure + build the release preset.
release:
    cmake --preset=release
    cmake --build --preset=release --parallel

# Run the dev binary.
run:
    ./build/debug/app/minnotesapp

# Remove build artifacts.
clean:
    rm -rf build
