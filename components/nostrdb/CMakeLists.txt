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
      ${NDB_VENDOR_PATH}/deps/flatcc/include
      ${NDB_VENDOR_PATH}/deps/flatcc/src
    )
  endif()
else()
  message(WARNING "ALLOW_STUB_NOSTRDB=ON: building nostrdb storage stub without vendored third_party/nostrdb")
endif()

add_library(nostrdb_storage STATIC
  src/nostrdb_storage.c
)

target_link_libraries(nostrdb_storage PUBLIC nostr)

# If the in-tree CMake target 'nostrdb' exists, link against it.
# Avoid linking vendored static archives here to prevent duplicate objects when
# 'nostrdb' is also built in-tree by libnostr.
if (TARGET nostrdb)
  target_link_libraries(nostrdb_storage PUBLIC nostrdb)
else()
  message(STATUS "components/nostrdb: target 'nostrdb' not found; skipping vendored lib linkage to avoid duplicate symbols. Ensure LIBNOSTR_WITH_NOSTRDB=ON in the root build.")
endif()

# Threads may be required by LMDB or the vendored build
find_package(Threads)
if (Threads_FOUND)
  target_link_libraries(nostrdb_storage PUBLIC Threads::Threads)
endif()

target_include_directories(nostrdb_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
