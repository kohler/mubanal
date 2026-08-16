# BuildMuPDF.cmake -- download and build MuPDF with its C++ bindings.
#
# This is the default. Most distributions package only MuPDF's C library, not
# libmupdfcpp, so building our own is the path of least resistance; where a
# suitable MuPDF is already installed, -DMUPDF_BUILD=OFF uses it instead, via
# FindMuPDF.cmake.
#
# This uses the official source release rather than a git clone. The release
# tarball is ~66MB and carries MuPDF's bundled third-party libraries; a
# --recursive clone of the git repository is ~960MB for the same result.
#
# Nothing is installed: mubanal links against the libraries where they are
# built, inside this build tree. That is not just tidiness -- MuPDF's
# `install-shared-*` targets refuse to run unless USE_SYSTEM_LIBS=yes, on the
# grounds that a shared library built from the bundled third-party sources would
# export them. Linking in place sidesteps that and keeps the build
# self-contained.
#
# Needs at build time: make, a C/C++ toolchain, python3 with venv, and network
# access. A from-scratch build takes several minutes, once per build tree.

include(ExternalProject)
include(ProcessorCount)

# Give the extracted files the extraction time, so a re-download rebuilds.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(MUPDF_VERSION "1.28.1" CACHE STRING "MuPDF version to download and build")
set(MUPDF_URL_HASH
    "SHA256=dc94c60b2537e2ac9a2d379dd3801545f84a3a302d15c9da358362a1270707c3"
    CACHE STRING "Expected hash of the MuPDF source tarball; update with MUPDF_VERSION")

ProcessorCount(_mupdf_jobs)
if(_mupdf_jobs EQUAL 0)
  set(_mupdf_jobs 1)
endif()
set(MUPDF_BUILD_JOBS ${_mupdf_jobs} CACHE STRING "Parallel jobs for the MuPDF build")

# MuPDF bundles its third-party libraries and builds them by default, because it
# often wants newer versions than distributions ship. That keeps this build
# self-contained, but it also means the bundled freetype, openjpeg, libjpeg and
# jbig2dec never receive distribution security updates -- worth weighing, since
# this tool parses PDFs from untrusted submitters. Turning this on uses the
# system copies where they are available; MuPDF bundles lcms2 (it prefers its
# lcms2mt fork), mujs, jpegxr and cmark-gfm regardless.
#
# Changing this on an existing build tree would leave stale objects behind, as
# MuPDF builds both ways into the same directory: delete the build tree first.
#
# On macOS this is not a preference but an incapability: MuPDF keeps its
# pkg-config discovery in the non-Darwin branch of Makerules, so it falls back
# to bare -l flags whatever is installed -- no -I for Homebrew's prefix, none
# for openjpeg's versioned include directory, and a stale -lfreetype2. So the
# option is offered only where it can work, and forced off elsewhere.
include(CMakeDependentOption)
cmake_dependent_option(MUPDF_SYSTEM_LIBS
  "Build MuPDF against system third-party libraries" ON
  "NOT APPLE" OFF)
if(APPLE)
  message(STATUS "MuPDF: bundling third-party libraries (system libraries are unavailable on macOS)")
endif()

set(_mupdf_make_vars USE_SYSTEM_LIBS=no)

if(MUPDF_SYSTEM_LIBS)
  # pkg-config is how the libraries are found, but its absence is not fatal.
  # Bundling everything is exactly what MUPDF_SYSTEM_LIBS=OFF does, and this
  # path has to keep working on a machine with nothing installed on it.
  find_package(PkgConfig)
endif()

if(MUPDF_SYSTEM_LIBS AND NOT PKG_CONFIG_FOUND)
  message(STATUS "MuPDF: pkg-config not found, bundling third-party libraries")
elseif(MUPDF_SYSTEM_LIBS)
  set(_mupdf_make_vars USE_SYSTEM_LIBS=yes)

  # USE_SYSTEM_LIBS=yes turns every library on at once, and MuPDF does not
  # check that they are actually present -- a missing one surfaces as a missing
  # header or an undefined symbol partway through the build. So test each one
  # here and bundle the ones that are absent. Makethird assigns each toggle
  # with `?=`, so these overrides win over the blanket setting.
  #
  # The predicates match MuPDF's own in Makerules, version constraints
  # included: claiming a library it will then reject would put the build right
  # back where it started.
  set(_mupdf_pkg_libs
    FREETYPE "freetype2>=18.3.12"
    GUMBO    "gumbo>=0.10.0"
    HARFBUZZ "harfbuzz>=2.0.0"
    LIBJPEG  "libjpeg"
    OPENJPEG "libopenjp2>=2.1.0"
    ZLIB     "zlib>=1.2.6"
    BROTLI   "libbrotlidec libbrotlienc>=0.6.0")

  set(_mupdf_sys "")
  set(_mupdf_bundled "")
  list(LENGTH _mupdf_pkg_libs _mupdf_n)
  math(EXPR _mupdf_last "${_mupdf_n} - 2")
  foreach(_i RANGE 0 ${_mupdf_last} 2)
    list(GET _mupdf_pkg_libs ${_i} _lib)
    math(EXPR _j "${_i} + 1")
    list(GET _mupdf_pkg_libs ${_j} _spec)
    separate_arguments(_spec)
    pkg_check_modules(_mupdf_probe QUIET ${_spec})
    if(_mupdf_probe_FOUND)
      list(APPEND _mupdf_sys ${_lib})
    else()
      list(APPEND _mupdf_bundled ${_lib})
      list(APPEND _mupdf_make_vars USE_SYSTEM_${_lib}=no)
    endif()
    unset(_mupdf_probe_FOUND CACHE)
  endforeach()

  # jbig2dec ships no pkg-config file -- MuPDF just links -ljbig2dec -- so look
  # for its header instead.
  find_path(MUPDF_JBIG2DEC_INCLUDE_DIR jbig2.h)
  mark_as_advanced(MUPDF_JBIG2DEC_INCLUDE_DIR)
  if(MUPDF_JBIG2DEC_INCLUDE_DIR)
    list(APPEND _mupdf_sys JBIG2DEC)
  else()
    list(APPEND _mupdf_bundled JBIG2DEC)
    list(APPEND _mupdf_make_vars USE_SYSTEM_JBIG2DEC=no)
  endif()

  string(REPLACE ";" " " _mupdf_sys_str "${_mupdf_sys}")
  string(REPLACE ";" " " _mupdf_bundled_str "${_mupdf_bundled}")
  message(STATUS "MuPDF system libraries: ${_mupdf_sys_str}")
  if(_mupdf_bundled)
    message(STATUS "MuPDF bundled libraries (not found): ${_mupdf_bundled_str}")
  endif()
endif()

# Pin the source directory so the paths below can be named at configure time.
set(MUPDF_SOURCE_DIR "${CMAKE_BINARY_DIR}/mupdf-src")
set(_mupdf_out "${MUPDF_SOURCE_DIR}/build/shared-release")

ExternalProject_Add(mupdf_external
  URL             "https://mupdf.com/downloads/archive/mupdf-${MUPDF_VERSION}-source.tar.gz"
  URL_HASH        ${MUPDF_URL_HASH}
  SOURCE_DIR      ${MUPDF_SOURCE_DIR}
  BUILD_IN_SOURCE ON
  CONFIGURE_COMMAND ""
  BUILD_COMMAND   sh "${CMAKE_CURRENT_LIST_DIR}/build-mupdf.sh"
                  <SOURCE_DIR> ${MUPDF_BUILD_JOBS} ${_mupdf_out}
                  ${_mupdf_make_vars}
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${_mupdf_out}/libmupdfcpp.so"
                   "${_mupdf_out}/libmupdf${CMAKE_SHARED_LIBRARY_SUFFIX}"
                   "${_mupdf_out}/mutool${CMAKE_EXECUTABLE_SUFFIX}"
  USES_TERMINAL_DOWNLOAD ON
  USES_TERMINAL_BUILD    ON)

# mutool comes along with the libraries and is useful for comparison, but it is
# buried several directories down in MuPDF's own build tree. Link it next to
# mubanal. build-mupdf.sh gives it absolute library references, so it works
# through the symlink like any other name for the file.
set(MUPDF_MUTOOL "${CMAKE_BINARY_DIR}/mutool${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(OUTPUT "${MUPDF_MUTOOL}"
  COMMAND ${CMAKE_COMMAND} -E create_symlink
          "${_mupdf_out}/mutool${CMAKE_EXECUTABLE_SUFFIX}" "${MUPDF_MUTOOL}"
  DEPENDS "${_mupdf_out}/mutool${CMAKE_EXECUTABLE_SUFFIX}"
  COMMENT "Linking mutool into ${CMAKE_BINARY_DIR}"
  VERBATIM)
add_custom_target(mupdf_mutool ALL DEPENDS "${MUPDF_MUTOOL}")

# The C++ headers are generated beside the C ones rather than merged into them.
set(MUPDF_INCLUDE_DIRS
  "${MUPDF_SOURCE_DIR}/include"
  "${MUPDF_SOURCE_DIR}/platform/c++/include")

# libmupdfcpp is named .so on every platform -- scripts/wrap/__main__.py
# hardcodes that and post-processes the result on macOS -- while libmupdf
# follows the platform convention.
set(MUPDF_LIBRARIES
  "${_mupdf_out}/libmupdfcpp.so"
  "${_mupdf_out}/libmupdf${CMAKE_SHARED_LIBRARY_SUFFIX}")

# target_include_directories rejects directories that do not yet exist, and
# these arrive with the download.
foreach(_dir IN LISTS MUPDF_INCLUDE_DIRS)
  file(MAKE_DIRECTORY "${_dir}")
endforeach()

function(mubanal_link_mupdf target)
  # The libraries are named by path above, so nothing else orders these builds.
  add_dependencies(${target} mupdf_external)
  # They stay in the build tree, which is not on the loader's search path. This
  # also resolves libmupdfcpp's own @rpath reference to libmupdf.
  set_target_properties(${target} PROPERTIES
    BUILD_RPATH "${_mupdf_out}"
    INSTALL_RPATH "${_mupdf_out}")

  if(NOT APPLE AND NOT WIN32)
    # libmupdfcpp is linked without an rpath of its own, so it depends on the
    # client's search path to find libmupdf. Modern linkers emit DT_RUNPATH,
    # which applies only to the binary's own direct dependencies and so would
    # not cover that; DT_RPATH is transitive.
    target_link_options(${target} PRIVATE "-Wl,--disable-new-dtags")
  endif()

  if(APPLE)
    # On macOS MuPDF gives both libraries a *relative* install name, which the
    # loader resolves against the working directory -- so a client that runs
    # from anywhere else fails, and no rpath can help. Retagging the libraries
    # themselves is not possible (their load commands have no room for a longer
    # path), but CMake links its own targets with -headerpad_max_install_names,
    # so rewrite the references here instead.
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND install_name_tool
        -change "build/shared-release/libmupdfcpp.so" "${_mupdf_out}/libmupdfcpp.so"
        -change "build/shared-release/libmupdf.dylib" "${_mupdf_out}/libmupdf.dylib"
        "$<TARGET_FILE:${target}>"
      COMMENT "Rewriting MuPDF library references in ${target}"
      VERBATIM)
  endif()
endfunction()
