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
endif()

target_include_directories(nostrdb_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
