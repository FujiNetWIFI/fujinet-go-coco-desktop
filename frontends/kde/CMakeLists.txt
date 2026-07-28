# KDE frontend: plain Qt6 Widgets -- deliberately NOT KDE Frameworks. Breeze
# arrives through the platform theme, and staying framework-free keeps this
# frontend reusable (and the future Windows port clean). Found-or-skipped so
# a machine without Qt can still build the core and the GNOME side.

# This is the ONLY C++ in the project -- XRoar is pure C, unlike the Apple II
# target's AppleWin -- so the top-level project() declares C alone and the
# language is enabled here. A core-only or GNOME-only build then needs no C++
# compiler at all. (Qt's own config modules run check_cxx_source_compiles
# during find_package, so this has to come first.)
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.4 COMPONENTS Widgets OpenGLWidgets QUIET)

if(NOT Qt6_FOUND)
  message(STATUS "KDE frontend: Qt6 Widgets not found -- skipping")
  return()
endif()

set(CMAKE_AUTOMOC ON)

set(KDE_APP_ID online.fujinet.go.coco.kde)

add_executable(fujinet-go-coco-kde
  main.cpp
  MainWindow.cpp
  DisplayWidget.cpp
  SettingsDialog.cpp
  FujiNetWindows.cpp
  debugger/DebuggerWindow.cpp
)

target_link_libraries(fujinet-go-coco-kde PRIVATE
  cocosession Qt6::Widgets Qt6::OpenGLWidgets)
# debugger/DebuggerWindow.cpp includes its own header by that path.
target_include_directories(fujinet-go-coco-kde PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(fujinet-go-coco-kde PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-coco-kde PRIVATE
  COCO_ICON_FILE="${CMAKE_SOURCE_DIR}/data/icons/hicolor/256x256/apps/fujinet-go-coco.png")

if(WITH_WEBVIEW)
  # QtWebEngine is enormous and absent on many systems, so it is never a hard
  # dependency: without it the FujiNet config opens in the system browser.
  find_package(Qt6 6.4 COMPONENTS WebEngineWidgets QUIET)
  if(Qt6WebEngineWidgets_FOUND)
    target_link_libraries(fujinet-go-coco-kde PRIVATE Qt6::WebEngineWidgets)
    target_compile_definitions(fujinet-go-coco-kde PRIVATE HAVE_WEBENGINE)
  else()
    message(STATUS "KDE frontend: QtWebEngine not found -- FujiNet config opens in the browser")
  endif()
endif()

install(TARGETS fujinet-go-coco-kde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/${KDE_APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${KDE_APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_entry_kde
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${KDE_APP_ID}.desktop")
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_kde
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${KDE_APP_ID}.metainfo.xml")
endif()
