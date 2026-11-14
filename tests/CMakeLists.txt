find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

# One suite per file of the original TypeScript tests. They cover the libraries
# and the terminal app (ui, and units runs the smith binary).
if(SMITH_BUILD_TUI)
  foreach(name units tools sandbox features agent ui)
    add_executable(test_${name} test_${name}.cpp TestUtil.h FakeServer.h)
    target_link_libraries(test_${name} PRIVATE smith_tui_app Qt::Test)
    target_compile_definitions(test_${name} PRIVATE
      SMITH_BINARY="$<TARGET_FILE:smith>"
      SMITH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    add_dependencies(test_${name} smith)
    add_test(NAME ${name} COMMAND test_${name})
  endforeach()
endif()

# The desktop app's QML against the fake server, offscreen.
if(SMITH_BUILD_DESKTOP)
  add_executable(test_desktop test_desktop.cpp TestUtil.h FakeServer.h)
  target_link_libraries(test_desktop PRIVATE smith_desktop_app smith_desktop_appplugin Qt6::QuickControls2 Qt6::Test)
  add_test(NAME desktop COMMAND test_desktop)
endif()
