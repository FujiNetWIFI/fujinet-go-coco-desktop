# Provide the XRoar sources (see cmake/Dependencies.cmake) and stage the subset
# we build into core/xroar-generated, which is what core/CMakeLists.txt
# compiles.
#
# XRoar builds with autotools. Rather than run its configure, we stage src/ +
# portalib/ + top-config.h and compile an explicit *headless* subset with our
# own flags, exactly as the Android app does. The SDL/GTK/X11/WASM/OpenGL
# frontends are simply never listed, so they are never compiled; three modules
# of our own (core/xroar/desktop/{ui,ao,joystick}_desktop.c) take their place.
#
# The copy is done in CMake rather than by a shell script: MSYS2/MinGW
# configures cannot execute a .sh through execute_process ("inappropriate file
# type or format"). The source edits live in tools/xroar/patch-staged-tree.py
# because a native Windows python IS directly invocable where a .sh is not.
#
# Staging is automatic: it runs when the staged tree is missing, when the
# source has moved, when the patch script or the generated config.h recipe
# changes, or on demand with -DXROAR_RESTAGE=ON (which is also how to pick up
# uncommitted edits in a working checkout pointed at by XROAR_SRC).

set(XROAR_GEN "${CMAKE_SOURCE_DIR}/core/xroar-generated")
set(XROAR_PATCH_SCRIPT "${CMAKE_SOURCE_DIR}/tools/xroar/patch-staged-tree.py")
set(XROAR_BECKER_SRC "${CMAKE_SOURCE_DIR}/tools/xroar/support/becker.c")

option(XROAR_RESTAGE "Re-stage XRoar sources from the source checkout" OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

coco_provide_dependency(
  NAME XRoar
  PATH third_party/xroar
  ARCHIVE "${XROAR_URL}"
  SHA256 "${XROAR_SHA256}"
  STRIP "xroar-${XROAR_VERSION}"
  SENTINEL src/becker.c
  OVERRIDE XROAR_SRC
  RESULT XROAR_DIR)

# What the staged tree was made from: the source identity plus hashes of the
# things that transform it, so that editing a transform re-stages rather than
# silently leaving the old result in place.
set(_xroar_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${XROAR_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _xroar_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT _xroar_head)
  # The normal case: a release tarball has no git metadata. Identify it by the
  # pinned version and the path it was unpacked to.
  set(_xroar_head "${XROAR_VERSION} ${XROAR_DIR}")
endif()
file(SHA256 "${XROAR_PATCH_SCRIPT}" _xroar_patch_hash)
file(SHA256 "${XROAR_BECKER_SRC}" _xroar_becker_hash)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _xroar_stage_hash)
set(_xroar_want
    "${_xroar_head} ${_xroar_patch_hash} ${_xroar_becker_hash} ${_xroar_stage_hash}")

set(_xroar_staged "")
if(EXISTS "${XROAR_GEN}/.source-info")
  file(READ "${XROAR_GEN}/.source-info" _xroar_staged)
  string(STRIP "${_xroar_staged}" _xroar_staged)
endif()

set(_xroar_sentinel "${XROAR_GEN}/src/xroar.c")

if(XROAR_RESTAGE OR NOT EXISTS "${_xroar_sentinel}"
   OR NOT _xroar_staged STREQUAL _xroar_want)
  message(STATUS "Staging XRoar sources from ${XROAR_DIR}")
  file(REMOVE_RECURSE "${XROAR_GEN}")
  file(MAKE_DIRECTORY "${XROAR_GEN}")

  foreach(_d src portalib)
    if(NOT IS_DIRECTORY "${XROAR_DIR}/${_d}")
      message(FATAL_ERROR "XRoar source ${XROAR_DIR} is missing ${_d}/")
    endif()
  endforeach()
  if(NOT EXISTS "${XROAR_DIR}/top-config.h")
    message(FATAL_ERROR "XRoar source ${XROAR_DIR} is missing top-config.h")
  endif()

  file(COPY "${XROAR_DIR}/src" "${XROAR_DIR}/portalib"
       DESTINATION "${XROAR_GEN}"
       PATTERN ".git" EXCLUDE
       PATTERN "build" EXCLUDE
       REGEX "\\.(o|a|so|d|lo|la)$" EXCLUDE)
  file(COPY "${XROAR_DIR}/top-config.h" DESTINATION "${XROAR_GEN}")

  if(NOT EXISTS "${_xroar_sentinel}")
    message(FATAL_ERROR "XRoar staging failed (source: ${XROAR_DIR})")
  endif()

  # Replace becker.c with the lazy-connect variant. The upstream becker_open()
  # connects exactly once and disables the port on failure; here XRoar shares a
  # process with the FujiNet runtime that it connects *out* to, and an in-place
  # CoCo 2 <-> CoCo 3 switch tears the link down and back up. A client that
  # gives up at startup never recovers from either. See the file's own header.
  file(COPY "${XROAR_BECKER_SRC}" DESTINATION "${XROAR_GEN}/src")

  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${XROAR_PATCH_SCRIPT}" "${XROAR_GEN}"
            "${XROAR_VERSION}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    # Leave nothing half-patched behind: the next configure must re-stage from
    # pristine sources rather than trying to patch a partial tree.
    file(REMOVE_RECURSE "${XROAR_GEN}")
    message(FATAL_ERROR
      "XRoar staging patches failed. The pinned version (${XROAR_VERSION}) "
      "and the anchors in\n  ${XROAR_PATCH_SCRIPT}\nhave to agree; a source "
      "checkout of a different version is the usual cause.\n${_out}${_err}")
  endif()

  file(WRITE "${XROAR_GEN}/.source-info" "${_xroar_want}\n")
endif()

message(STATUS "XRoar ${XROAR_VERSION} staged from ${XROAR_DIR}")
