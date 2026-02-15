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

  # ── Real NDB memory benchmark ──────────────────────────────────────
  # Uses real nostrdb with realistic content instead of GtkStringObject mocks.
  # Requires testkit + nostr-gobject + app model.
  if(TARGET gnostr-testkit)
    set(_NDB_VENDOR_DIR ${CMAKE_SOURCE_DIR}/third_party/nostrdb)
    set(_NOSTR_CONFIG_DIR ${CMAKE_BINARY_DIR})

    add_executable(gnostr_bench_real_memory
      gnostr_bench_real_memory.c
      ${CMAKE_SOURCE_DIR}/apps/gnostr/src/model/gn-nostr-event-model.c
      ${CMAKE_SOURCE_DIR}/apps/gnostr/src/model/gn-nostr-event-item.c
      ${CMAKE_SOURCE_DIR}/apps/gnostr/src/util/content_renderer.c
    )
    target_link_libraries(gnostr_bench_real_memory PRIVATE
      gnostr-testkit
      nostr_gobject
      nostr_gtk
      PkgConfig::GTK4
      PkgConfig::GLIB2
      PkgConfig::GOBJECT2
    )
    target_include_directories(gnostr_bench_real_memory PRIVATE
      ${CMAKE_SOURCE_DIR}/apps/gnostr/src
      ${CMAKE_SOURCE_DIR}/apps/gnostr/src/model
      ${CMAKE_SOURCE_DIR}/nostr-gobject/include
      ${CMAKE_SOURCE_DIR}/nostr-gtk/include
      ${CMAKE_SOURCE_DIR}/libnostr/include
      ${_NOSTR_CONFIG_DIR}
      ${CMAKE_SOURCE_DIR}/libgo/include
      ${_NDB_VENDOR_DIR}/src
      ${_NDB_VENDOR_DIR}/ccan
      ${_NDB_VENDOR_DIR}/deps/flatcc/include)
    target_compile_definitions(gnostr_bench_real_memory PRIVATE GNOSTR_TESTING)

    if(NOT APPLE)
      target_link_options(gnostr_bench_real_memory PRIVATE
        -Wl,--unresolved-symbols=ignore-in-object-files)
    endif()
    if(TARGET nostrdb_storage)
      target_link_libraries(gnostr_bench_real_memory PRIVATE nostrdb_storage)
      if(NOT TARGET nostrdb AND EXISTS ${_NDB_VENDOR_DIR}/libnostrdb.a)
        if(APPLE)
          target_link_options(gnostr_bench_real_memory PRIVATE
            -Wl,-force_load,${_NDB_VENDOR_DIR}/libnostrdb.a)
          if(EXISTS ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
            target_link_libraries(gnostr_bench_real_memory PRIVATE
              ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
          endif()
          target_link_libraries(gnostr_bench_real_memory PRIVATE "-framework Security")
        else()
          target_link_options(gnostr_bench_real_memory PRIVATE
            -Wl,--whole-archive ${_NDB_VENDOR_DIR}/libnostrdb.a -Wl,--no-whole-archive)
          if(EXISTS ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
            target_link_libraries(gnostr_bench_real_memory PRIVATE
              ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
          endif()
        endif()
      endif()
    endif()

    if(XVFB_RUN)
      add_test(NAME bench_real_memory
               COMMAND ${XVFB_RUN} -a $<TARGET_FILE:gnostr_bench_real_memory>
                       --n-events 10000 --max-rss-mb 1024)
    elseif(APPLE)
      add_test(NAME bench_real_memory
               COMMAND gnostr_bench_real_memory
                       --n-events 10000 --max-rss-mb 1024)
    endif()

    if(TEST bench_real_memory)
      set_tests_properties(bench_real_memory PROPERTIES
        TIMEOUT 600
        LABELS "benchmark;memory;ndb;real;nightly"
      )
    endif()
  endif()
endif()
