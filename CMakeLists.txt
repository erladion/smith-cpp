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
# A sanitizer for everything built here, e.g. "thread" or "address,undefined".
set(SMITH_SANITIZE "" CACHE STRING "Build with -fsanitize=<this>")

# Qt 6.6+, or Qt 5.15. Qt 6 wins when both are found;
# point CMAKE_PREFIX_PATH at one to pick it. An older Qt 6 (a distribution's
# 6.4, say) is passed over for Qt 5 rather than failing.
find_package(Qt6 6.6 QUIET COMPONENTS Core)
if(Qt6_FOUND)
  set(QT_VERSION_MAJOR 6)
  set(SMITH_QT_MINIMUM 6.6)
else()
  find_package(Qt5 5.15 QUIET COMPONENTS Core)
  if(NOT Qt5_FOUND)
    message(FATAL_ERROR "Smith needs Qt 6.6 or newer, or Qt 5.15; neither was found. "
                        "Point CMAKE_PREFIX_PATH at a Qt installation.")
  endif()
  set(QT_VERSION_MAJOR 5)
  set(SMITH_QT_MINIMUM 5.15)
endif()
find_package(Qt${QT_VERSION_MAJOR} ${SMITH_QT_MINIMUM} REQUIRED COMPONENTS Core Network)
message(STATUS "Building against Qt ${Qt${QT_VERSION_MAJOR}Core_VERSION}")

# The desktop app needs Qt Quick; without it, only the terminal app is built.
if(SMITH_BUILD_DESKTOP)
  find_package(Qt${QT_VERSION_MAJOR} ${SMITH_QT_MINIMUM} QUIET COMPONENTS Gui Qml Quick QuickControls2)
  if(NOT Qt${QT_VERSION_MAJOR}Quick_FOUND OR NOT Qt${QT_VERSION_MAJOR}QuickControls2_FOUND)
    message(STATUS "Qt Quick or Quick Controls 2 not found: smith-desktop is not built")
    set(SMITH_BUILD_DESKTOP OFF)
  elseif(QT_VERSION_MAJOR EQUAL 6)
    qt_standard_project_setup(REQUIRES 6.6)
  else()
    # For the folder dialog's fallback; part of qtbase, so it is there when Quick is.
    find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets)
  endif()
endif()

# Settings every library and app here builds with.
add_library(smith_settings INTERFACE)
target_compile_definitions(smith_settings INTERFACE
  SMITH_VERSION="${PROJECT_VERSION}"
  QT_NO_KEYWORDS
)
target_compile_options(smith_settings INTERFACE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
if(SMITH_SANITIZE)
  target_compile_options(smith_settings INTERFACE -fsanitize=${SMITH_SANITIZE} -fno-omit-frame-pointer)
  target_link_options(smith_settings INTERFACE -fsanitize=${SMITH_SANITIZE})
endif()
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
