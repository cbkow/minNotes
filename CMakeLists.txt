cmake_minimum_required(VERSION 3.24)

project(MinNotes
    VERSION 0.1.0
    DESCRIPTION "minNotes — block-based markdown notes editor"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Sql)
qt_standard_project_setup(REQUIRES 6.11)

# Code-block syntax highlighting: KSyntaxHighlighting (KDE) — a QSyntaxHighlighter
# over QTextDocument, the same mechanism the inline highlighter uses. Built once
# from source into external/kf6 (gitignored); see external/build-ksyntax.sh.
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/external/kf6")
find_package(KF6SyntaxHighlighting 6.0 REQUIRED)

add_subdirectory(app)
