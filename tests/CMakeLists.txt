# Headless regression harness for the document-state fixes (BlockModel layer).
# Opt-in: configure with -DMINNOTES_BUILD_TESTS=ON. Links the model + its core
# siblings directly (no QML/Quick), so it exercises the exact fixed code paths.
qt_add_executable(mn_regression_test
    regression_test.cpp
    ${CMAKE_SOURCE_DIR}/app/core/BlockModel.cpp
    ${CMAKE_SOURCE_DIR}/app/core/Document.cpp
    ${CMAKE_SOURCE_DIR}/app/core/TableGrid.cpp
    ${CMAKE_SOURCE_DIR}/app/core/MediaStore.cpp
    # PathMap.cpp defines mn::toRef / mn::resolveRef, referenced by BlockModel
    # and MediaStore (cross-OS path mapping). Link it or the test fails LNK2019.
    ${CMAKE_SOURCE_DIR}/app/core/PathMap.cpp
)
target_include_directories(mn_regression_test PRIVATE ${CMAKE_SOURCE_DIR}/app/core)
target_link_libraries(mn_regression_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Sql Qt6::Network
    Qt6::Pdf                       # MediaStore.cpp probes PDFs via QPdfDocument
    minnotes::ffmpeg)              # MediaStore.cpp probes media via FFmpeg

# FFmpeg DLLs next to the test exe so it launches on Windows.
if(WIN32)
    file(GLOB _t_ffmpeg "${MINNOTES_FFMPEG_ROOT}/bin/*.dll")
    foreach(_d IN LISTS _t_ffmpeg)
        get_filename_component(_n "${_d}" NAME)
        add_custom_command(TARGET mn_regression_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_d}" "$<TARGET_FILE_DIR:mn_regression_test>/${_n}" VERBATIM)
    endforeach()
endif()
