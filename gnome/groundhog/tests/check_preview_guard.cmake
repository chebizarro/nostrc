# groundhog-mls-preview-guard (review M5): the cases of
# cmake/GroundhogPreviewGuard.cmake, in script mode (cmake -P).
# Script mode sets no policies: without this, CMake 3.x (Ubuntu 24.04, the
# Linux gate) reads the guard's if(... IN_LIST ...) with CMP0057 OLD and fails.
cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/GroundhogPreviewGuard.cmake)

foreach (name IN LISTS GROUNDHOG_PREVIEW_PACKAGING_ENV)
  unset(ENV{${name}})
endforeach()

function(expect_ok)
  groundhog_preview_problem(problem ${ARGN})
  if (problem)
    message(FATAL_ERROR "refused, should build (${ARGN}): ${problem}")
  endif()
endfunction()

function(expect_refused)
  groundhog_preview_problem(problem ${ARGN})
  if (NOT problem)
    message(FATAL_ERROR "allowed, should be refused: ${ARGN}")
  endif()
endfunction()

# Development builds.
expect_ok(BUILD_TYPE Debug INSTALL_PREFIX /usr/local)
expect_ok(BUILD_TYPE "" INSTALL_PREFIX /usr/local)
expect_ok(BUILD_TYPE debug INSTALL_PREFIX /home/dev/.local)
expect_ok(CONFIGURATION_TYPES Debug INSTALL_PREFIX /usr/local)
expect_ok(BUILD_TYPE Debug INSTALL_PREFIX /usr/local/lib/usr)

# Release and packaging build types, any spelling; multi-config lists.
foreach (type Release RelWithDebInfo MinSizeRel None release RELWITHDEBINFO)
  expect_refused(BUILD_TYPE ${type} INSTALL_PREFIX /usr/local)
endforeach()
expect_refused(CONFIGURATION_TYPES Debug Release INSTALL_PREFIX /usr/local)
expect_refused(CONFIGURATION_TYPES Debug RelWithDebInfo INSTALL_PREFIX /usr/local)

# Packaging and install configurations.
expect_refused(BUILD_TYPE Debug INSTALL_PREFIX /usr/local CPACK)
foreach (prefix /usr /usr/ /app /snap/groundhog/current)
  expect_refused(BUILD_TYPE Debug INSTALL_PREFIX ${prefix})
endforeach()
foreach (name IN LISTS GROUNDHOG_PREVIEW_PACKAGING_ENV)
  set(ENV{${name}} "x")
  expect_refused(BUILD_TYPE Debug INSTALL_PREFIX /usr/local)
  unset(ENV{${name}})
endforeach()

message(STATUS "GROUNDHOG_ENCRYPTED_GROUPS_PREVIEW guard: every case as expected")
