project(nostrdb_storage C)

include_directories(
  ${CMAKE_SOURCE_DIR}/libnostr/include
  include
)

# Require vendored nostrdb by default (no fake stub). Set ALLOW_STUB_NOSTRDB=ON to bypass.
option(ALLOW_STUB_NOSTRDB "Allow building nostrdb storage without vendored third_party/nostrdb" OFF)
set(NDB_VENDOR_PATH "${CMAKE_SOURCE_DIR}/third_party/nostrdb")
if (NOT ALLOW_STUB_NOSTRDB)
  if (NOT EXISTS "${NDB_VENDOR_PATH}")
    message(FATAL_ERROR "third_party/nostrdb not found at ${NDB_VENDOR_PATH}. Please clone the real nostrdb vendored repo.")
  else()
    message(STATUS "components/nostrdb: using vendored sources at ${NDB_VENDOR_PATH}")
    include_directories(
      ${NDB_VENDOR_PATH}/src
      ${NDB_VENDOR_PATH}/ccan
      ${NDB_VENDOR_PATH}/ccan/ccan
      ${NDB_VENDOR_PATH}/ccan/ccan/array_size
      ${NDB_VENDOR_PATH}/ccan/ccan/mem
      ${NDB_VENDOR_PATH}/ccan/ccan/short_types
      ${NDB_VENDOR_PATH}/ccan/ccan/crypto/sha256
      ${NDB_VENDOR_PATH}/ccan/ccan/tal
      ${NDB_VENDOR_PATH}/ccan/ccan/tal/str
      ${NDB_VENDOR_PATH}/deps/lmdb
    )
  endif()
else()
  message(WARNING "ALLOW_STUB_NOSTRDB=ON: building nostrdb storage stub without vendored third_party/nostrdb")
endif()

add_library(nostrdb_storage STATIC
  src/nostrdb_storage.c
)

target_link_libraries(nostrdb_storage PUBLIC nostr)

# If the vendored 'nostrdb' static library target exists (built elsewhere), link it too.
if (TARGET nostrdb)
  target_link_libraries(nostrdb_storage PUBLIC nostrdb)
else()
  # Otherwise, if a prebuilt static archive exists in the vendored dir, link it directly
  set(NDB_VENDOR_LIB "${NDB_VENDOR_PATH}/libnostrdb.a")
  if (EXISTS ${NDB_VENDOR_LIB})
    if (APPLE)
      # Force load the archive so its objects (and constructors) are retained
      target_link_libraries(nostrdb_storage PUBLIC -Wl,-force_load,${NDB_VENDOR_LIB})
      # Also propagate to dependents so final link line includes the archive
      target_link_libraries(nostrdb_storage INTERFACE -Wl,-force_load,${NDB_VENDOR_LIB})
    else()
      # Whole-archive on ELF to keep all objects referenced by nostrdb_storage
      target_link_libraries(nostrdb_storage PUBLIC -Wl,--whole-archive ${NDB_VENDOR_LIB} -Wl,--no-whole-archive)
      target_link_libraries(nostrdb_storage INTERFACE -Wl,--whole-archive ${NDB_VENDOR_LIB} -Wl,--no-whole-archive)
    endif()
    # Propagate include dirs so dependents can include <nostrdb.h>
    target_include_directories(nostrdb_storage PUBLIC ${NDB_VENDOR_PATH}/src ${NDB_VENDOR_PATH}/ccan ${NDB_VENDOR_PATH}/deps/lmdb)
    # Link vendored LMDB static archive if present
    set(NDB_VENDOR_LMDB_LIB "${NDB_VENDOR_PATH}/deps/lmdb/liblmdb.a")
    if (EXISTS ${NDB_VENDOR_LMDB_LIB})
      if (APPLE)
        target_link_libraries(nostrdb_storage PUBLIC ${NDB_VENDOR_LMDB_LIB})
        target_link_libraries(nostrdb_storage INTERFACE ${NDB_VENDOR_LMDB_LIB})
      else()
        target_link_libraries(nostrdb_storage PUBLIC -Wl,--whole-archive ${NDB_VENDOR_LMDB_LIB} -Wl,--no-whole-archive)
        target_link_libraries(nostrdb_storage INTERFACE -Wl,--whole-archive ${NDB_VENDOR_LMDB_LIB} -Wl,--no-whole-archive)
      endif()
    endif()
    # On macOS, nostrdb uses SecRandomCopyBytes; link the Security framework
    if (APPLE)
      target_link_options(nostrdb_storage PUBLIC "SHELL:-framework Security")
      target_link_options(nostrdb_storage INTERFACE "SHELL:-framework Security")
    endif()
  else()
    message(WARNING "components/nostrdb: vendored libnostrdb.a not found; symbols may be unresolved")
  endif()
endif()

# Threads may be required by LMDB or the vendored build
find_package(Threads)
if (Threads_FOUND)
  target_link_libraries(nostrdb_storage PUBLIC Threads::Threads)
endif()

target_include_directories(nostrdb_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
