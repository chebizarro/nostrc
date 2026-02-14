# nostr-gobject/CMakeLists.txt — CMake build for the nostr-glib-1.0 GObject layer
#
# Builds libnostr-glib-1.0, optionally generates GIR/typelib via g-ir-scanner,
# installs headers, pkg-config .pc file, and GIR artifacts.
#
# SPDX-License-Identifier: GPL-3.0-or-later

include(GNUInstallDirs)

# ── Version metadata ─────────────────────────────────────────────────
set(NOSTR_GLIB_MAJOR_VERSION 1)
set(NOSTR_GLIB_MINOR_VERSION 0)
set(NOSTR_GLIB_MICRO_VERSION 0)
set(NOSTR_GLIB_VERSION "${NOSTR_GLIB_MAJOR_VERSION}.${NOSTR_GLIB_MINOR_VERSION}.${NOSTR_GLIB_MICRO_VERSION}")

# ── Find dependencies ────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB2    REQUIRED IMPORTED_TARGET glib-2.0>=2.70)
pkg_check_modules(GOBJECT2 REQUIRED IMPORTED_TARGET gobject-2.0>=2.70)
pkg_check_modules(GIO2     REQUIRED IMPORTED_TARGET gio-2.0>=2.70)
pkg_check_modules(JSON_GLIB  QUIET IMPORTED_TARGET json-glib-1.0>=1.6)
pkg_check_modules(SECP256K1  REQUIRED IMPORTED_TARGET libsecp256k1)
pkg_check_modules(LWS        REQUIRED IMPORTED_TARGET libwebsockets)
pkg_check_modules(LIBSODIUM  QUIET IMPORTED_TARGET libsodium)

find_library(NSYNC_LIB NAMES nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR NAMES nsync.h REQUIRED)

# ── Version header (configure from template) ─────────────────────────
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/nostr-glib-version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/nostr-glib-version.h
  @ONLY)

# ── Source files ─────────────────────────────────────────────────────
# Note: nostr_ndb_store.c is excluded — it depends on app-level storage_ndb.c
set(NOSTR_GLIB_SOURCES
  src/nostr-error.c
  src/nostr-enums.c
  src/nostr_event.c
  src/nostr_event_bus.c
  src/nostr_keys.c
  src/nostr_relay.c
  src/nostr_subscription.c
  src/nostr_simple_pool.c
  src/nostr_pool.c
  src/nostr_relay_store.c
  src/nostr_store.c
  src/crypto_utils.c
  src/nostr_async.c
  src/nostr_query_batcher.c
  src/nostr_subscription_registry.c
  src/nostr_filter.c
  src/nostr_json.c
  src/nostr_pointer.c
  src/nostr_nip19.c
  src/nostr_tag_list.c
  src/nostr_nip46_client.c
  src/nostr_nip46_bunker.c
  src/nostr_nip49.c
  src/nostr_bip39.c
)

set(NOSTR_GLIB_HEADERS
  include/nostr-glib.h
  include/nostr-error.h
  include/nostr-types.h
  include/nostr-enums.h
  include/nostr_event.h
  include/nostr_event_bus.h
  include/nostr_keys.h
  include/nostr_relay.h
  include/nostr_subscription.h
  include/nostr_simple_pool.h
  include/nostr_pool.h
  include/nostr_relay_store.h
  include/nostr_store.h
  include/crypto_utils_gobject.h
  include/nostr_async.h
  include/nostr_query_batcher.h
  include/nostr_subscription_registry.h
  include/nostr_filter.h
  include/nostr_json.h
  include/nostr_pointer.h
  include/nostr_nip19.h
  include/nostr_tag_list.h
  include/nostr_nip46_client.h
  include/nostr_nip46_bunker.h
  include/nostr_nip49.h
  include/nostr_bip39.h
)

# ── Library target ───────────────────────────────────────────────────
add_library(nostr_glib ${NOSTR_GLIB_SOURCES})

set_target_properties(nostr_glib PROPERTIES
  OUTPUT_NAME nostr-glib-1.0
  VERSION     ${NOSTR_GLIB_VERSION}
  SOVERSION   ${NOSTR_GLIB_MAJOR_VERSION}
)

target_include_directories(nostr_glib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/nostr-glib-1.0>
  PRIVATE
    ${CMAKE_SOURCE_DIR}/nips/nip04/include
    ${CMAKE_SOURCE_DIR}/nips/nip06/include
    $<$<BOOL:${ENABLE_NIP11}>:${CMAKE_SOURCE_DIR}/nips/nip11/include>
    ${CMAKE_SOURCE_DIR}/nips/nip46/include
    ${NSYNC_INCLUDE_DIR}
)

# Public dependencies (propagated to consumers)
target_link_libraries(nostr_glib
  PUBLIC
    PkgConfig::GLIB2
    PkgConfig::GOBJECT2
    PkgConfig::GIO2
)

# Private dependencies (implementation only)
target_link_libraries(nostr_glib
  PRIVATE
    libnostr
    nostr_json
    libgo
    PkgConfig::SECP256K1
    PkgConfig::LWS
    OpenSSL::SSL
    OpenSSL::Crypto
    ${NSYNC_LIB}
)

# Conditionally link NIP libraries (guards handle missing targets)
foreach(_nip nip04 nip06 nip11 nip19
             nostr_nip44_core nostr_nip46_core
             nostr_nip49_core nostr_nip49_glib)
  if(TARGET ${_nip})
    target_link_libraries(nostr_glib PRIVATE ${_nip})
  endif()
endforeach()

if(JSON_GLIB_FOUND)
  target_link_libraries(nostr_glib PRIVATE PkgConfig::JSON_GLIB)
endif()
if(LIBSODIUM_FOUND)
  target_link_libraries(nostr_glib PRIVATE PkgConfig::LIBSODIUM)
endif()
if(ENABLE_NIP11)
  target_compile_definitions(nostr_glib PRIVATE ENABLE_NIP11)
endif()

# Inherit sanitizer flags from root CMakeLists.txt
apply_sanitizers(nostr_glib)

# ── GObject Introspection ────────────────────────────────────────────
option(NOSTR_GLIB_ENABLE_GIR "Generate GNostr-1.0 GIR and typelib" OFF)

if(NOSTR_GLIB_ENABLE_GIR)
  find_program(G_IR_SCANNER  g-ir-scanner)
  find_program(G_IR_COMPILER g-ir-compiler)

  if(G_IR_SCANNER AND G_IR_COMPILER)
    # g-ir-scanner requires a shared library. If nostr_glib is static,
    # create a thin shared wrapper (same pattern as root NOSTR_ENABLE_GI).
    get_target_property(_glib_type nostr_glib TYPE)
    if(_glib_type STREQUAL "STATIC_LIBRARY")
      set(_gir_wrapper_dir "${CMAKE_CURRENT_BINARY_DIR}/gir_wrappers")
      file(MAKE_DIRECTORY ${_gir_wrapper_dir})
      file(WRITE "${_gir_wrapper_dir}/dummy.c"
           "/* GIR shared wrapper */\nint _nostr_glib_gir_dummy(void){return 0;}\n")
      add_library(nostr_glib_shared SHARED "${_gir_wrapper_dir}/dummy.c")
      target_link_libraries(nostr_glib_shared PRIVATE nostr_glib)
      if(APPLE)
        target_link_options(nostr_glib_shared PRIVATE
          "-Wl,-force_load,$<TARGET_FILE:nostr_glib>")
      else()
        target_link_options(nostr_glib_shared PRIVATE
          -Wl,--whole-archive $<TARGET_FILE:nostr_glib> -Wl,--no-whole-archive)
      endif()
      set(_gir_lib_name nostr_glib_shared)
      set(_gir_lib_dep  nostr_glib_shared)
    else()
      set(_gir_lib_name nostr_glib)
      set(_gir_lib_dep  nostr_glib)
    endif()

    # Collect absolute paths for scanner
    set(_gir_input "")
    foreach(_hdr ${NOSTR_GLIB_HEADERS})
      list(APPEND _gir_input "${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}")
    endforeach()
    foreach(_src ${NOSTR_GLIB_SOURCES})
      list(APPEND _gir_input "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
    endforeach()

    set(_gir_dir  "${CMAKE_CURRENT_BINARY_DIR}/gir")
    set(_gir_file "${_gir_dir}/GNostr-1.0.gir")
    set(_typelib  "${_gir_dir}/GNostr-1.0.typelib")

    add_custom_command(
      OUTPUT ${_gir_file} ${_typelib}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_gir_dir}
      COMMAND ${CMAKE_COMMAND} -E env
        GI_SCANNER_DISABLE_CACHE=1
        "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:${_gir_lib_dep}>:${CMAKE_BINARY_DIR}/libnostr:${CMAKE_BINARY_DIR}/libjson:$ENV{LD_LIBRARY_PATH}"
        "DYLD_FALLBACK_LIBRARY_PATH=$<TARGET_FILE_DIR:${_gir_lib_dep}>:${CMAKE_BINARY_DIR}/libnostr:${CMAKE_BINARY_DIR}/libjson:$ENV{DYLD_FALLBACK_LIBRARY_PATH}"
        ${G_IR_SCANNER}
          --namespace=GNostr
          --nsversion=1.0
          --symbol-prefix=gnostr
          --identifier-prefix=GNostr
          --warn-all
          --library ${_gir_lib_name}
          --library-path $<TARGET_FILE_DIR:${_gir_lib_dep}>
          --library-path ${CMAKE_BINARY_DIR}/libnostr
          --library-path ${CMAKE_BINARY_DIR}/libjson
          --pkg glib-2.0 --pkg gobject-2.0 --pkg gio-2.0
          -I ${CMAKE_CURRENT_SOURCE_DIR}/include
          -I ${CMAKE_CURRENT_BINARY_DIR}
          -I ${CMAKE_SOURCE_DIR}/libnostr/include
          -I ${CMAKE_BINARY_DIR}/libnostr/generated
          -I ${CMAKE_SOURCE_DIR}/libgo/include
          $<$<BOOL:${NSYNC_INCLUDE_DIR}>:-I${NSYNC_INCLUDE_DIR}>
          --include=GLib-2.0
          --include=GObject-2.0
          --include=Gio-2.0
          --output ${_gir_file}
          ${_gir_input}
      COMMAND ${G_IR_COMPILER} ${_gir_file} -o ${_typelib}
      DEPENDS ${_gir_lib_dep}
      COMMENT "Generating GNostr-1.0 GIR and typelib"
      VERBATIM
    )

    add_custom_target(nostr-glib-gir ALL DEPENDS ${_typelib})

    install(FILES ${_gir_file}
            DESTINATION ${CMAKE_INSTALL_DATADIR}/gir-1.0)
    install(FILES ${_typelib}
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/girepository-1.0)
  else()
    message(WARNING "g-ir-scanner/g-ir-compiler not found; GIR generation disabled")
  endif()
endif()

# ── pkg-config ───────────────────────────────────────────────────────
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nostr-glib-1.0.pc"
"prefix=${CMAKE_INSTALL_PREFIX}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}
includedir=\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}

Name: nostr-glib-1.0
Description: GObject wrapper for libnostr — Nostr protocol library
Version: ${NOSTR_GLIB_VERSION}
Requires: glib-2.0 gobject-2.0 gio-2.0
Requires.private: openssl libsecp256k1
Libs: -L\${libdir} -lnostr-glib-1.0
Cflags: -I\${includedir}/nostr-glib-1.0
")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostr-glib-1.0.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ── Installation ─────────────────────────────────────────────────────
install(TARGETS nostr_glib
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES ${NOSTR_GLIB_HEADERS}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr-glib-1.0)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostr-glib-version.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr-glib-1.0)

# ── Tests ────────────────────────────────────────────────────────────
if(BUILD_TESTING)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nostr_event.c")
    add_executable(test_nostr_glib_event tests/test_nostr_event.c)
    target_link_libraries(test_nostr_glib_event PRIVATE nostr_glib)
    apply_sanitizers(test_nostr_glib_event)
    add_test(NAME nostr_glib_event COMMAND test_nostr_glib_event)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip19.c")
    add_executable(test_nostr_glib_nip19 tests/test_nip19.c)
    target_link_libraries(test_nostr_glib_nip19 PRIVATE nostr_glib)
    apply_sanitizers(test_nostr_glib_nip19)
    add_test(NAME nostr_glib_nip19 COMMAND test_nostr_glib_nip19)
  endif()
endif()
