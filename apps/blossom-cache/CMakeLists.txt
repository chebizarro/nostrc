cmake_minimum_required(VERSION 3.22)
project(blossom-cache C)

include(GNUInstallDirs)

set(APP_NAME "blossom-cache")
set(APP_ID "org.gnostr.BlossomCache")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.56 gobject-2.0>=2.56 gio-2.0>=2.56)
pkg_check_modules(SOUP3 REQUIRED IMPORTED_TARGET libsoup-3.0)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

# ---- Optional LMDB backend ----
# Look for LMDB in nostrdb's vendored copy first, then system
set(_NDB_LMDB_DIR "${CMAKE_SOURCE_DIR}/third_party/nostrdb/deps/lmdb")
option(ENABLE_LMDB_BACKEND "Build with optional LMDB metadata backend" ON)

set(HAVE_LMDB FALSE)
if(ENABLE_LMDB_BACKEND)
  if(EXISTS "${_NDB_LMDB_DIR}/lmdb.h")
    message(STATUS "blossom-cache: Found LMDB in nostrdb vendor (${_NDB_LMDB_DIR})")
    set(HAVE_LMDB TRUE)
    set(LMDB_INCLUDE_DIR "${_NDB_LMDB_DIR}")
    # Build LMDB from source (same as gnostr does)
    if(EXISTS "${_NDB_LMDB_DIR}/mdb.c")
      add_library(blossom-cache-lmdb STATIC
        "${_NDB_LMDB_DIR}/mdb.c"
        "${_NDB_LMDB_DIR}/midl.c"
      )
      target_include_directories(blossom-cache-lmdb PUBLIC "${_NDB_LMDB_DIR}")
      set(LMDB_LIBRARY blossom-cache-lmdb)
    elseif(EXISTS "${_NDB_LMDB_DIR}/liblmdb.a")
      set(LMDB_LIBRARY "${_NDB_LMDB_DIR}/liblmdb.a")
    endif()
  else()
    pkg_check_modules(LMDB QUIET IMPORTED_TARGET lmdb)
    if(LMDB_FOUND)
      message(STATUS "blossom-cache: Found system LMDB")
      set(HAVE_LMDB TRUE)
      set(LMDB_LIBRARY PkgConfig::LMDB)
    else()
      message(STATUS "blossom-cache: LMDB not found — using SQLite-only backend")
    endif()
  endif()
endif()

# Source files
set(BLOSSOM_CACHE_SOURCES
  src/main.c
  src/bc-application.c
  src/bc-blob-store.c
  src/bc-db-backend.c
  src/bc-db-backend-sqlite.c
  src/bc-db-backend-lmdb.c
  src/bc-http-server.c
  src/bc-cache-manager.c
  src/bc-upstream-client.c
)

add_executable(${APP_NAME} ${BLOSSOM_CACHE_SOURCES})

target_compile_features(${APP_NAME} PRIVATE c_std_11)
target_compile_definitions(${APP_NAME} PRIVATE
  APP_ID="${APP_ID}"
  APP_NAME="${APP_NAME}"
  G_LOG_DOMAIN="blossom-cache"
)

target_include_directories(${APP_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(${APP_NAME} PRIVATE
  PkgConfig::GLIB
  PkgConfig::SOUP3
  PkgConfig::JSON_GLIB
  PkgConfig::SQLITE3
)

# Link LMDB if available
if(HAVE_LMDB)
  target_link_libraries(${APP_NAME} PRIVATE ${LMDB_LIBRARY})
  if(LMDB_INCLUDE_DIR)
    target_include_directories(${APP_NAME} PRIVATE ${LMDB_INCLUDE_DIR})
  endif()
endif()

# Apply sanitizer link flags when available
if(COMMAND apply_sanitizers)
  apply_sanitizers(${APP_NAME})
endif()

install(TARGETS ${APP_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- GSettings schema compilation (development builds) ----
set(BC_SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data/schemas)
set(BC_SCHEMA_FILE ${BC_SCHEMA_DIR}/${APP_ID}.gschema.xml)
set(BC_SCHEMA_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gschemas.compiled)

find_program(GLIB_COMPILE_SCHEMAS glib-compile-schemas)
if(GLIB_COMPILE_SCHEMAS AND EXISTS ${BC_SCHEMA_FILE})
  add_custom_command(
    OUTPUT ${BC_SCHEMA_OUTPUT}
    COMMAND ${GLIB_COMPILE_SCHEMAS}
            ${BC_SCHEMA_DIR}
            --targetdir=${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${BC_SCHEMA_FILE}
    COMMENT "Compiling GSettings schemas for blossom-cache"
    VERBATIM
  )
  add_custom_target(blossom-cache-schemas ALL DEPENDS ${BC_SCHEMA_OUTPUT})
  add_dependencies(${APP_NAME} blossom-cache-schemas)
endif()

# ---- GSettings schema install ----
install(FILES ${BC_SCHEMA_FILE}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)
if(GLIB_COMPILE_SCHEMAS)
  install(CODE "execute_process(COMMAND ${GLIB_COMPILE_SCHEMAS} \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)")
endif()

# ---- Tests ----
if(BUILD_TESTING)
  add_executable(test-bc-blob-store
    tests/test_blob_store.c
    src/bc-blob-store.c
    src/bc-db-backend.c
    src/bc-db-backend-sqlite.c
  )
  target_compile_features(test-bc-blob-store PRIVATE c_std_11)
  target_compile_definitions(test-bc-blob-store PRIVATE G_LOG_DOMAIN="test-blob-store")
  target_include_directories(test-bc-blob-store PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(test-bc-blob-store PRIVATE
    PkgConfig::GLIB PkgConfig::SQLITE3 PkgConfig::JSON_GLIB)
  add_test(NAME test-bc-blob-store COMMAND test-bc-blob-store)
  set_tests_properties(test-bc-blob-store PROPERTIES
    TIMEOUT 30
    LABELS "blossom-cache;blob-store;unit")
endif()
