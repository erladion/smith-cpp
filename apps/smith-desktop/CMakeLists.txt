# smith-desktop: the desktop app, in QML. Everything but main() is a library
# with the QML module, for the tests.
add_library(smith_desktop_app STATIC
  Backend.cpp
  Backend.h
  BackendCommands.cpp
  BackendProperties.cpp
  BackendSessions.cpp
  BackendWorktrees.cpp
  MarkdownHtml.cpp
  MarkdownHtml.h
  Palette.cpp
  Palette.h
  RainItem.cpp
  RainItem.h
  TranscriptModel.cpp
  TranscriptModel.h
  Window.cpp
  Window.h
)
set(QT Qt${QT_VERSION_MAJOR})
target_link_libraries(smith_desktop_app PUBLIC smith_frontend smith_tui ${QT}::Gui ${QT}::Qml ${QT}::Quick ${QT}::QuickControls2)
# The generated QML type registrations include the headers by file name.
target_include_directories(smith_desktop_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Every file but the folder dialog is the same for Qt 5 and Qt 6.
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
  qml/SmithToolTip.qml
  qml/Spinner.qml
  qml/Splash.qml
  qml/StatusBar.qml
  qml/TodoCard.qml
  qml/ToolCard.qml
  qml/TopBar.qml
  qml/TranscriptView.qml
  qml/WorktreeList.qml
)

if(QT_VERSION_MAJOR EQUAL 6)
  list(APPEND SMITH_DESKTOP_QML qml/ProjectFolderDialog.qml)
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
  target_link_libraries(smith-desktop PRIVATE smith_desktop_app smith_desktop_appplugin)
else()
  # Qt 5 has no QML modules in CMake: the same files go into a resource file
  # at the paths Qt 6 gives them, with a qmldir for the Theme singleton.
  # Window.cpp registers the C++ types.
  list(APPEND SMITH_DESKTOP_QML qml/qt5/ProjectFolderDialog.qml)
  set(resources ${CMAKE_CURRENT_BINARY_DIR}/smith_desktop.qrc)
  set(qmldir ${CMAKE_CURRENT_BINARY_DIR}/qmldir)
  file(WRITE ${qmldir}.in "singleton Theme 1.0 Theme.qml\n")
  configure_file(${qmldir}.in ${qmldir} COPYONLY)
  set(entries "    <file alias=\"qmldir\">${qmldir}</file>\n")
  string(APPEND entries "    <file alias=\"icons/smith.svg\">${CMAKE_CURRENT_SOURCE_DIR}/icons/smith.svg</file>\n")
  foreach(file ${SMITH_DESKTOP_QML})
    get_filename_component(name ${file} NAME)
    string(APPEND entries "    <file alias=\"${name}\">${CMAKE_CURRENT_SOURCE_DIR}/${file}</file>\n")
  endforeach()
  file(WRITE ${resources}.in "<RCC>\n  <qresource prefix=\"/qt/qml/Smith/Desktop\">\n${entries}  </qresource>\n</RCC>\n")
  configure_file(${resources}.in ${resources} COPYONLY)
  qt5_add_resources(resource_sources ${resources})
  target_sources(smith_desktop_app PRIVATE ${resource_sources})
  add_executable(smith-desktop main.cpp)
  target_link_libraries(smith-desktop PRIVATE smith_desktop_app Qt5::Widgets)
endif()
install(TARGETS smith-desktop RUNTIME DESTINATION bin)
