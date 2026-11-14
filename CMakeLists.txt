cmake_minimum_required(VERSION 3.21)

project(smith VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network)

set(SMITH_CORE_SOURCES
  src/app/Runtime.cpp
  src/core/Agent.cpp
  src/core/Config.cpp
  src/core/Context.cpp
  src/core/Diff.cpp
  src/core/Dirs.cpp
  src/core/Errors.cpp
  src/core/Glob.cpp
  src/core/History.cpp
  src/core/Http.cpp
  src/core/Instructions.cpp
  src/core/Jobs.cpp
  src/core/Json.cpp
  src/core/Paths.cpp
  src/core/Preferences.cpp
  src/core/Process.cpp
  src/core/Prompt.cpp
  src/core/Reasoning.cpp
  src/core/RunLater.cpp
  src/core/Sandbox.cpp
  src/core/ContainerSandbox.cpp
  src/core/Server.cpp
  src/core/Session.cpp
  src/core/Skills.cpp
  src/core/Think.cpp
  src/core/Tool.cpp
  src/core/ToolLog.cpp
  src/core/Toolchain.cpp
  src/core/Undo.cpp
  src/core/tools/AskTool.cpp
  src/core/tools/BashTool.cpp
  src/core/tools/FileTools.cpp
  src/core/tools/JobTools.cpp
  src/core/tools/SearchTools.cpp
  src/core/tools/SkillTool.cpp
  src/core/tools/TaskTool.cpp
  src/core/tools/TodoTool.cpp
  src/tui/Editor.cpp
  src/tui/Keys.cpp
  src/tui/Markdown.cpp
  src/tui/Picker.cpp
  src/tui/Rain.cpp
  src/tui/Renderer.cpp
  src/tui/Style.cpp
  src/tui/Terminal.cpp
  src/tui/Text.cpp
  src/ui/App.cpp
  src/ui/Commands.cpp
  src/ui/Connection.cpp
  src/ui/Duration.cpp
  src/ui/Entries.cpp
  src/ui/Folders.cpp
  src/ui/Greetings.cpp
  src/ui/Replay.cpp
  src/ui/SessionCommands.cpp
  src/ui/Suggestions.cpp
  src/ui/View.cpp
)

# Everything but main(), so the tests link against the same code.
add_library(smithcore STATIC ${SMITH_CORE_SOURCES})
target_include_directories(smithcore PUBLIC src)
target_link_libraries(smithcore PUBLIC Qt6::Core Qt6::Network)
target_compile_definitions(smithcore PUBLIC
  SMITH_VERSION="${PROJECT_VERSION}"
  QT_NO_KEYWORDS
)
target_compile_options(smithcore PUBLIC -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

add_executable(smith src/main.cpp)
target_link_libraries(smith PRIVATE smithcore)

# The desktop app (QML), when Qt Quick is installed.
option(SMITH_BUILD_DESKTOP "Build smith-desktop, the QML desktop app" ON)
if(SMITH_BUILD_DESKTOP)
  find_package(Qt6 6.5 COMPONENTS Gui Qml Quick QuickControls2)
  if(NOT Qt6Quick_FOUND OR NOT Qt6QuickControls2_FOUND)
    message(STATUS "Qt Quick not found: smith-desktop is not built")
    set(SMITH_BUILD_DESKTOP OFF)
  endif()
endif()
if(SMITH_BUILD_DESKTOP)
  qt_standard_project_setup(REQUIRES 6.5)
  add_library(smithdesktop STATIC
    src/desktop/Backend.cpp
    src/desktop/MarkdownHtml.cpp
    src/desktop/Palette.cpp
    src/desktop/RainItem.cpp
    src/desktop/TranscriptModel.cpp
    src/desktop/Backend.h
    src/desktop/MarkdownHtml.h
    src/desktop/Palette.h
    src/desktop/RainItem.h
    src/desktop/TranscriptModel.h
  )
  target_link_libraries(smithdesktop PUBLIC smithcore Qt6::Gui Qt6::Qml Qt6::Quick)
  # The generated QML type registrations include the headers by file name.
  target_include_directories(smithdesktop PRIVATE src/desktop)
  set(SMITH_DESKTOP_QML
    src/desktop/qml/Main.qml
    src/desktop/qml/Theme.qml
    src/desktop/qml/ActivityBar.qml
    src/desktop/qml/ApprovalPanel.qml
    src/desktop/qml/BannerCard.qml
    src/desktop/qml/Composer.qml
    src/desktop/qml/DiffView.qml
    src/desktop/qml/Picker.qml
    src/desktop/qml/QuestionPanel.qml
    src/desktop/qml/Sidebar.qml
    src/desktop/qml/SmithButton.qml
    src/desktop/qml/Spinner.qml
    src/desktop/qml/Splash.qml
    src/desktop/qml/StatusBar.qml
    src/desktop/qml/TodoCard.qml
    src/desktop/qml/ToolCard.qml
    src/desktop/qml/TopBar.qml
    src/desktop/qml/TranscriptView.qml
  )
  # The files sit at the module's root, next to its qmldir, so they see each other and the Theme singleton.
  foreach(file ${SMITH_DESKTOP_QML})
    get_filename_component(name ${file} NAME)
    set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${name})
  endforeach()
  set_source_files_properties(src/desktop/qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
  set_source_files_properties(src/desktop/icons/smith.svg PROPERTIES QT_RESOURCE_ALIAS icons/smith.svg)
  qt_add_qml_module(smithdesktop
    URI Smith.Desktop
    VERSION 1.0
    QML_FILES ${SMITH_DESKTOP_QML}
    RESOURCES src/desktop/icons/smith.svg
  )

  qt_add_executable(smith-desktop src/desktop/main.cpp)
  target_link_libraries(smith-desktop PRIVATE smithdesktop smithdesktopplugin Qt6::QuickControls2)
  install(TARGETS smith-desktop RUNTIME DESTINATION bin)
endif()

option(SMITH_BUILD_TESTS "Build the unit tests" ON)
if(SMITH_BUILD_TESTS)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  enable_testing()
  # One suite per file of the original TypeScript tests.
  foreach(name units tools sandbox features agent ui)
    add_executable(test_${name} tests/test_${name}.cpp tests/TestUtil.h tests/FakeServer.h)
    target_link_libraries(test_${name} PRIVATE smithcore Qt6::Test)
    target_compile_definitions(test_${name} PRIVATE
      SMITH_BINARY="$<TARGET_FILE:smith>"
      SMITH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_dependencies(test_${name} smith)
    add_test(NAME ${name} COMMAND test_${name})
  endforeach()
  if(SMITH_BUILD_DESKTOP)
    # The desktop app's QML against the fake server, offscreen.
    add_executable(test_desktop tests/test_desktop.cpp tests/TestUtil.h tests/FakeServer.h)
    target_link_libraries(test_desktop PRIVATE smithdesktop smithdesktopplugin Qt6::QuickControls2 Qt6::Test)
    target_include_directories(test_desktop PRIVATE src)
    add_test(NAME desktop COMMAND test_desktop)
  endif()
endif()

install(TARGETS smith RUNTIME DESTINATION bin)
