# logoscore-webhost -- the Qt WebEngine page host.
#
# A PROJECT OF ITS OWN, and that is the point: it links no Logos library, sees no
# Logos header and shares no include path with the daemon that spawns it. Its
# whole dependency set is Qt. So it can be configured and built on its own --
# `cmake -S src/webhost` -- which is what the `webhost` nix output does, and why
# that output does not drag liblogos, the SDK or the Qt host runtime in behind a
# binary that uses none of them.
#
# The top-level build add_subdirectory()s this when LOGOSCORE_WITH_WEBENGINE=ON,
# so a tree built that way ships the host beside the daemon, which is where the
# backend looks for it first.
cmake_minimum_required(VERSION 3.16)
project(LogoscoreWebhost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(QT NAMES Qt6 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED
             COMPONENTS Core Gui Network WebEngineCore WebEngineQuick WebChannel)

add_executable(logoscore-webhost main.cpp)
target_link_libraries(logoscore-webhost PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::WebEngineCore
    Qt${QT_VERSION_MAJOR}::WebEngineQuick
    Qt${QT_VERSION_MAJOR}::WebChannel
)

if(APPLE)
    set_target_properties(logoscore-webhost PROPERTIES
        INSTALL_RPATH "@loader_path/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
else()
    set_target_properties(logoscore-webhost PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()

install(TARGETS logoscore-webhost RUNTIME DESTINATION bin)
