# libcocosession: the toolkit-agnostic session core. Compiles the staged XRoar
# tree (the same explicit headless subset the Android app builds) plus the
# desktop session/audio/gamepad/FujiNet/debugger layers.

include(GNUInstallDirs)

set(XR "${CMAKE_SOURCE_DIR}/core/xroar-generated")

if(NOT EXISTS "${XR}/src/xroar.c")
  message(FATAL_ERROR
    "Staged XRoar tree is missing. cmake/StageXRoar.cmake should have "
    "produced it; re-run with -DXROAR_RESTAGE=ON.")
endif()

find_package(SDL3 CONFIG REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---- the staged XRoar core -------------------------------------------------
# XRoar normally builds with autotools. We are not running its configure, so
# the settings its Makefile would supply on the compile line are established
# here, and cmake/StageXRoar.cmake generates the config.h that top-config.h
# reads.

# We pass an explicit -rompath at run time, so these defaults never resolve
# anything; they exist because the sources reference the macros.
set(XROAR_DEFS
  HAVE_CONFIG_H=1
  ROMPATH="."
  CONFPATH="."
  AUTOSAVE_PREFIX="."
  # Guards the module registrations patch-staged-tree.py adds to src/ui.c and
  # src/ao.c. The Android app guards the same edits on __ANDROID__; a define of
  # our own is clearer here, because "desktop" covers four platform macros.
  XROAR_DESKTOP_MODULES=1
)

# XRoar's C is permissive about these three, and they are upstream's to fix.
# Silencing them only on XRoar's own sources keeps our warnings visible.
set(XROAR_WARN_OPTS -Wno-unused-parameter -Wno-sign-compare -Wno-multichar)

# Derived from XRoar's src/Makefile.am with the CoCo/Dragon feature set, and
# identical to the list the Android app compiles. The SDL/GTK/X11/WASM/OpenGL
# frontends are absent, so they are never built; so are the template .c files
# that are #included by other sources (mc6809_common.c, mc680x_ops.c,
# vo_render_tmpl.c, hkbd_lang_tables.c) and main_unix.c, whose main() we
# replace with our own drive loop.
set(XROAR_CORE_SOURCES
  src/ao.c
  src/auto_kbd.c
  src/ay891x.c
  src/becker.c
  src/blockdev.c
  src/breakpoint.c
  src/cart.c
  src/clock.c
  src/colourspace.c
  src/crc16.c
  src/crc32.c
  src/crclist.c
  src/dkbd.c
  src/events.c
  src/filter.c
  src/fs.c
  src/hexs19.c
  src/hkbd.c
  src/hkbd_joystick.c
  src/joystick.c
  src/keyboard.c
  src/logging.c
  src/machine.c
  src/mc10_cart.c
  src/messenger.c
  src/module.c
  src/mos6551.c
  src/ntsc.c
  src/null/ui_null.c
  src/null/ao_null.c
  src/part.c
  src/path.c
  src/printer.c
  src/ram.c
  src/rombank.c
  src/romlist.c
  src/screenshot.c
  src/serialise.c
  src/snapshot.c
  src/sndfile_compat.c
  src/sound.c
  src/tape.c
  src/tape_cas.c
  src/tape_sndfile.c
  src/ui.c
  src/vdg_pal.c
  src/vdg_palette.c
  src/vdisk.c
  src/vdrive.c
  src/vo.c
  src/vo_render.c
  src/xconfig.c
  src/xroar.c
  # MACHINE_ARCH_DRAGON
  src/dragon/coco.c
  src/dragon/immunity.c
  src/dragon/deluxecoco.c
  src/dragon/dragon.c
  src/dragon/dragon32.c
  src/dragon/dragon64.c
  src/dragon/dragonpro.c
  # MACHINE_ARCH_COCO3
  src/coco3.c
  # PART_MC6801 / PART_MC6809
  src/mc6801/mc6801.c
  src/mc6809/hd6309.c
  src/mc6809/mc6809.c
  # PART_MC6821
  src/mc6821.c
  # PART_MC6847
  src/mc6847/font-6847.c
  src/mc6847/font-6847t1.c
  src/mc6847/mc6847.c
  # PART_MC6883
  src/mc6883.c
  src/samx8.c
  # PART_TCC1014 (CoCo 3 GIME)
  src/tcc1014/font-gime.c
  src/tcc1014/tcc1014.c
  # CART_ARCH_DRAGON
  src/deltados.c
  src/dragondos.c
  src/gmc.c
  src/ide.c
  src/idecart.c
  src/ikon.c
  src/mooh.c
  src/mpi.c
  src/nx32.c
  src/orch90.c
  src/rsdos.c
  src/sn76489.c
  src/spi65.c
  src/spi_sdcard.c
  src/wd279x.c
)
list(TRANSFORM XROAR_CORE_SOURCES PREPEND "${XR}/")

set(XROAR_PORTALIB_SOURCES
  portalib/c-ctype.c
  portalib/c-strcasecmp.c
  portalib/c-strncasecmp.c
  portalib/delegate.c
  portalib/intfuncs.c
  portalib/pl-estrcpy.c
  portalib/sds.c
  portalib/sdsx.c
  portalib/slist.c
  portalib/strnlen.c
  portalib/strsep.c
  portalib/xmalloc.c
)
list(TRANSFORM XROAR_PORTALIB_SOURCES PREPEND "${XR}/")

add_library(xroar_core STATIC
  ${XROAR_CORE_SOURCES}
  ${XROAR_PORTALIB_SOURCES}
  # Our XRoar modules, registered in the staged src/ui.c and src/ao.c.
  xroar/desktop/ui_desktop.c
  xroar/desktop/ao_desktop.c
  xroar/desktop/joystick_desktop.c
)
set_target_properties(xroar_core PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(xroar_core PUBLIC ${XROAR_DEFS})
target_compile_options(xroar_core PRIVATE ${XROAR_WARN_OPTS})
# PUBLIC: coco_host.c and the debugger include XRoar's headers directly.
target_include_directories(xroar_core PUBLIC
  "${XR}" "${XR}/src" "${XR}/portalib"
  # ui_desktop.c and friends include "../coco_host.h".
  "${CMAKE_CURRENT_SOURCE_DIR}/xroar")
if(WIN32)
  # becker.c is a winsock client; portalib and src/fs.c want the rest.
  target_link_libraries(xroar_core PUBLIC ws2_32)
  # portalib/sdsx.c needs POSIX regex, which MinGW-w64 does not have. XRoar's
  # own Windows builds use TRE, and config.h defines HAVE_TRE there; libsystre
  # provides the regcomp/regexec entry points against it.
  target_link_libraries(xroar_core PUBLIC systre tre)
else()
  target_link_libraries(xroar_core PUBLIC m)
endif()

# ---- the CoCo system ROMs --------------------------------------------------
# XRoar resolves ROMs by basename against its -rompath, so unlike the Apple II
# target nothing has to be patched into the core: the session materialises
# whatever is embedded here into the user's ROM directory on first start.
# What changes with WITH_COCO_ROMS is only which images are embedded.
# See COMPLIANCE.md.
set(COCO_ROM_DIR_SRC "${CMAKE_SOURCE_DIR}/tools/xroar/roms")
# Always embedded: freely redistributable, and the FujiNet link does not work
# without them.
set(COCO_FREE_ROMS
  "${COCO_ROM_DIR_SRC}/hdbdw3bck.rom"
  "${COCO_ROM_DIR_SRC}/hdbdw3bc3.rom")
# Tandy/Microware copyright: local development only.
set(COCO_SYSTEM_ROMS
  "${COCO_ROM_DIR_SRC}/bas13.rom"
  "${COCO_ROM_DIR_SRC}/extbas11.rom"
  "${COCO_ROM_DIR_SRC}/coco3.rom")

if(WITH_COCO_ROMS)
  set(_embed_system ${COCO_SYSTEM_ROMS})
else()
  set(_embed_system "")
  message(STATUS
    "WITH_COCO_ROMS=OFF: the Tandy system ROMs are NOT embedded; the app reads "
    "them from the user's ROM directory at run time")
endif()

set(COCO_ROMS_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded.c")
add_custom_command(
  OUTPUT "${COCO_ROMS_C}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/tools/xroar/embed-roms.py"
          "${COCO_ROMS_C}"
          --system ${_embed_system}
          --free ${COCO_FREE_ROMS}
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/xroar/embed-roms.py"
          ${_embed_system} ${COCO_FREE_ROMS}
  COMMENT "Embedding the CoCo ROMs (WITH_COCO_ROMS=${WITH_COCO_ROMS})"
  VERBATIM)

# ---- the session library ---------------------------------------------------
add_library(cocosession STATIC
  xroar/coco_host.c
  src/session.c
  src/settings.c
  src/paths.c
  src/input_map.c
  src/audio_sdl.c
  src/gamepad_sdl.c
  src/fujinet_runtime.c
  debugger/debugger.c
  debugger/m6809dasm.c
  debugger/cocovideo.c
  debugger/symtab.c
  debugger/csrcinfo.c
  debugger/dwtrace.c
  "${COCO_ROMS_C}"
)

target_include_directories(cocosession
  PUBLIC include xroar
  PRIVATE src debugger "${CMAKE_CURRENT_BINARY_DIR}"
)
# -Wno-format-truncation: the path layer composes COCO_PATH_MAX-sized buffers
# from other max-sized buffers; snprintf truncation is the intended clamp
# there, not a bug.
target_compile_options(cocosession PRIVATE -Wall -Wextra -Wno-format-truncation)
# GCC 13's loop unswitching miscompiles dwtrace.c at -O3 (Ubuntu 24.04's
# 13.3: a TIME request reads as cut short and its reply as orphaned).
# GCC 14 and clang build it correctly.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 14)
  set_source_files_properties(debugger/dwtrace.c PROPERTIES COMPILE_OPTIONS -fno-unswitch-loops)
endif()
target_compile_definitions(cocosession PRIVATE
  _GNU_SOURCE
  COCO_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  COCO_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-coco"
  COCO_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-coco"
)

target_link_libraries(cocosession PUBLIC
  xroar_core
  SDL3::SDL3
  Threads::Threads
)
if(WIN32)
  target_link_libraries(cocosession PUBLIC ws2_32)
else()
  target_link_libraries(cocosession PUBLIC dl m)
endif()

# ---- tests -----------------------------------------------------------------
add_executable(input_test tests/input_test.c)
target_link_libraries(input_test cocosession)

add_executable(boot_smoke tests/boot_smoke.c)
target_link_libraries(boot_smoke cocosession)
# roms_embedded.h (src/) and the generated table's declarations, so the test
# can lay the ROMs down itself instead of depending on a provisioned session.
target_include_directories(boot_smoke PRIVATE src)

add_executable(debug_test tests/debug_test.c)
target_link_libraries(debug_test cocosession)
target_include_directories(debug_test PRIVATE src)

add_executable(cpu_switch_test tests/cpu_switch_test.c)
target_link_libraries(cpu_switch_test cocosession)
target_include_directories(cpu_switch_test PRIVATE src)

add_executable(dasm_test tests/dasm_test.c)
target_link_libraries(dasm_test cocosession)
target_include_directories(dasm_test PRIVATE debugger)

add_executable(symtab_test tests/symtab_test.c)
target_link_libraries(symtab_test cocosession)
target_include_directories(symtab_test PRIVATE debugger)

add_executable(csrcinfo_test tests/csrcinfo_test.c)
target_link_libraries(csrcinfo_test cocosession)
target_include_directories(csrcinfo_test PRIVATE debugger)
# Fixture .lst/.map came from a real `cmoc -i --decb` build, not hand-written
# (see core/tests/data) -- pointed at by absolute path so the test runs the
# same regardless of ctest's working directory.
target_compile_definitions(csrcinfo_test PRIVATE
  CSRCINFO_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")

# The FujiNet tab's DriveWire decoder, on made-up bytes: no ROMs, no
# network.
add_executable(dwtrace_test tests/dwtrace_test.c)
target_link_libraries(dwtrace_test cocosession)
target_include_directories(dwtrace_test PRIVATE debugger)

add_executable(session_test tests/session_test.c)
target_link_libraries(session_test cocosession)

add_executable(drivewire_smoke tests/drivewire_smoke.c)
target_link_libraries(drivewire_smoke cocosession)
target_include_directories(drivewire_smoke PRIVATE src)

add_executable(video_test tests/video_test.c)
target_link_libraries(video_test cocosession)
target_include_directories(video_test PRIVATE src)

# Includes debugger/cocovideo.c to reach a static -- see gimerow_test.c. That
# brings in the rest of the file's statics unused, hence -Wno-unused-function.
add_executable(gimerow_test tests/gimerow_test.c)
target_link_libraries(gimerow_test cocosession)
target_include_directories(gimerow_test PRIVATE debugger
                           "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(gimerow_test PRIVATE -Wall -Wextra -Wno-unused-function)

add_test(NAME input COMMAND input_test)
add_test(NAME session COMMAND session_test)
add_test(NAME dasm COMMAND dasm_test)
add_test(NAME symtab COMMAND symtab_test)
add_test(NAME csrcinfo COMMAND csrcinfo_test)
add_test(NAME dwtrace COMMAND dwtrace_test)
add_test(NAME debug COMMAND debug_test)
add_test(NAME cpu_switch COMMAND cpu_switch_test)
add_test(NAME boot_smoke COMMAND boot_smoke)
add_test(NAME drivewire_smoke COMMAND drivewire_smoke)
# One process per machine -- see video_test.c for why the same binary is
# registered twice rather than looping over both machines in one process.
add_test(NAME video_coco3 COMMAND video_test coco3)
add_test(NAME video_coco2 COMMAND video_test coco2)
add_test(NAME gimerow COMMAND gimerow_test)
# All need system ROMs, which a WITH_COCO_ROMS=OFF checkout does not have;
# drivewire_smoke additionally needs libfujinet and a free Becker port. All
# are reported as 77 rather than as failures.
set_tests_properties(boot_smoke drivewire_smoke debug cpu_switch video_coco3
                     video_coco2 PROPERTIES SKIP_RETURN_CODE 77)
# It boots a machine, types at it and waits on real DriveWire traffic, so it
# is the one test here that takes seconds rather than milliseconds. It also
# binds a fixed port, so it must not run beside another copy of itself.
set_tests_properties(drivewire_smoke PROPERTIES TIMEOUT 120 RUN_SERIAL ON)

# The no_embedded_roms check is registered at the TOP level instead, so it can
# also be pointed at the frontend executables -- those are the artifacts that
# actually ship, and they are not known in this scope. See the end of the
# top-level CMakeLists.txt.
