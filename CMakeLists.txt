cmake_minimum_required(VERSION 3.24)

# Deployment floor — minNotes targets macOS 13.0+ (same as QCView). MUST be set
# BEFORE project() so the compiler/linker pick it up for every target. Test for
# emptiness (not just DEFINED): CMake/Qt seed an empty cache entry, so a plain
# `NOT DEFINED` guard would skip and the SDK default (host version) would win.
# FORCE overwrites that empty entry.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(MinNotes
    VERSION 0.1.2
    DESCRIPTION "minNotes — block-based markdown notes editor"
    LANGUAGES CXX
)

# Auto-update signing: the Ed25519 PUBLIC key the updater verifies releases
# against (Sparkle on macOS via Info.plist SUPublicEDKey; WinSparkle on Windows
# via win_sparkle_set_eddsa_public_key — compiled into the Windows updater). The
# matching PRIVATE key lives only in the macOS login Keychain (+ an out-of-repo
# backup) and signs every release; it is NEVER committed. Same key as the sister
# apps so one keychain key signs them all. Override with -D to rotate.
set(MINNOTES_UPDATE_PUBLIC_KEY "+Xlrad3WswxIRTyzBTp8KaTEjyfPO5Ap4llmPdZf3RQ="
    CACHE STRING "Ed25519 public key for auto-update signature verification")

# C++20: ink-stroke-modeler (the studio's stroke smoothing) requires it,
# and QCView builds the same way. Was 17 until 2026-06-11.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The ported video engine (app/player/) carries Objective-C++ sources on macOS
# (frame_handle.mm, zero_copy_bridge_metal.mm — the CVPixelBuffer/Metal zero-copy
# bridge). Enable OBJCXX so .mm files compile.
if(APPLE)
    enable_language(OBJCXX)
endif()

if(NOT DEFINED CMAKE_PREFIX_PATH AND NOT DEFINED Qt6_DIR)
    if(APPLE)
        set(CMAKE_PREFIX_PATH "$ENV{HOME}/Qt/6.11.1/macos" CACHE PATH "Qt install root")
    elseif(WIN32)
        set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64" CACHE PATH "Qt install root")
    endif()
endif()

# Sql for the SQLite-canonical document store (DESIGN.md §3); the rest for the
# block editor (pure C++/Qt — no Rust/cxx-qt; the virtualization spike proved
# the whole stack in C++ and the central QAbstractListModel is native here).
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Sql
    ShaderTools       # qt_add_shaders -> .qsb (the video YUV->RGB shaders)
    GuiPrivate        # rhi/qrhi.h for the VideoSurfaceItem QQuickRhiItem
    Pdf               # QPdfDocument probe + QtQuick.Pdf inline/scroll viewers
    Network)          # download remote <img> from pasted HTML into the sidecar
qt_standard_project_setup(REQUIRES 6.11)

# Code-block syntax highlighting: KSyntaxHighlighting (KDE) — a QSyntaxHighlighter
# over QTextDocument, the same mechanism the inline highlighter uses. Built once
# from source into external/kf6 (gitignored); see external/build-ksyntax.sh.
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/external/kf6")
find_package(KF6SyntaxHighlighting 6.0 REQUIRED)

# ---- ink-stroke-modeler (video-annotation stroke smoothing) --------
# Google's real-time stroke smoothing — the studio's freehand strokes are
# modeled at capture time so they render identically here and in QCView
# (PLAN-video-annotations.md; modeled points are baked into the shared
# sidecar JSON). Same pinned commit as QCView-Player (tip of main
# 2026-03-10; the project doesn't tag releases). Pulls Abseil transitively.
# First configure needs network; the checkout lands in build/_deps.
include(FetchContent)
FetchContent_Declare(
    ink_stroke_modeler
    GIT_REPOSITORY https://github.com/google/ink-stroke-modeler.git
    GIT_TAG da42d439389c90ec7574f0381ec53e7f5be0c2eb
    GIT_SHALLOW FALSE
)
set(INK_STROKE_MODELER_BUILD_TESTING     OFF CACHE BOOL "" FORCE)
set(INK_STROKE_MODELER_FIND_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(INK_STROKE_MODELER_ENABLE_INSTALL    OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL                  OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST                        OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ink_stroke_modeler)
message(STATUS "ink-stroke-modeler: pinned commit da42d439")

# ---- ffmpeg (vendored prebuilt) ------------------------------------
# The ported video engine (app/player/, from ufb) links FFmpeg for demux +
# hardware-accelerated decode (VideoToolbox on macOS; Vulkan/D3D11 on Windows).
# Binaries are vendored under external/ffmpeg (gitignored); they are NOT built
# here — copy/build them per platform (macOS dylibs come from ufb's prebuilt
# set; Windows uses the .lib/.dll set). The `minnotes::ffmpeg` INTERFACE target
# aggregates the per-library imports so app/CMakeLists just links one name.
set(MINNOTES_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/external/ffmpeg")
set(_mn_ffmpeg_libs avcodec avformat avutil avfilter swscale swresample)
if(WIN32 AND EXISTS "${MINNOTES_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    # Windows: import libraries (.lib) for link, DLLs for runtime. The DLL
    # filename encodes the MAJOR version (avcodec-62.dll), which drifts per
    # release — glob the bin/ DLL rather than hardcoding the number.
    add_library(minnotes::ffmpeg INTERFACE IMPORTED)
    target_include_directories(minnotes::ffmpeg
        INTERFACE "${MINNOTES_FFMPEG_ROOT}/include")
    foreach(_lib IN LISTS _mn_ffmpeg_libs)
        add_library(minnotes::${_lib} SHARED IMPORTED)
        file(GLOB _dll_match "${MINNOTES_FFMPEG_ROOT}/bin/${_lib}-[0-9]*.dll")
        list(GET _dll_match 0 _dll)
        set_target_properties(minnotes::${_lib} PROPERTIES
            IMPORTED_IMPLIB "${MINNOTES_FFMPEG_ROOT}/lib/${_lib}.lib"
            IMPORTED_LOCATION "${_dll}"
            INTERFACE_INCLUDE_DIRECTORIES "${MINNOTES_FFMPEG_ROOT}/include")
        target_link_libraries(minnotes::ffmpeg INTERFACE minnotes::${_lib})
    endforeach()
    message(STATUS "minnotes: ffmpeg (Windows) found at ${MINNOTES_FFMPEG_ROOT}")
elseif(APPLE AND EXISTS "${MINNOTES_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    # Darwin: shared dylibs named lib<name>.<MAJOR>.dylib. The MAJOR differs per
    # release; glob to avoid hardcoding numbers that drift. install_name is
    # @rpath/... — app/CMakeLists adds external/ffmpeg/lib to the target rpath.
    add_library(minnotes::ffmpeg INTERFACE IMPORTED)
    target_include_directories(minnotes::ffmpeg
        INTERFACE "${MINNOTES_FFMPEG_ROOT}/include")
    foreach(_lib IN LISTS _mn_ffmpeg_libs)
        file(GLOB _dylib_match "${MINNOTES_FFMPEG_ROOT}/lib/lib${_lib}.[0-9]*.dylib")
        list(LENGTH _dylib_match _match_count)
        if(_match_count GREATER 0)
            list(GET _dylib_match 0 _dylib)
            add_library(minnotes::${_lib} SHARED IMPORTED)
            set_target_properties(minnotes::${_lib} PROPERTIES
                IMPORTED_LOCATION "${_dylib}"
                INTERFACE_INCLUDE_DIRECTORIES "${MINNOTES_FFMPEG_ROOT}/include")
            target_link_libraries(minnotes::ffmpeg INTERFACE minnotes::${_lib})
        else()
            message(WARNING "minnotes::${_lib}: no darwin dylib in "
                "${MINNOTES_FFMPEG_ROOT}/lib/ (expected lib${_lib}.<MAJOR>.dylib)")
        endif()
    endforeach()
    message(STATUS "minnotes: ffmpeg (darwin) found at ${MINNOTES_FFMPEG_ROOT}")
else()
    message(FATAL_ERROR "minnotes: ffmpeg NOT found at ${MINNOTES_FFMPEG_ROOT} "
        "— the video engine (app/player/) needs it. Vendor the prebuilt binaries there.")
endif()

add_subdirectory(app)

# Opt-in headless regression harness for the BlockModel document-state fixes.
# Off by default so normal/release builds stay lean; enable with
# -DMINNOTES_BUILD_TESTS=ON, then build + run the mn_regression_test target.
option(MINNOTES_BUILD_TESTS "Build the headless regression test" OFF)
if(MINNOTES_BUILD_TESTS)
    add_subdirectory(tests)
endif()
