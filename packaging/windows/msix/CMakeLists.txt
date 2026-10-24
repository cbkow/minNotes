# The Microsoft Store package (Windows only; ported from QCView-Player installer/CMakeLists.txt).
#   minnotes_msix        packs minNotes.exe + the tree its POST_BUILD steps stage into
#                        packaging/windows/msix/dist/minNotes-${PROJECT_VERSION}-x64.msix (see build_msix.ps1)
#   minnotes_msix_clean  wipes dist/ and any leftover staging/
# Build it from the `store` preset (MINNOTES_STORE_BUILD=ON: no WinSparkle — the Store owns updates).

if(NOT WIN32)
    return()
endif()

# The identity Partner Center assigned (Product management → Product identity). Ingestion rejects a mismatch.
set(MN_MSIX_IDENTITY "cbkow.minNotes"
    CACHE STRING "MSIX Identity/Name — must match Partner Center.")
set(MN_MSIX_PUBLISHER "CN=C64A1D41-EFAA-4EED-AE81-0C074867D4E9"
    CACHE STRING "MSIX Identity/Publisher — must match Partner Center (and a sideload cert's subject).")
set(MN_MSIX_PUBLISHER_DISPLAY_NAME "cbkow"
    CACHE STRING "MSIX Properties/PublisherDisplayName.")
set(MN_MSIX_VERSION "${PROJECT_VERSION}.0")   # MSIX versions have four parts

set(_mn_manifest_out "${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/AppxManifest.xml.in" "${_mn_manifest_out}" @ONLY)

find_program(MN_PWSH NAMES pwsh powershell)
if(NOT MN_PWSH)
    message(WARNING "No PowerShell on PATH — the minnotes_msix target is unavailable.")
    return()
endif()

if(NOT MINNOTES_STORE_BUILD)
    message(STATUS "minNotes: minnotes_msix is meant for the store preset (this tree carries WinSparkle; the script refuses it)")
endif()

add_custom_target(minnotes_msix
    COMMAND "${MN_PWSH}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/build_msix.ps1"
        -BuildDir "$<TARGET_FILE_DIR:minnotesapp>"
        -Version "${PROJECT_VERSION}"
        -ManifestPath "${_mn_manifest_out}"
        -ImagesDir "${CMAKE_CURRENT_SOURCE_DIR}/images"
        -OutputDir "${CMAKE_CURRENT_SOURCE_DIR}/dist"
    DEPENDS minnotesapp
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Packaging minNotes ${PROJECT_VERSION} as MSIX (packaging/windows/msix/dist/)"
    VERBATIM)

add_custom_target(minnotes_msix_clean
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_CURRENT_SOURCE_DIR}/dist" "${CMAKE_CURRENT_SOURCE_DIR}/staging"
    COMMENT "Removing packaging/windows/msix/dist and staging"
    VERBATIM)
