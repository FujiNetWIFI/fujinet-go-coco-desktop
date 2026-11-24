# External source dependencies.
#
# Standard practice for every target: the build provides each dependency for
# itself. A plain `git clone` of this repository (no --recurse-submodules),
# GNOME Builder, a flatpak build or a source tarball all end up with a usable
# checkout without the developer having to know the dependency exists.
#
# Two dependencies, provided two ways:
#
#   fujinet-firmware  a pinned git submodule under third_party/.
#   XRoar             a pinned release tarball, unpacked under third_party/.
#                     XRoar is distributed as a tarball from 6809.org.uk and
#                     the copy this repository builds against has no git
#                     metadata at all, so there is nothing to submodule. A
#                     URL + SHA256 is the honest pin: it is byte-reproducible,
#                     it is what the flatpak manifests already have to declare
#                     (`type: archive`), and it needs no fork to maintain.
#
# Resolution order for each dependency:
#   1. <NAME>_SRC (cache variable or environment) -- an out-of-tree working
#      checkout, which is how the dependencies are developed in tandem, e.g.
#      cmake -B build -DXROAR_SRC=~/Workspace/xroar-1.11
#   2. third_party/<name>, initialising the submodule when this tree is a git
#      checkout (git dependencies) or unpacking the pinned archive (archive
#      dependencies). An existing clean git checkout is moved onto the pin
#      when the pin changes.
#   3. For git dependencies with no git metadata to work from (release
#      tarballs, some IDE source copies): a direct clone of the pinned commit.

find_package(Git QUIET)

# XRoar 1.11 -- Ciaran Anscomb's Dragon/Tandy CoCo emulator, GPL-3.0-or-later.
# The tarball unpacks to a single xroar-<version>/ directory, which is stripped
# on the way into third_party/xroar.
set(XROAR_VERSION "1.11")
set(XROAR_URL "https://www.6809.org.uk/xroar/dl/xroar-${XROAR_VERSION}.tar.gz")
# Upstream keeps only its newest releases in dl/ (1.11 went 404 on 2026-11-15
# and broke every CI job). The Wayback Machine holds the same bytes; the pinned
# SHA256 is what makes either source trustworthy.
set(XROAR_URL_FALLBACK
    "https://web.archive.org/web/2026id_/https://www.6809.org.uk/xroar/dl/xroar-${XROAR_VERSION}.tar.gz")
set(XROAR_SHA256 "70270805ebd52c0b62237cc2f28b32b5c7df9764d10a7c7190de584cfc6e95af")

# fujinet-firmware: PR #2247's head (0c9f00c16), fork branch boip-idle-wait =
# master f703133d0 + the BoIP idle-wait fix (idle wakeups ~1000/s -> ~100/s).
# The previous pin was 13465cdd0 (in master). TEMPORARY: the pin returns to
# master when #2247 merges. The fork URL resolves the SHA.
set(FUJINET_COMMIT "0c9f00c16dff62d2b3c6b08821322f50642594db")
set(FUJINET_URL "https://github.com/tschak909/fujinet-firmware")

# coco_provide_dependency(NAME <n> PATH <p> SENTINEL <file> OVERRIDE <VAR>
#                         RESULT <out>
#                         [URL <u> COMMIT <sha>]        # git dependency
#                         [ARCHIVE <url> [FALLBACK <url>] SHA256 <hash> [STRIP <dir>]])
#
# SENTINEL is a path inside the checkout that only exists once the sources are
# really there -- an empty submodule directory is otherwise indistinguishable
# from a populated one, and a half-extracted archive from a complete one.
function(coco_provide_dependency)
  cmake_parse_arguments(DEP ""
    "NAME;PATH;URL;COMMIT;ARCHIVE;FALLBACK;SHA256;STRIP;SENTINEL;OVERRIDE;RESULT" ""
    ${ARGN})

  # 1. Explicit override: a checkout the developer maintains themselves.
  set(_override "")
  if(DEFINED ${DEP_OVERRIDE})
    set(_override "${${DEP_OVERRIDE}}")
  elseif(DEFINED ENV{${DEP_OVERRIDE}})
    set(_override "$ENV{${DEP_OVERRIDE}}")
  endif()
  if(_override)
    if(NOT EXISTS "${_override}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "${DEP_OVERRIDE}=${_override} does not look like a ${DEP_NAME} "
        "checkout (no ${DEP_SENTINEL}).")
    endif()
    message(STATUS "${DEP_NAME}: using ${_override} (${DEP_OVERRIDE})")
    set(${DEP_RESULT} "${_override}" PARENT_SCOPE)
    return()
  endif()

  set(_path "${CMAKE_SOURCE_DIR}/${DEP_PATH}")

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}" AND DEP_ARCHIVE)
    # 2a. Archive dependency: download the pin and unpack it.
    #
    # Extract into a scratch directory first and move the result into place
    # only once it is complete, so an interrupted configure cannot leave a
    # partial tree that the sentinel test would go on to accept.
    set(_dl "${CMAKE_BINARY_DIR}/_deps/${DEP_NAME}.archive")
    set(_tmp "${CMAKE_BINARY_DIR}/_deps/${DEP_NAME}-extract")

    if(NOT EXISTS "${_dl}")
      message(STATUS "${DEP_NAME}: downloading ${DEP_ARCHIVE}")
      set(_dl_rc 1)
      set(_dl_msg "")
      foreach(_url IN ITEMS "${DEP_ARCHIVE}" "${DEP_FALLBACK}")
        if(NOT _url OR _dl_rc EQUAL 0)
          continue()
        endif()
        # No EXPECTED_HASH here: on a failed download it raises a CMake error
        # that fails the configure even with STATUS, and the fallback would
        # never run. The hash is checked by hand below instead.
        file(DOWNLOAD "${_url}" "${_dl}"
             TLS_VERIFY ON
             STATUS _dl_status)
        list(GET _dl_status 0 _dl_rc)
        list(GET _dl_status 1 _dl_msg)
        if(_dl_rc EQUAL 0)
          file(SHA256 "${_dl}" _dl_hash)
          if(NOT _dl_hash STREQUAL "${DEP_SHA256}")
            set(_dl_rc 1)
            set(_dl_msg "SHA256 ${_dl_hash} is not the pinned ${DEP_SHA256}")
          endif()
        endif()
        if(NOT _dl_rc EQUAL 0)
          file(REMOVE "${_dl}")
          message(STATUS "${DEP_NAME}: ${_url} failed: ${_dl_msg}")
        endif()
      endforeach()
      if(NOT _dl_rc EQUAL 0)
        message(FATAL_ERROR
          "Could not download ${DEP_NAME} from ${DEP_ARCHIVE} (or ${DEP_FALLBACK}): ${_dl_msg}\n"
          "Download it by hand and unpack it, then point ${DEP_OVERRIDE} at "
          "the result.")
      endif()
    endif()

    file(REMOVE_RECURSE "${_tmp}")
    file(MAKE_DIRECTORY "${_tmp}")
    file(ARCHIVE_EXTRACT INPUT "${_dl}" DESTINATION "${_tmp}")

    set(_extracted "${_tmp}")
    if(DEP_STRIP)
      set(_extracted "${_tmp}/${DEP_STRIP}")
    endif()
    if(NOT EXISTS "${_extracted}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "${DEP_NAME} archive did not contain ${DEP_STRIP}/${DEP_SENTINEL} "
        "(${DEP_ARCHIVE}).")
    endif()

    file(REMOVE_RECURSE "${_path}")
    get_filename_component(_parent "${_path}" DIRECTORY)
    file(MAKE_DIRECTORY "${_parent}")
    file(RENAME "${_extracted}" "${_path}")
    file(REMOVE_RECURSE "${_tmp}")

  elseif(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
      message(FATAL_ERROR
        "${DEP_NAME} is missing and git is not installed. Either install git "
        "or unpack ${DEP_URL} (commit ${DEP_COMMIT}) into ${DEP_PATH}.")
    endif()

    if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
      # 2b. Submodule checkout. --filter=blob:none keeps the fetch to the
      # history the build needs; fujinet-firmware is a large repository.
      message(STATUS "${DEP_NAME}: fetching submodule ${DEP_PATH}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} submodule update --init --filter=blob:none
                -- "${DEP_PATH}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _rc)
      if(NOT _rc EQUAL 0)
        # Older servers/mirrors may refuse partial clones.
        execute_process(
          COMMAND ${GIT_EXECUTABLE} submodule update --init -- "${DEP_PATH}"
          WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
          RESULT_VARIABLE _rc)
      endif()
    endif()

    if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
      # 3. No git metadata (tarball): clone the pin outright.
      message(STATUS "${DEP_NAME}: cloning ${DEP_URL} @ ${DEP_COMMIT}")
      file(REMOVE_RECURSE "${_path}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} clone --filter=blob:none "${DEP_URL}" "${_path}"
        RESULT_VARIABLE _rc)
      if(_rc EQUAL 0)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -c advice.detachedHead=false checkout
                  --quiet "${DEP_COMMIT}"
          WORKING_DIRECTORY "${_path}"
          RESULT_VARIABLE _rc)
      endif()
    endif()
  endif()

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    if(DEP_ARCHIVE)
      message(FATAL_ERROR
        "Could not provide ${DEP_NAME}. Unpack ${DEP_ARCHIVE} into "
        "${DEP_PATH}, or point ${DEP_OVERRIDE} at an existing checkout.")
    else()
      message(FATAL_ERROR
        "Could not provide ${DEP_NAME}. Fetch it manually with\n"
        "    git submodule update --init ${DEP_PATH}\n"
        "or point ${DEP_OVERRIDE} at an existing checkout.")
    endif()
  endif()

  # Move a checkout that has drifted from the pin recorded here onto it. The
  # clone is made once, so without this a pin bump never reaches an existing
  # tree: local builds keep the old sources (and the FujiNet source patches,
  # which are anchored to exact text, fail confusingly) while CI, cloning
  # fresh, gets the new ones. A checkout with local changes is left alone,
  # loudly; develop through <NAME>_SRC instead.
  if(DEP_COMMIT AND EXISTS "${_path}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
      execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${_path}" rev-parse HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc)
      if(_rc EQUAL 0 AND NOT _head STREQUAL DEP_COMMIT)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -C "${_path}" status --porcelain
                  --untracked-files=no
          OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_dirty)
          message(WARNING
            "${DEP_NAME}: ${DEP_PATH} is at ${_head} with local changes, not "
            "the pin ${DEP_COMMIT} (cmake/Dependencies.cmake). Building it as "
            "it is.")
        else()
          message(STATUS "${DEP_NAME}: moving ${DEP_PATH} ${_head} -> ${DEP_COMMIT}")
          execute_process(
            COMMAND ${GIT_EXECUTABLE} -C "${_path}" cat-file -e "${DEP_COMMIT}^{commit}"
            RESULT_VARIABLE _have ERROR_QUIET)
          if(NOT _have EQUAL 0)
            execute_process(
              COMMAND ${GIT_EXECUTABLE} -C "${_path}" fetch --quiet
                      --filter=blob:none "${DEP_URL}" "${DEP_COMMIT}"
              RESULT_VARIABLE _rc)
          endif()
          execute_process(
            COMMAND ${GIT_EXECUTABLE} -c advice.detachedHead=false -C "${_path}"
                    checkout --quiet "${DEP_COMMIT}"
            RESULT_VARIABLE _rc)
          if(NOT _rc EQUAL 0)
            message(FATAL_ERROR
              "${DEP_NAME}: could not move ${DEP_PATH} to the pin ${DEP_COMMIT}. "
              "Delete ${DEP_PATH} to re-clone it, or point ${DEP_OVERRIDE} at a "
              "checkout.")
          endif()
        endif()
      endif()
    endif()
  endif()

  message(STATUS "${DEP_NAME}: ${_path}")
  set(${DEP_RESULT} "${_path}" PARENT_SCOPE)
endfunction()
