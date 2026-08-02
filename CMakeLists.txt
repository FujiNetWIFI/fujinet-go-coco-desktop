cmake_minimum_required(VERSION 3.20)
# The version lives here and nowhere else: the About boxes, the macOS bundle
# and the release workflow all take it from PROJECT_VERSION, and cutting a
# tag checks that the tag agrees with it (see "Cutting a release" in the
# README).
project(fujinet-go-coco-desktop VERSION 0.2.0 LANGUAGES C)

add_compile_definitions(COCO_VERSION_STRING="${PROJECT_VERSION}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Which native frontend(s) to build. Each is found-or-skipped so a lean
# machine can still build the other one (or just the core + tests).
set(FRONTEND "all" CACHE STRING
    "Frontend to build: gnome, kde, macos, windows, all, none")
option(WITH_WEBVIEW "Embed the FujiNet web UI (WebKitGTK / QtWebEngine)" ON)

# Color BASIC, Extended Color BASIC and the CoCo 3's Super Extended Color BASIC
# are Tandy/Microware copyrighted firmware and are NOT freely licensed. The
# HDB-DOS DW3 cartridge ROMs are freely redistributable and are always
# embedded, so the FujiNet link works either way.
#
#   ON  (default) -- embed the system ROMs too, so a development build boots
#                    straight to the CoCo 3 banner. NOT for redistribution.
#   OFF           -- embed only HDB-DOS; the user supplies the system ROMs
#                    through the "Import System ROMs..." path. Every artifact
#                    this repository publishes is built this way, and
#                    core/tests/no_embedded_roms.py checks that claim against
#                    the shipped binaries.
#
# See COMPLIANCE.md.
option(WITH_COCO_ROMS "Embed the Tandy system ROMs (see COMPLIANCE.md)" ON)

# Included here, at the top of the tree, so every subdirectory sees the
# *variables* it defines: GNUInstallDirs caches CMAKE_INSTALL_DATADIR as an
# empty string and only sets its non-cache fallback (share) in the including
# scope. Including it in a subdirectory (as core/ does) therefore leaves the
# frontends with an empty DATADIR -- and "${CMAKE_INSTALL_DATADIR}/applications"
# then resolves to the absolute path /applications, outside the prefix.
include(GNUInstallDirs)

include(cmake/Dependencies.cmake)
include(cmake/StageXRoar.cmake)

enable_testing()
add_subdirectory(core)

# The FujiNet runtime: built from the pinned firmware sources as part of the
# normal build and installed alongside the app (the session provisions the
# user's runtime tree from the installed copy on first start). Set up before
# the frontends so the macOS bundle can depend on the library it packs.
include(cmake/FujiNetRuntime.cmake)

# Each frontend is native to one platform; with FRONTEND=all (the default)
# only the host's viable frontends are considered, so a Windows/macOS
# configure never descends into the Linux (pkg-config) frontends and vice
# versa. Naming a frontend explicitly still forces just that one.
#
# add_frontend also skips a directory that has no CMakeLists.txt yet, which is
# how a milestone that has not landed stays out of the way instead of breaking
# the configure for everyone. Naming it explicitly is an error, so nobody can
# ask for a frontend and silently get nothing.
function(add_frontend name)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/frontends/${name}/CMakeLists.txt")
    if(FRONTEND STREQUAL name)
      message(FATAL_ERROR "The ${name} frontend is not implemented yet.")
    endif()
    message(STATUS "${name} frontend: not implemented yet -- skipping")
    return()
  endif()
  add_subdirectory("frontends/${name}")
endfunction()

if(FRONTEND STREQUAL "gnome" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(gnome)
endif()
if(FRONTEND STREQUAL "kde" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(kde)
endif()
if(FRONTEND STREQUAL "macos" OR (FRONTEND STREQUAL "all" AND APPLE))
  add_frontend(macos)
endif()
if(FRONTEND STREQUAL "windows" OR (FRONTEND STREQUAL "all" AND WIN32))
  add_frontend(windows)
endif()

# The ROM-less claim, checked on every binary built with this option off.
#
# Registered here rather than in core/ so it can be pointed at the frontend
# executables: those are the artifacts a user actually downloads, and they link
# differently from the test binaries. The Apple II port learned this the hard
# way -- checking only the test binary both hid a bug and would have produced a
# false positive the moment it was pointed anywhere real.
if(NOT WITH_COCO_ROMS)
  set(COCO_ROM_CHECK_TARGETS boot_smoke)
  foreach(_fe gnome kde macos windows)
    if(TARGET fujinet-go-coco-${_fe})
      list(APPEND COCO_ROM_CHECK_TARGETS fujinet-go-coco-${_fe})
    endif()
  endforeach()
  foreach(_t IN LISTS COCO_ROM_CHECK_TARGETS)
    add_test(NAME no_embedded_roms_${_t}
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_SOURCE_DIR}/core/tests/no_embedded_roms.py"
                     "$<TARGET_FILE:${_t}>"
                     "${CMAKE_SOURCE_DIR}/tools/xroar/roms")
  endforeach()
endif()

# Application icons (rendered from the Android app's launcher art by
# tools/icons/make-icons.py). Desktop environments look the icon up by the
# desktop-entry id, so the same art is installed under each frontend's id --
# at every hicolor size, which is what makes it show up crisply everywhere
# from the 16px window list to the 256px app grid.
#
# Only for frontends actually built here: a GNOME-only configure installing
# the KDE id's icon too was harmless (nothing references an orphaned icon
# with no matching .desktop/.metainfo), but M9's per-frontend packages make
# it worth tightening -- a package should not ship files for an app it does
# not contain.
set(COCO_ICON_SIZES 16 24 32 48 64 128 192 256 512)
set(COCO_ICON_TARGETS_AND_IDS
  fujinet-go-coco-gnome online.fujinet.go.coco.gnome
  fujinet-go-coco-kde online.fujinet.go.coco.kde)
list(LENGTH COCO_ICON_TARGETS_AND_IDS _coco_icon_n)
math(EXPR _coco_icon_last "${_coco_icon_n} - 1")
foreach(_i RANGE 0 ${_coco_icon_last} 2)
  list(GET COCO_ICON_TARGETS_AND_IDS ${_i} _target)
  math(EXPR _j "${_i} + 1")
  list(GET COCO_ICON_TARGETS_AND_IDS ${_j} _id)
  if(TARGET ${_target})
    foreach(_size IN LISTS COCO_ICON_SIZES)
      install(FILES data/icons/hicolor/${_size}x${_size}/apps/fujinet-go-coco.png
              DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps
              RENAME ${_id}.png)
    endforeach()
  endif()
endforeach()

# ---- packaging (M9) --------------------------------------------------------
#
# One package per frontend, not one package for both: GNOME (GTK4/
# libadwaita) and KDE (Qt6) pull in entirely different runtime library
# stacks, and a user who wants one should not be made to depend on the
# other. So this expects FRONTEND=gnome or FRONTEND=kde -- exactly the same
# per-frontend split the flatpak manifests already use -- and names the
# package accordingly; FRONTEND=all (the default) still configures fine for
# ordinary development builds, it just packages under a name that says so
# rather than silently mixing both frontends' files into one archive.
if(FRONTEND STREQUAL "gnome")
  set(CPACK_PACKAGE_NAME "fujinet-go-coco-gnome")
elseif(FRONTEND STREQUAL "kde")
  set(CPACK_PACKAGE_NAME "fujinet-go-coco-kde")
else()
  set(CPACK_PACKAGE_NAME "fujinet-go-coco")
endif()

set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "FujiNet")
set(CPACK_PACKAGE_CONTACT "https://github.com/FujiNetWIFI/fujinet-go-coco-desktop/issues")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://fujinet.online/")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Self-contained Tandy Color Computer with built-in FujiNet")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

if(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR "DEB;RPM;TGZ")

  # Auto-detected from the actual linked libraries (dpkg-shlibdeps /
  # rpmbuild's own dependency generator) rather than hand-listed: GTK4 vs
  # Qt6 alone makes one fixed list wrong for whichever frontend was not
  # built, and a hand-list silently goes stale the moment a dependency's
  # package name changes across distributions.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_SECTION "games")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")

  set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
  set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
  set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
  set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
endif()

# CPACK_PACKAGE_FILE_NAME defaults to <name>-<version>-<system>[-<arch>];
# left as-is except spelling out the generator explicitly is unnecessary --
# the default already includes it via the file extension.
include(CPack)
