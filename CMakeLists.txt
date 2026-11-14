cmake_minimum_required(VERSION 3.21)

project(smith VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(SMITH_BUILD_TUI "Build smith, the terminal app" ON)
option(SMITH_BUILD_DESKTOP "Build smith-desktop, the QML desktop app" ON)
option(SMITH_BUILD_TESTS "Build the tests" ON)

# Qt 6.6+, or Qt 5.15 for the terminal app. Qt 6 wins when both are found;
# point CMAKE_PREFIX_PATH at one to pick it.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
if(QT_VERSION_MAJOR EQUAL 6)
  set(SMITH_QT_MINIMUM 6.6)
else()
  set(SMITH_QT_MINIMUM 5.15)
endif()
find_package(Qt${QT_VERSION_MAJOR} ${SMITH_QT_MINIMUM} REQUIRED COMPONENTS Core Network)
message(STATUS "Building against Qt ${Qt${QT_VERSION_MAJOR}Core_VERSION}")

# The desktop app needs Qt 6 and Qt Quick; without them, only the terminal app is built.
if(SMITH_BUILD_DESKTOP AND QT_VERSION_MAJOR LESS 6)
  message(STATUS "Qt 5: smith-desktop needs Qt 6 and is not built")
  set(SMITH_BUILD_DESKTOP OFF)
endif()
if(SMITH_BUILD_DESKTOP)
  find_package(Qt6 6.6 COMPONENTS Gui Qml Quick QuickControls2)
  if(NOT Qt6Quick_FOUND OR NOT Qt6QuickControls2_FOUND)
    message(STATUS "Qt Quick not found: smith-desktop is not built")
    set(SMITH_BUILD_DESKTOP OFF)
  else()
    qt_standard_project_setup(REQUIRES 6.6)
  endif()
endif()

# Settings every library and app here builds with.
add_library(smith_settings INTERFACE)
target_compile_definitions(smith_settings INTERFACE
  SMITH_VERSION="${PROJECT_VERSION}"
  QT_NO_KEYWORDS
)
target_compile_options(smith_settings INTERFACE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# Libraries are included as "core/...", "tui/..." and "frontend/...".
target_include_directories(smith_settings INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/libs)

add_subdirectory(libs/core)
add_subdirectory(libs/tui)
add_subdirectory(libs/frontend)

if(SMITH_BUILD_TUI)
  add_subdirectory(apps/smith)
endif()
if(SMITH_BUILD_DESKTOP)
  add_subdirectory(apps/smith-desktop)
endif()

if(SMITH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
