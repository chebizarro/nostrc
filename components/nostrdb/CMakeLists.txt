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

# nostrdb itself: reuse libnostr's in-tree 'nostrdb' target when libnostr
# was built with its nostrdb backend (LIBNOSTR_WITH_NOSTRDB=ON, dev builds);
# otherwise build a private static copy so the storage driver -- and the
# relay daemons that link it -- work while libnostr.so stays nostrdb-free
# (the packaged build, nostrc-prqu.5).
if (TARGET nostrdb)
  target_link_libraries(nostrdb_storage PUBLIC nostrdb)
elseif (NOT ALLOW_STUB_NOSTRDB)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
  include(NostrdbVendored)
  get_filename_component(_nostrc_cmake_dir "${CMAKE_CURRENT_LIST_DIR}/../../cmake" ABSOLUTE)
  include("${_nostrc_cmake_dir}/NostrcConfigureProvenance.cmake")
  set(_nostrdb_compat "${CMAKE_CURRENT_BINARY_DIR}/nostrdb_vendored_compat")
  if(NOT EXISTS "${NDB_VENDOR_PATH}/src/bolt11/talstr.h")
    configure_file("${_nostrc_cmake_dir}/nostrdb-talstr.h.in"
      "${_nostrdb_compat}/talstr.h" COPYONLY)
    nostrc_record_configure_output("${CMAKE_CURRENT_LIST_FILE}"
      "${_nostrc_cmake_dir}/nostrdb-talstr.h.in" "${_nostrdb_compat}/talstr.h")
  endif()
  set(NOSTRDB_HEX_COMPAT_COMMENT "")
  configure_file("${_nostrc_cmake_dir}/nostrdb-hex-compat.h.in"
    "${_nostrdb_compat}/hex_compat.h" @ONLY)
  nostrc_record_configure_output("${CMAKE_CURRENT_LIST_FILE}"
    "${_nostrc_cmake_dir}/nostrdb-hex-compat.h.in" "${_nostrdb_compat}/hex_compat.h")
  unset(NOSTRDB_HEX_COMPAT_COMMENT)
  nostrc_add_vendored_nostrdb(nostrdb_vendored "${NDB_VENDOR_PATH}" "${_nostrdb_compat}")
  target_link_libraries(nostrdb_storage PUBLIC nostrdb_vendored)
  message(STATUS "components/nostrdb: libnostr has no nostrdb backend; linking a private static nostrdb (nostrdb_vendored)")
endif()

# Threads may be required by LMDB or the vendored build
find_package(Threads)
if (Threads_FOUND)
  target_link_libraries(nostrdb_storage PUBLIC Threads::Threads)
endif()

target_include_directories(nostrdb_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(nostrdb_storage PRIVATE ${CMAKE_SOURCE_DIR}/apps/relayd/include)
