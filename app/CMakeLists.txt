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
)

target_include_directories(minnotesapp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(minnotesapp PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
)
