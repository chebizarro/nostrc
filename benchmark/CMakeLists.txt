# benchmark/CMakeLists.txt — Performance and memory benchmarks
#
# These are NOT run during normal `ctest` runs. They're gated behind
# BUILD_BENCHMARKS and intended for nightly CI or manual profiling.
#
# SPDX-License-Identifier: GPL-3.0-or-later

option(BUILD_BENCHMARKS "Build performance and memory benchmarks" OFF)

if(BUILD_BENCHMARKS)
  include(${CMAKE_SOURCE_DIR}/cmake/GnTest.cmake)

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK4     REQUIRED IMPORTED_TARGET gtk4>=4.6)
  pkg_check_modules(GLIB2    REQUIRED IMPORTED_TARGET glib-2.0>=2.70)
  pkg_check_modules(GOBJECT2 REQUIRED IMPORTED_TARGET gobject-2.0>=2.70)

  # Memory timeline scroll benchmark
  add_executable(gnostr_bench_memory_timeline_scroll
    gnostr_bench_memory_timeline_scroll.c
  )
  target_link_libraries(gnostr_bench_memory_timeline_scroll PRIVATE
    PkgConfig::GTK4
    PkgConfig::GLIB2
    PkgConfig::GOBJECT2
  )

  # Register as a CTest benchmark with longer timeout
  # Use xvfb-run on Linux for headless rendering
  find_program(XVFB_RUN xvfb-run)
  if(XVFB_RUN)
    add_test(NAME bench_memory_timeline_scroll
             COMMAND ${XVFB_RUN} -a $<TARGET_FILE:gnostr_bench_memory_timeline_scroll>
                     --n-events 10000 --max-rss-mb 1024)
  elseif(APPLE)
    add_test(NAME bench_memory_timeline_scroll
             COMMAND gnostr_bench_memory_timeline_scroll
                     --n-events 10000 --max-rss-mb 1024)
  endif()

  if(TARGET bench_memory_timeline_scroll)
    set_tests_properties(bench_memory_timeline_scroll PROPERTIES
      TIMEOUT 300
      LABELS "benchmark;memory;nightly"
    )
  endif()
endif()
