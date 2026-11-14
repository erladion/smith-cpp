# A general terminal toolkit: styled text and its width, wrapping, Markdown,
# digital rain, keys, a line editor, pickers and an inline renderer. It knows
# nothing of the agent. The desktop app uses its Markdown renderer and rain.
add_library(smith_tui STATIC
  Editor.cpp
  Keys.cpp
  Markdown.cpp
  Picker.cpp
  Rain.cpp
  Renderer.cpp
  Style.cpp
  Terminal.cpp
  Text.cpp
)
target_link_libraries(smith_tui PUBLIC smith_settings Qt::Core)
