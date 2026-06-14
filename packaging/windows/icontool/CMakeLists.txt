# Standalone helper that renders external/icons/minNotesWindows.svg into
# packaging/windows/minNotes.ico (+ a committed PNG master). Built on demand,
# not part of the app build. See build-icon.ps1.
cmake_minimum_required(VERSION 3.21)
project(icontool LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED CMAKE_PREFIX_PATH AND NOT DEFINED Qt6_DIR)
    set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64" CACHE PATH "Qt install root")
endif()
find_package(Qt6 REQUIRED COMPONENTS Core Gui Svg)
add_executable(icontool main.cpp)
target_link_libraries(icontool PRIVATE Qt6::Core Qt6::Gui Qt6::Svg)
