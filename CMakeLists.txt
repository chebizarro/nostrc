cmake_minimum_required(VERSION 3.10)
project(nostrc)

# Enable CTest early so all subdirectories can register tests (including NIPs)
enable_testing()

# Top-level build for the nostrc monorepo.
# This project builds multiple libraries:
#  - libgo: Go-like concurrency primitives in C (channels, contexts, etc.)
#  - libnostr: Core Nostr protocol types and helpers
#  - libjson: JSON interop layer based on jansson
# It also configures tests under `tests/` via CTest.

# Options to enable/disable NIPs and NSON
# Note: NipOptions.cmake may define per-NIP toggles if present. It is optional.
include(${CMAKE_SOURCE_DIR}/NipOptions.cmake)

# Toggle optional NSON support (custom serialization). When ON, the `nson/` subdir
# may be built and linked by consumers that need it.
option(ENABLE_NSON "Enable NSON compilation" OFF)

# (enable_testing already called at the top)

# Build order ensures libgo is available for libnostr/libjson as needed.
add_subdirectory(libgo)
add_subdirectory(libnostr)
add_subdirectory(libjson)

# Add test targets from `tests/`.
add_subdirectory(tests)

# Sanitizer helper (Debug builds)
function(apply_sanitizers target)
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    if (GO_ENABLE_TSAN)
      target_compile_options(${target} PRIVATE -fsanitize=thread)
      target_link_options(${target} PRIVATE -fsanitize=thread)
    else()
      if (GO_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address)
        target_link_options(${target} PRIVATE -fsanitize=address)
      endif()
      if (GO_ENABLE_UBSAN)
        target_compile_options(${target} PRIVATE -fsanitize=undefined)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
      endif()
    endif()
  endif()
endfunction()

# Examples
add_executable(relay_smoke examples/relay_smoke.c)
target_link_libraries(relay_smoke PRIVATE libnostr nostr_json)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)
target_include_directories(relay_smoke PRIVATE ${NSYNC_INCLUDE_DIR})
find_library(NSYNC_LIB nsync REQUIRED)
target_link_libraries(relay_smoke PRIVATE ${NSYNC_LIB})
apply_sanitizers(relay_smoke)

## GLib demo: only if libnostr built with GLib support
if (TARGET nostr)
  get_target_property(_nostr_defs nostr COMPILE_DEFINITIONS)
  if (NOSTR_WITH_GLIB)
    # Try to reuse GLib pkg from libnostr configuration
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLIB QUIET IMPORTED_TARGET glib-2.0>=2.50 gobject-2.0>=2.50)
    if (GLIB_FOUND)
      add_executable(json_provider_demo examples/json_provider_demo.c)
      target_link_libraries(json_provider_demo PRIVATE libnostr nostr_json PkgConfig::GLIB)
      target_compile_definitions(json_provider_demo PRIVATE NOSTR_ENABLE_LEGACY_ALIASES=0)
      target_include_directories(json_provider_demo PRIVATE 
        libnostr/include 
        ${CMAKE_BINARY_DIR}/libnostr/generated)
      # nsync dependency required via libgo headers used transitively
      find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)
      target_include_directories(json_provider_demo PRIVATE ${NSYNC_INCLUDE_DIR})
      find_library(NSYNC_LIB nsync REQUIRED)
      target_link_libraries(json_provider_demo PRIVATE ${NSYNC_LIB})
      apply_sanitizers(json_provider_demo)
    endif()
  endif()
endif()


# Optional GObject Introspection generation
option(NOSTR_ENABLE_GI "Enable GIR/typelib generation target" ON)
if (NOSTR_ENABLE_GI)
  find_package(PkgConfig REQUIRED)
  # Ensure our pkg-config files are available at build time by adding build-time pc dirs if needed
  # Consumers typically run this after an install, but for convenience we'll use build artifacts too.
  find_program(G_IR_SCANNER g-ir-scanner)
  find_program(G_IR_COMPILER g-ir-compiler)
  if (G_IR_SCANNER AND G_IR_COMPILER)
    # Includes needed for headers referenced by libnostr public headers
    find_path(NSYNC_INCLUDE_DIR nsync.h)
    add_custom_target(nostr-gir
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gir
      COMMAND ${CMAKE_COMMAND} -E env GI_SCANNER_DISABLE_CACHE=1
        ${G_IR_SCANNER}
        --cflags-begin
        -std=gnu11
        -DNOSTR_WITH_GLIB=1
        --cflags-end
        --namespace=Nostr
        --nsversion=1.0
        --symbol-prefix=nostr_
        --identifier-prefix=Nostr
        --warn-all
        --library nostr
        --library nostr_json
        --library-path ${CMAKE_BINARY_DIR}/libnostr
        --library-path ${CMAKE_BINARY_DIR}/libjson
        --pkg glib-2.0 --pkg gobject-2.0
        --pkg jansson --pkg libwebsockets --pkg openssl --pkg libsecp256k1
        -I ${CMAKE_SOURCE_DIR}/libnostr/include
        -I ${CMAKE_SOURCE_DIR}/libgo/include
        $<$<BOOL:${NSYNC_INCLUDE_DIR}>:-I${NSYNC_INCLUDE_DIR}>
        -I ${CMAKE_BINARY_DIR}/libnostr/generated
        ${CMAKE_SOURCE_DIR}/libnostr/include/nostr-utils.h
        ${CMAKE_SOURCE_DIR}/libnostr/include/nostr-event.h
        ${CMAKE_SOURCE_DIR}/libnostr/include/nostr-filter.h
        --output ${CMAKE_BINARY_DIR}/gir/Nostr-1.0.gir
      COMMAND ${G_IR_COMPILER} ${CMAKE_BINARY_DIR}/gir/Nostr-1.0.gir -o ${CMAKE_BINARY_DIR}/gir/Nostr-1.0.typelib
      DEPENDS nostr nostr_json
      VERBATIM
    )
  endif()
endif()

