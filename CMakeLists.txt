cmake_minimum_required(VERSION 3.16)
project(tinylsm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default for single-config generators (Make/Ninja) when the user does not pass
# -DCMAKE_BUILD_TYPE=... Multi-config generators (VS, Xcode) use their own types.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)
endif()

# Single source of truth: project(VERSION ...) → generated header for Version().
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tinylsm_version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/tinylsm_version.h
  @ONLY
)

add_library(tinylsm STATIC
  src/version.cpp
  src/status.cpp
  src/crc32c.cpp
)

target_include_directories(tinylsm
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_compile_features(tinylsm PUBLIC cxx_std_17)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  option(TINYLSM_BUILD_TESTS "Build tinylsm unit tests" ON)
else()
  option(TINYLSM_BUILD_TESTS "Build tinylsm unit tests" OFF)
endif()
if(TINYLSM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
