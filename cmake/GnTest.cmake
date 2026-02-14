# cmake/GnTest.cmake — Shared test helper macros for GNostr projects
#
# Provides two functions:
#   gn_add_gtest(NAME ... SOURCES ... LIBS ... [ENV ...])
#   gn_add_gtest_xvfb(NAME ... SOURCES ... LIBS ... [ENV ...])
#
# Both functions:
#   - Create an executable from SOURCES
#   - Link against LIBS
#   - Set G_DEBUG=fatal-warnings,gc-friendly and G_SLICE=always-malloc
#   - Register with CTest
#   - Apply sanitizers if the apply_sanitizers() function is available
#
# The _xvfb variant wraps the test command in xvfb-run for headless GTK tests.
#
# SPDX-License-Identifier: GPL-3.0-or-later

# ── Standard GLib test (no display required) ─────────────────────────
function(gn_add_gtest NAME)
  cmake_parse_arguments(ARG "" "TIMEOUT" "SOURCES;LIBS;ENV" ${ARGN})

  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "gn_add_gtest(${NAME}): SOURCES is required")
  endif()

  add_executable(${NAME} ${ARG_SOURCES})
  target_link_libraries(${NAME} PRIVATE ${ARG_LIBS})

  add_test(NAME ${NAME} COMMAND ${NAME})

  # Base environment for all GLib tests
  set(_env
    "G_DEBUG=fatal-warnings,gc-friendly"
    "G_SLICE=always-malloc"
  )
  if(ARG_ENV)
    list(APPEND _env ${ARG_ENV})
  endif()

  set_tests_properties(${NAME} PROPERTIES ENVIRONMENT "${_env}")

  # Default timeout 60s; override with TIMEOUT param
  if(ARG_TIMEOUT)
    set_tests_properties(${NAME} PROPERTIES TIMEOUT ${ARG_TIMEOUT})
  else()
    set_tests_properties(${NAME} PROPERTIES TIMEOUT 60)
  endif()

  # Apply sanitizers if the function exists (defined in root CMakeLists.txt)
  if(COMMAND apply_sanitizers)
    apply_sanitizers(${NAME})
  endif()
endfunction()

# ── GTK widget test (requires Xvfb for headless rendering) ───────────
function(gn_add_gtest_xvfb NAME)
  cmake_parse_arguments(ARG "" "TIMEOUT" "SOURCES;LIBS;ENV" ${ARGN})

  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "gn_add_gtest_xvfb(${NAME}): SOURCES is required")
  endif()

  add_executable(${NAME} ${ARG_SOURCES})
  target_link_libraries(${NAME} PRIVATE ${ARG_LIBS})

  # Try to find xvfb-run; if not available, register as a normal test
  # (will skip on macOS where Xvfb isn't needed for GTK)
  find_program(XVFB_RUN xvfb-run)

  if(XVFB_RUN)
    add_test(NAME ${NAME}
             COMMAND ${XVFB_RUN} -a $<TARGET_FILE:${NAME}>)
  elseif(APPLE)
    # macOS doesn't need Xvfb — GTK can use the native backend
    add_test(NAME ${NAME} COMMAND ${NAME})
  else()
    message(WARNING "xvfb-run not found; widget test '${NAME}' may fail without a display")
    add_test(NAME ${NAME} COMMAND ${NAME})
  endif()

  # Environment: GLib hardening + GDK backend for Xvfb
  set(_env
    "G_DEBUG=fatal-warnings,gc-friendly"
    "G_SLICE=always-malloc"
  )
  # Only set GDK_BACKEND on Linux where Xvfb provides X11
  if(NOT APPLE)
    list(APPEND _env "GDK_BACKEND=x11")
  endif()
  if(ARG_ENV)
    list(APPEND _env ${ARG_ENV})
  endif()

  set_tests_properties(${NAME} PROPERTIES ENVIRONMENT "${_env}")

  # Widget tests may be slower; default 90s timeout
  if(ARG_TIMEOUT)
    set_tests_properties(${NAME} PROPERTIES TIMEOUT ${ARG_TIMEOUT})
  else()
    set_tests_properties(${NAME} PROPERTIES TIMEOUT 90)
  endif()

  # Label for CI filtering
  set_tests_properties(${NAME} PROPERTIES LABELS "gtk;widget")

  if(COMMAND apply_sanitizers)
    apply_sanitizers(${NAME})
  endif()
endfunction()
