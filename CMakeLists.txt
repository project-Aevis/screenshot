cmake_minimum_required(VERSION 3.16)

project(cutefish-screenshot LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

find_package(Qt6 COMPONENTS Core DBus Gui Widgets Quick LinguistTools REQUIRED)

set(PROJECT_SOURCES
    src/main.cpp
    src/screenshotview.cpp
    qml.qrc
)

add_executable(cutefish-screenshot
  ${PROJECT_SOURCES}
)

target_link_libraries(cutefish-screenshot
  PRIVATE
  Qt6::Core
  Qt6::DBus
  Qt6::Gui
  Qt6::Widgets
  Qt6::Quick
)

file(GLOB TS_FILES translations/*.ts)
qt6_add_translation(QM_FILES ${TS_FILES})
add_custom_target(translations DEPENDS ${QM_FILES} SOURCES ${TS_FILES})
add_dependencies(cutefish-screenshot translations)

install(FILES ${QM_FILES} DESTINATION /usr/share/cutefish-screenshot/translations)
install(FILES ${QM_FILES} DESTINATION /usr/share/aevis-screenshot/translations)

install(TARGETS cutefish-screenshot RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES
    cutefish-screenshot.desktop
    aevis-screenshot.desktop
    DESTINATION /usr/share/applications/
    COMPONENT Runtime
)

install(CODE "execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink cutefish-screenshot \$ENV{DESTDIR}\${CMAKE_INSTALL_FULL_BINDIR}/aevis-screenshot)")
