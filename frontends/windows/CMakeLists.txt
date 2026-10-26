# Windows frontend: native Win32 (Direct3D 11 display, GDI fallback) over the shared
# session core. Built only on Windows hosts; verified by the Windows CI job
# (MSYS2/MinGW-w64, .github/workflows/windows.yml). WIN32_EXECUTABLE => a
# WinMain entry point, no console window.

if(NOT WIN32)
  return()
endif()

enable_language(RC)

# The manifest carries the app version in its assembly identity, so it is
# generated from PROJECT_VERSION rather than repeating it. resource.rc picks
# the generated copy up through the RC compiler's include path.
configure_file(app.manifest.in "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" @ONLY)

add_executable(fujinet-go-coco-windows WIN32
  main.c
  display.c
  prefs.c
  debugger/dbg_window.c
  resource.rc
)
target_link_libraries(fujinet-go-coco-windows PRIVATE
  cocosession
  # d3d11: the picture (display.c; d3dcompiler_47 and avrt are loaded, not
  # linked). dwmapi: DwmFlush, the GDI fallback's frame clock.
  d3d11 gdi32 user32 comdlg32 dwmapi shell32 comctl32
)
target_include_directories(fujinet-go-coco-windows PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(fujinet-go-coco-windows PRIVATE -Wall -Wextra)

# Bundle the MinGW runtime statically so the .exe has no loose DLL deps.
target_link_options(fujinet-go-coco-windows PRIVATE
  -static -static-libgcc -static-libstdc++)

install(TARGETS fujinet-go-coco-windows RUNTIME DESTINATION bin)
