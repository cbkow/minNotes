# Executable target is `minnotesapp`, NOT `minnotes`: on case-insensitive
# APFS the latter collides with the `MinNotes` QML-module output directory
# (ld: "Is a directory"). The app title and URI stay "minNotes".
qt_add_executable(minnotesapp
    main.cpp
    MinNotesApplication.h
    MinNotesApplication.cpp
    AppUpdater.h                 # Q_OBJECT — Sparkle "Check for Updates" bridge
    sparkle_updater_macos.h
    core/FenwickTree.h
    core/BlockModel.h
    core/BlockModel.cpp
    core/DocumentManager.h
    core/DocumentManager.cpp
    core/PathMap.h
    core/PathMap.cpp
    core/PathMapController.h
    core/PathMapController.cpp
    core/Document.h
    core/Document.cpp
    core/Exporter.h
    core/Exporter.cpp
    core/InlineMarkdownHighlighter.h
    core/InlineMarkdownHighlighter.cpp
    core/CodeHighlighter.h
    core/CodeHighlighter.cpp
    core/CodeSyntax.h
    core/CodeSyntax.cpp
    core/TableGrid.h
    core/TableGrid.cpp
    core/Clipboard.h
    core/Clipboard.cpp
    core/MediaStore.h
    core/MediaStore.cpp
    core/VideoFrameProvider.h
    # Video-note sidecar (QCView interop — PLAN-video-annotations.md).
    # annotation_note/annotation_io are VERBATIM QCView ports (the on-disk
    # contract); VideoNotesModel is the QML_ELEMENT adapter, so it lives in
    # the executable target for qmltyperegistrar.
    notes/annotation_note.h
    notes/annotation_note.cpp
    notes/annotation_io.h
    notes/annotation_io.cpp
    notes/VideoNotesModel.h
    notes/VideoNotesModel.cpp
    # Stroke engine (QCView ports + the studio's QPainter overlay).
    notes/active_stroke.h
    notes/ink_stroke_modeler_wrapper.h
    notes/ink_stroke_modeler_wrapper.cpp
    notes/annotation_serializer.h
    notes/annotation_serializer.cpp
    notes/annotation_thumbnail.h
    notes/annotation_thumbnail.cpp
    notes/viewport_annotator.h
    notes/viewport_annotator.cpp
    notes/VideoAnnotator.h
    notes/VideoAnnotator.cpp
    notes/SketchCanvas.h
    notes/SketchCanvas.cpp
    notes/doc_ink.h
    notes/doc_ink.cpp
    notes/DocInkCanvas.h
    notes/DocInkCanvas.cpp
    # Video engine (ported from ufb; audio omitted for v1). Cross-platform
    # core lives here; the platform hwaccel/zero-copy sources are added per-OS
    # below. VideoDecoder + VideoSurfaceItem are QML_ELEMENT types, so they must
    # be in the qt_add_executable target for qmltyperegistrar to see them.
    player/video_decoder.h
    player/video_decoder.cpp
    player/VideoSurfaceItem.h
    player/VideoSurfaceItem.cpp
    player/frame_handle.h
    player/frame_index.h
    player/frame_index.cpp
    player/scrub_decoder.h
    player/scrub_decoder.cpp
    player/scrub_frame_cache.h
    player/scrub_frame_cache.cpp
    player/simple_lru.h
    player/decoder_cleanup_queue.h
    player/decoder_cleanup_queue.cpp
    player/timecode_formatter.h
    player/timecode_formatter.cpp
    # Audio engine (cross-platform core; CoreAudio/WASAPI device sources added
    # per-OS below). AudioPlayer is a QML_ELEMENT, so it lives in the target.
    player/audio_player.h
    player/audio_player.cpp
    player/audio_decoder.h
    player/audio_decoder.cpp
    player/multi_stream_audio_decoder.h
    player/multi_stream_audio_decoder.cpp
    player/audio_ring_buffer.h
    player/audio_routing.h
    player/i_audio_source.h
    # Audio rate engine (ported from QCView fb398ef1): continuous A/V sync
    # servo, constant-pitch review speeds (SoundTouch WSOLA), grain shuttle
    # audio for FF/RW + drag-scrub.
    player/audio_sync_servo.h
    player/fractional_resampler.h
    player/audio_routing_matrix.h
    player/tempo_stage.h
    player/tempo_stage.cpp
    player/audio_chunk_reader.h
    player/audio_chunk_reader.cpp
    player/shuttle_audio_engine.h
    player/shuttle_audio_engine.cpp
)

# Document.cpp stamps doc_meta.app_version with this on save. (The WinSparkle
# block below defines the same value again for its own TU — identical dups are
# harmless.)
target_compile_definitions(minnotesapp PRIVATE
    "MINNOTES_APP_VERSION=\"${PROJECT_VERSION}\"")

# Mark Theme.qml as a QML singleton BEFORE qt_add_qml_module so the generated
# qmldir registers it correctly (else `Theme.colors.x` fails to resolve).
set_source_files_properties(qml/MinNotes/App/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(qml/MinNotes/App/Toasts.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(minnotesapp
    URI MinNotes.App
    VERSION 1.0
    QML_FILES
        qml/MinNotes/App/Main.qml
        qml/MinNotes/App/Editor.qml
        qml/MinNotes/App/Theme.qml
        qml/MinNotes/App/MnScrollBar.qml
        qml/MinNotes/App/ThemedMenu.qml
        qml/MinNotes/App/ThemedMenuSeparator.qml
        qml/MinNotes/App/Icon.qml
        qml/MinNotes/App/FlatButton.qml
        qml/MinNotes/App/FlatToolTip.qml
        qml/MinNotes/App/LeftRail.qml
        qml/MinNotes/App/BottomRail.qml
        qml/MinNotes/App/BlockTable.qml
        qml/MinNotes/App/TableTabs.qml
        qml/MinNotes/App/MediaBlock.qml
        qml/MinNotes/App/FlatSlider.qml
        qml/MinNotes/App/Inspector.qml
        qml/MinNotes/App/ColorPickerInline.qml
        qml/MinNotes/App/ChoicePicker.qml
        qml/MinNotes/App/ChoiceColumnEditor.qml
        qml/MinNotes/App/BlockKanban.qml
        qml/MinNotes/App/VideoTransport.qml
        qml/MinNotes/App/PathMappingsDialog.qml
        qml/MinNotes/App/Toasts.qml
    RESOURCES
        qml/MinNotes/App/PhosphorIcons.js
        qml/MinNotes/App/fonts/Phosphor.ttf
        qml/MinNotes/App/fonts/Phosphor-Fill.ttf
        qml/MinNotes/App/fonts/Phosphor-Duotone.ttf
        qml/MinNotes/App/fonts/Phosphor-Thin.ttf
        qml/MinNotes/App/fonts/Inter_18pt-Regular.ttf
        qml/MinNotes/App/fonts/Inter_18pt-Bold.ttf
        qml/MinNotes/App/fonts/Inter_18pt-Italic.ttf
        qml/MinNotes/App/fonts/Inter_18pt-BoldItalic.ttf
        qml/MinNotes/App/fonts/JetBrainsMono-Regular.ttf
        qml/MinNotes/App/fonts/Aspekta-400.ttf
        qml/MinNotes/App/fonts/Aspekta-700.ttf
        qml/MinNotes/App/fonts/Lora-Regular.ttf
        qml/MinNotes/App/fonts/Lora-Bold.ttf
        qml/MinNotes/App/fonts/Lora-Italic.ttf
        qml/MinNotes/App/fonts/Lora-BoldItalic.ttf
)

# core/ is on the include path too so the generated QML type registration can
# resolve `#include <InlineMarkdownHighlighter.h>` (it emits the basename only).
target_include_directories(minnotesapp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/notes
    ${CMAKE_CURRENT_SOURCE_DIR}/player
    ${CMAKE_CURRENT_SOURCE_DIR}/player/d3d11)

# Video YUV->RGB + passthrough GLSL compiled to .qsb bundles. The resource
# PREFIX is kept as "/ufb" so VideoSurfaceItem.cpp (a verbatim port) finds them
# at ":/ufb/player/shaders/..." — keeping the engine a pure copy makes future
# re-syncs from ufb trivial.
qt_add_shaders(minnotesapp "minnotes_player_shaders"
    PREFIX "/ufb"
    FILES
        player/shaders/fullscreen.vert
        player/shaders/passthrough.frag
        player/shaders/yuv_to_rgb_biplanar.frag
        player/shaders/yuv_to_rgb_ayuv.frag
)

target_link_libraries(minnotesapp PRIVATE
    Qt6::Core
    Qt6::CorePrivate  # qzipwriter_p.h — the DOCX exporter's zip (the QCView recipe)
    Qt6::Gui
    Qt6::GuiPrivate   # rhi/qrhi.h for the VideoSurfaceItem QQuickRhiItem
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
    Qt6::Pdf          # QPdfDocument (probe) + QtQuick.Pdf viewers
    Qt6::Network      # remote <img> download (HTML paste)
    KF6::SyntaxHighlighting
    minnotes::ffmpeg
    # Stroke smoothing (FetchContent, pinned in the root CMakeLists).
    InkStrokeModeler::stroke_modeler
    # WSOLA tempo stage (constant-pitch review speeds). Static, vendored
    # via FetchContent in the root CMakeLists.
    SoundTouch
)

# ink-stroke-modeler doesn't propagate its include dir as a usage
# requirement (same workaround as QCView).
target_include_directories(minnotesapp PRIVATE
    ${CMAKE_BINARY_DIR}/_deps/ink_stroke_modeler-src)

# ---- macOS .app bundle: Info.plist, icons, Sparkle ----------------
# Turns minnotesapp into minnotesapp.app with a custom Info.plist (URI scheme
# minnotes://, the .mndb document type + exported UTI, Sparkle SU* keys), a
# proper Finder/dock icon (legacy .icns for macOS 13–25 + the Tahoe layered
# .icon for 26+), and the vendored Sparkle.framework embedded in Frameworks/.
if(APPLE)
    set_target_properties(minnotesapp PROPERTIES
        MACOSX_BUNDLE TRUE
        # The .app + inner executable are named "minNotes" (the product name)
        # while the CMake target stays "minnotesapp". The target can't be
        # "minNotes" — on case-insensitive APFS that bare link output collides
        # with the "MinNotes" QML-module dir in app/. But OUTPUT_NAME only names
        # the bundle output (app/minNotes.app/Contents/MacOS/minNotes), which
        # lives under the .app and never as a bare app/minNotes — so no clash.
        OUTPUT_NAME "minNotes"
        # rpath: Sparkle (+ any future bundled dylibs) resolve from the app's
        # Frameworks/. BUILD_RPATH also keeps the FFmpeg lib dir so the dev .app
        # launches from the build tree (the DMG step copies FFmpeg into
        # Frameworks/ + restamps @rpath; INSTALL_RPATH is the shipped form).
        INSTALL_RPATH "@executable_path/../Frameworks"
        BUILD_RPATH   "@executable_path/../Frameworks;${MINNOTES_FFMPEG_ROOT}/lib")

    # ---- Legacy raster icon (.icns, macOS 13–25) ----------------------
    # iconutil + sips are part of the macOS toolchain (no extra deps). Source
    # is the committed 1024² transparent master (foreground mark, no baked
    # squircle — macOS 26 applies its own mortise to the layered .icon below).
    # The master is regenerated from external/icons/minNotesMacOS.svg via
    # rsvg-convert + magick (see packaging/macos/README).
    set(MN_ICON_PNG    "${CMAKE_SOURCE_DIR}/packaging/macos/minNotesMacOS.png")
    set(MN_ICONSET_DIR "${CMAKE_BINARY_DIR}/minnotes.iconset")
    set(MN_ICNS_FILE   "${CMAKE_BINARY_DIR}/minnotes.icns")
    add_custom_command(
        OUTPUT  "${MN_ICNS_FILE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MN_ICONSET_DIR}"
        COMMAND sips -z 16   16   "${MN_ICON_PNG}" --out "${MN_ICONSET_DIR}/icon_16x16.png"
        COMMAND sips -z 32   32   "${MN_ICON_PNG}" --out "${MN_ICONSET_DIR}/icon_16x16@2x.png"
        COMMAND sips -z 32   32   "${MN_ICON_PNG}" --out "${MN_ICONSET_DIR}/icon_32x32.png"
        COMMAND sips -z 64   64   "${MN_ICON_PNG}" --out "${MN_ICONSET_DIR}/icon_32x32@2x.png"
        COMMAND sips -z 128  128  "${MN_ICON_PNG}" --out "${MN_ICONSET_DIR}/icon_128x128.png"
        COMMAND sips -z 256  256  "${MN_ICON_PNG}" --out "${MN_ICONSET_DIR}/icon_128x128@2x.png"
        COMMAND sips -z 256  256  "${MN_ICON_PNG}" --out "${MN_ICONSET_DIR}/icon_256x256.png"
        COMMAND sips -z 512  512  "${MN_ICON_PNG}" --out "${MN_ICONSET_DIR}/icon_256x256@2x.png"
        COMMAND sips -z 512  512  "${MN_ICON_PNG}" --out "${MN_ICONSET_DIR}/icon_512x512.png"
        COMMAND sips -z 1024 1024 "${MN_ICON_PNG}" --out "${MN_ICONSET_DIR}/icon_512x512@2x.png"
        COMMAND iconutil -c icns "${MN_ICONSET_DIR}" -o "${MN_ICNS_FILE}"
        DEPENDS "${MN_ICON_PNG}"
        COMMENT "Generating minnotes.icns from ${MN_ICON_PNG}"
        VERBATIM)
    set_source_files_properties("${MN_ICNS_FILE}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(minnotesapp PRIVATE "${MN_ICNS_FILE}")

    # ---- Tahoe layered app icon (.icon → Assets.car, macOS 26+) -------
    # external/icons/minNotesMacOS.icon is the Icon Composer bundle (a
    # gray-gradient fill layer + the foreground mark). actool compiles it to
    # Assets.car; macOS 26 reads CFBundleIconName + Assets.car and applies the
    # system squircle + Liquid Glass. The asset name MUST match --app-icon and
    # CFBundleIconName. Guarded on the asset + actool (full Xcode) so the build
    # stays green without them — older macOS keeps the .icns above.
    set(MN_APPICON_ICON "${CMAKE_SOURCE_DIR}/external/icons/minNotesMacOS.icon")
    set(MN_APPICON_NAME "minNotesMacOS")
    set(MN_ICON_NAME_BLOCK "")
    if(EXISTS "${MN_APPICON_ICON}")
        set(MN_ASSETS_DIR "${CMAKE_BINARY_DIR}/AppIconAssets")
        set(MN_ASSETS_CAR "${MN_ASSETS_DIR}/Assets.car")
        add_custom_command(
            OUTPUT  "${MN_ASSETS_CAR}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${MN_ASSETS_DIR}"
            COMMAND xcrun actool "${MN_APPICON_ICON}"
                    --compile "${MN_ASSETS_DIR}"
                    --output-format human-readable-text
                    --notices --warnings --errors
                    --output-partial-info-plist "${MN_ASSETS_DIR}/partial.plist"
                    --app-icon "${MN_APPICON_NAME}" --include-all-app-icons
                    --enable-on-demand-resources NO
                    --development-region en
                    --target-device mac
                    --minimum-deployment-target 26.0
                    --platform macosx
            DEPENDS "${MN_APPICON_ICON}"
            COMMENT "Compiling ${MN_APPICON_NAME}.icon → Assets.car (actool)"
            VERBATIM)
        set_source_files_properties("${MN_ASSETS_CAR}"
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(minnotesapp PRIVATE "${MN_ASSETS_CAR}")
        # Real newline inside the string so configure_file emits two plist lines.
        set(MN_ICON_NAME_BLOCK
"    <key>CFBundleIconName</key>
    <string>${MN_APPICON_NAME}</string>")
        message(STATUS "minNotes: Tahoe layered icon ${MN_APPICON_NAME}.icon → Assets.car")
    else()
        message(STATUS "minNotes: ${MN_APPICON_ICON} not found — legacy .icns only")
    endif()

    # ---- Info.plist (conditional CFBundleIconName) --------------------
    # configure_file rather than CMake's MACOSX_BUNDLE_* auto-substitution, so
    # CFBundleIconName is injected ONLY when Assets.car ships (else macOS 26
    # would point at a missing asset → blank icon). The template references the
    # bundle vars in ${...} form; @MN_ICON_NAME_BLOCK@ is the @VAR@ form.
    set(MACOSX_BUNDLE_EXECUTABLE_NAME "minNotes")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    configure_file(
        "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
        "${CMAKE_BINARY_DIR}/Info.plist")
    set_target_properties(minnotesapp PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/Info.plist")

    # ---- Sparkle auto-update ------------------------------------------
    # Vendored under external/Sparkle (gitignored; copied from QCView /
    # fetched via the GitHub release). When present, compile the .mm bridge,
    # define MINNOTES_HAVE_SPARKLE, link -framework Sparkle, and ditto the
    # framework into Contents/Frameworks (ditto preserves the Versions symlink
    # layout cmake -E copy_directory would flatten + break signing). When
    # absent the bridge stays a no-op and the build still succeeds (dev builds
    # without update support; the DMG release script ensures it's vendored).
    set(MN_SPARKLE_DIR "${CMAKE_SOURCE_DIR}/external/Sparkle")
    if(EXISTS "${MN_SPARKLE_DIR}/Sparkle.framework")
        target_sources(minnotesapp PRIVATE sparkle_updater_macos.mm)
        target_compile_definitions(minnotesapp PRIVATE MINNOTES_HAVE_SPARKLE)
        # -F at compile time so `#import <Sparkle/Sparkle.h>` resolves, and at
        # link time so `-framework Sparkle` finds it.
        target_compile_options(minnotesapp PRIVATE "-F${MN_SPARKLE_DIR}")
        target_link_options(minnotesapp PRIVATE "-F${MN_SPARKLE_DIR}")
        target_link_libraries(minnotesapp PRIVATE "-framework Sparkle")
        add_custom_command(TARGET minnotesapp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:minnotesapp>/../Frameworks"
            COMMAND ditto
                "${MN_SPARKLE_DIR}/Sparkle.framework"
                "$<TARGET_FILE_DIR:minnotesapp>/../Frameworks/Sparkle.framework"
            COMMENT "Embedding Sparkle.framework into minNotes.app/Contents/Frameworks"
            VERBATIM)
        message(STATUS "minNotes: Sparkle vendored — auto-update enabled")
    else()
        message(STATUS "minNotes: external/Sparkle absent — auto-update disabled (no-op shim)")
    endif()
endif()

# ---- Windows: shipped binary name ---------------------------------
# Name the output "minNotes.exe" (the product name) while the CMake target stays
# "minnotesapp". The target itself can't be "minNotes" — a bare "minNotes" link
# output collides with the "MinNotes" QML-module dir on case-insensitive
# filesystems (same reason the APPLE block above uses OUTPUT_NAME, not a target
# rename). OUTPUT_NAME only renames the binary, so minNotes.exe is safe (and the
# .exe extension keeps it distinct from any MinNotes/ dir regardless).
if(WIN32)
    # WIN32_EXECUTABLE → link the GUI subsystem (WinMain) instead of the console
    # subsystem, so launching minNotes.exe does NOT pop a terminal window. Qt
    # supplies the WinMain entry point via Qt6::EntryPointPrivate (pulled in by
    # qt_add_executable). Trade-off: stdout/stderr go nowhere — use a file logger
    # (see PORTING-NOTES §7) if console logs are needed.
    set_target_properties(minnotesapp PROPERTIES
        OUTPUT_NAME "minNotes"
        WIN32_EXECUTABLE TRUE)

    # ---- App icon + version resource (.rc) ----------------------------
    # The MSVC linker embeds the ICON (Explorer/taskbar/Alt-Tab + Qt's default
    # window icon) AND a VS_VERSION_INFO block (Properties > Details). The .rc is
    # generated from minNotes.rc.in with the project version substituted, so the
    # exe self-reports its version. minNotes.ico comes from
    # external/icons/minNotesWindows.svg via packaging/windows/build-icon.ps1.
    set(MN_RC_ICON_PATH "${CMAKE_SOURCE_DIR}/packaging/windows/minNotes.ico")
    set(MN_VER_MAJOR ${PROJECT_VERSION_MAJOR})
    set(MN_VER_MINOR ${PROJECT_VERSION_MINOR})
    set(MN_VER_PATCH ${PROJECT_VERSION_PATCH})
    configure_file(
        "${CMAKE_SOURCE_DIR}/packaging/windows/minNotes.rc.in"
        "${CMAKE_BINARY_DIR}/minNotes.rc" @ONLY)
    target_sources(minnotesapp PRIVATE "${CMAKE_BINARY_DIR}/minNotes.rc")

    # ---- WinSparkle auto-update --------------------------------------
    # Vendored under external/winsparkle (gitignored; copied from ufb). When
    # present, compile the WinSparkle bridge, define MINNOTES_HAVE_WINSPARKLE +
    # the app version (the updater's compare value) + the shared Ed25519 public
    # key, link the import lib, and copy WinSparkle.dll next to the exe. When
    # absent the bridge stays a no-op (header) and the build still succeeds.
    set(_mn_winsparkle "${CMAKE_SOURCE_DIR}/external/winsparkle")
    if(EXISTS "${_mn_winsparkle}/include/winsparkle.h")
        target_sources(minnotesapp PRIVATE winsparkle_updater_windows.cpp)
        target_compile_definitions(minnotesapp PRIVATE
            MINNOTES_HAVE_WINSPARKLE
            "MINNOTES_APP_VERSION=\"${PROJECT_VERSION}\""
            "MINNOTES_UPDATE_PUBLIC_KEY=\"${MINNOTES_UPDATE_PUBLIC_KEY}\"")
        target_include_directories(minnotesapp PRIVATE "${_mn_winsparkle}/include")
        target_link_libraries(minnotesapp PRIVATE "${_mn_winsparkle}/lib/WinSparkle.lib")
        add_custom_command(TARGET minnotesapp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mn_winsparkle}/bin/WinSparkle.dll"
                "$<TARGET_FILE_DIR:minnotesapp>/WinSparkle.dll"
            COMMENT "Sync WinSparkle.dll next to minNotes.exe" VERBATIM)
        message(STATUS "minNotes: WinSparkle vendored — auto-update enabled")
    else()
        message(STATUS "minNotes: external/winsparkle absent — auto-update disabled (no-op shim)")
    endif()

    # ---- windeployqt: Qt DLLs + plugins + QML modules next to the exe -
    # --qmldir scans the QML imports so the right modules (incl. the Fusion
    # Quick Controls style we select at runtime) get deployed. Flags mirror ufb.
    get_target_property(_mn_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_mn_qt_bin "${_mn_qmake}" DIRECTORY)
    set(_mn_windeployqt "${_mn_qt_bin}/windeployqt.exe")
    if(EXISTS "${_mn_windeployqt}")
        add_custom_command(TARGET minnotesapp POST_BUILD
            COMMAND "${_mn_windeployqt}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                --no-compiler-runtime
                "$<TARGET_FILE:minnotesapp>"
            COMMENT "Running windeployqt (Qt DLLs + plugins + QML modules)"
            VERBATIM)
    else()
        message(WARNING "minNotes: windeployqt not found at ${_mn_windeployqt} "
            "— the build tree won't be self-contained for packaging.")
    endif()

    # ---- Vendored runtime DLLs (FFmpeg + KSyntaxHighlighting) ---------
    # windeployqt only handles Qt; these vendored libs must sit next to the exe
    # for the packaged app to run on a clean machine. KF6SyntaxHighlighting.dll
    # backs code-block highlighting; the FFmpeg DLLs back the video engine.
    set(_mn_extra_dlls "")
    file(GLOB _mn_ffmpeg_dlls "${MINNOTES_FFMPEG_ROOT}/bin/*.dll")
    list(APPEND _mn_extra_dlls ${_mn_ffmpeg_dlls})
    if(EXISTS "${CMAKE_SOURCE_DIR}/external/kf6/bin/KF6SyntaxHighlighting.dll")
        list(APPEND _mn_extra_dlls "${CMAKE_SOURCE_DIR}/external/kf6/bin/KF6SyntaxHighlighting.dll")
    endif()
    foreach(_dll IN LISTS _mn_extra_dlls)
        get_filename_component(_dn "${_dll}" NAME)
        add_custom_command(TARGET minnotesapp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:minnotesapp>/${_dn}"
            COMMENT "Sync ${_dn} next to minNotes.exe" VERBATIM)
    endforeach()
endif()

# ---- platform video/audio backends --------------------------------
if(APPLE)
    # Metal zero-copy bridge + CoreVideo CVPixelBuffer handle (frame_handle.mm),
    # and the CoreAudio output device.
    target_sources(minnotesapp PRIVATE
        player/frame_handle.mm
        player/zero_copy_bridge_metal.h
        player/zero_copy_bridge_metal.mm
        player/coreaudio_device.h
        player/coreaudio_device.mm)
    target_link_libraries(minnotesapp PRIVATE
        "-framework CoreVideo"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework AudioUnit")
    # FFmpeg rpath is set in the .app bundle block above (BUILD_RPATH carries
    # both @executable_path/../Frameworks for Sparkle and the FFmpeg lib dir so
    # the dev .app resolves the @rpath/lib*.dylib install_names from the build
    # tree; the DMG step copies them into Frameworks/).
elseif(WIN32)
    # Windows hwaccel: FFmpeg decodes ProRes into Vulkan VkImages (shared
    # VkDevice), a Vulkan compute YUV->RGB pass composites into a D3D11-shared
    # texture that Qt's QRhi (D3D11) samples. Plus the WASAPI audio device.
    target_sources(minnotesapp PRIVATE
        player/frame_handle_nonapple.cpp
        player/vulkan/vulkan_device_manager.h
        player/vulkan/vulkan_device_manager.cpp
        player/vulkan_hw_device_ctx.h
        player/vulkan_hw_device_ctx.cpp
        player/d3d11/d3d11_device_manager.h
        player/d3d11/d3d11_device_manager.cpp
        player/d3d11/d3d11_vulkan_yuv_compositor.h
        player/d3d11/d3d11_vulkan_yuv_compositor.cpp
        player/d3d11/d3d11_vulkan_decode_bridge.h
        player/d3d11/d3d11_vulkan_decode_bridge.cpp
        player/wasapi_audio_device.h
        player/wasapi_audio_device.cpp)
    find_package(Vulkan REQUIRED)
    target_link_libraries(minnotesapp PRIVATE Vulkan::Vulkan)
    target_compile_definitions(minnotesapp PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    # shaderc (from the Vulkan SDK): the YUV->RGB compositor compiles its GLSL
    # at runtime. d3d11/dxgi/dxguid for the device manager + bridge; avrt for
    # WASAPI MMCSS thread priority.
    target_link_libraries(minnotesapp PRIVATE
        "$ENV{VULKAN_SDK}/Lib/shaderc_combined.lib"
        d3d11 dxgi dxguid avrt)
endif()
