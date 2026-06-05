cmake_minimum_required(VERSION 3.21)
project(minNotesSpike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick Gui)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(spikeapp
    main.cpp
    BlockModel.cpp
    BlockModel.h
    FenwickTree.h
)

qt_add_qml_module(spikeapp
    URI Spike
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/ArmListView.qml
        qml/ArmFlickable.qml
        qml/ArmEditor.qml
        qml/BlockDelegate.qml
        qml/Hud.qml
)

target_link_libraries(spikeapp PRIVATE Qt6::Quick Qt6::Gui)
