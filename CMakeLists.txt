cmake_minimum_required(VERSION 3.16)
project(mubanal CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Most distributions package only MuPDF's C library, not the C++ bindings this
# needs. Turn this on to fetch and build MuPDF instead of locating one.
option(MUPDF_BUILD "Download and build MuPDF with C++ bindings" ON)

if(MUPDF_BUILD)
  include(BuildMuPDF)
else()
  find_package(MuPDF REQUIRED)
endif()
# jsoncpp is used only by the --dump-runs/--runs diagnostic path. Which of its
# two discovery files a distribution ships varies, so accept either: the CMake
# package config (target name differs by version) or pkg-config.
find_package(jsoncpp CONFIG QUIET)
if(TARGET JsonCpp::JsonCpp)
  set(MUBANAL_JSON_LIBRARY JsonCpp::JsonCpp)
elseif(TARGET jsoncpp_lib)
  set(MUBANAL_JSON_LIBRARY jsoncpp_lib)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
  set(MUBANAL_JSON_LIBRARY PkgConfig::JSONCPP)
endif()

# MuPDF's generated C++ headers declare some destructors only #ifndef NDEBUG, so
# the consumer and libmupdfcpp must agree or the link fails with missing
# destructors. Release MuPDF builds define NDEBUG; flip this off if you built
# MuPDF with debugging enabled.
option(MUPDF_NDEBUG "libmupdfcpp was built with NDEBUG (a release build)" ON)

add_executable(mubanal
  src/main.cc
  src/util.cc
  src/extract.cc
  src/runs.cc
  src/analyze.cc
  src/report.cc)

target_include_directories(mubanal PRIVATE src ${MUPDF_INCLUDE_DIRS})
target_link_libraries(mubanal PRIVATE ${MUPDF_LIBRARIES} ${MUBANAL_JSON_LIBRARY})

if(MUPDF_BUILD)
  mubanal_link_mupdf(mubanal)
endif()

if(MUPDF_NDEBUG)
  target_compile_definitions(mubanal PRIVATE NDEBUG)
endif()

if(NOT MSVC)
  target_compile_options(mubanal PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

install(TARGETS mubanal RUNTIME DESTINATION bin)
