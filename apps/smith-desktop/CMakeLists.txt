# smith-desktop: the desktop app, in QML. Everything but main() is a library
# with the QML module, for the tests.
add_library(smith_desktop_app STATIC
  Backend.cpp
  BackendCommands.cpp
  BackendProperties.cpp
  BackendSessions.cpp
  BackendWorktrees.cpp
  Backend.h
  MarkdownHtml.cpp
  MarkdownHtml.h
  Palette.cpp
  Palette.h
  RainItem.cpp
  RainItem.h
  TranscriptModel.cpp
  TranscriptModel.h
)
target_link_libraries(smith_desktop_app PUBLIC smith_frontend smith_tui Qt6::Gui Qt6::Qml Qt6::Quick)
# The generated QML type registrations include the headers by file name.
target_include_directories(smith_desktop_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set(SMITH_DESKTOP_QML
  qml/Main.qml
  qml/Theme.qml
  qml/ActivityBar.qml
  qml/ApprovalPanel.qml
  qml/BannerCard.qml
  qml/Composer.qml
  qml/DiffView.qml
  qml/LandPanel.qml
  qml/Picker.qml
  qml/QuestionPanel.qml
  qml/Sidebar.qml
  qml/SmithButton.qml
  qml/Spinner.qml
  qml/Splash.qml
  qml/StatusBar.qml
  qml/TodoCard.qml
  qml/ToolCard.qml
  qml/TopBar.qml
  qml/TranscriptView.qml
  qml/WorktreeList.qml
)
# The files sit at the module's root, next to its qmldir, so they see each other and the Theme singleton.
foreach(file ${SMITH_DESKTOP_QML})
  get_filename_component(name ${file} NAME)
  set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(icons/smith.svg PROPERTIES QT_RESOURCE_ALIAS icons/smith.svg)
qt_add_qml_module(smith_desktop_app
  URI Smith.Desktop
  VERSION 1.0
  OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/qml/Smith/Desktop
  QML_FILES ${SMITH_DESKTOP_QML}
  RESOURCES icons/smith.svg
)

qt_add_executable(smith-desktop main.cpp)
target_link_libraries(smith-desktop PRIVATE smith_desktop_app smith_desktop_appplugin Qt6::QuickControls2)
install(TARGETS smith-desktop RUNTIME DESTINATION bin)
