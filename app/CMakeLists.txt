# Executable target is `minnotesapp`, NOT `minnotes`: on case-insensitive
# APFS the latter collides with the `MinNotes` QML-module output directory
# (ld: "Is a directory"). The app title and URI stay "minNotes".
qt_add_executable(minnotesapp
    main.cpp
    core/FenwickTree.h
    core/BlockModel.h
    core/BlockModel.cpp
    core/Document.h
    core/Document.cpp
    core/InlineMarkdownHighlighter.h
    core/InlineMarkdownHighlighter.cpp
    core/CodeHighlighter.h
    core/CodeHighlighter.cpp
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
)

# Mark Theme.qml as a QML singleton BEFORE qt_add_qml_module so the generated
# qmldir registers it correctly (else `Theme.colors.x` fails to resolve).
set_source_files_properties(qml/MinNotes/App/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(minnotesapp
    URI MinNotes.App
    VERSION 1.0
    QML_FILES
        qml/MinNotes/App/Main.qml
        qml/MinNotes/App/Editor.qml
        qml/MinNotes/App/Theme.qml
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
)

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
    # The @rpath/lib*.dylib install_names of the vendored FFmpeg resolve via
    # this build-tree rpath (dev runs the unbundled binary). A future .app
    # bundle step would instead copy them to Contents/Frameworks + @executable_path.
    set_target_properties(minnotesapp PROPERTIES
        BUILD_RPATH "${MINNOTES_FFMPEG_ROOT}/lib")
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
