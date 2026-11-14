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
endif()

install(TARGETS smith RUNTIME DESTINATION bin)
