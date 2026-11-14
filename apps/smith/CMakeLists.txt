# smith: the terminal app. Everything but main() is a library, for the tests.
add_library(smith_tui_app STATIC
  App.cpp
  Connection.cpp
  Entries.cpp
  SessionCommands.cpp
  Suggestions.cpp
  View.cpp
)
target_link_libraries(smith_tui_app PUBLIC smith_frontend smith_tui)
target_include_directories(smith_tui_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(smith main.cpp)
target_link_libraries(smith PRIVATE smith_tui_app)
install(TARGETS smith RUNTIME DESTINATION bin)
