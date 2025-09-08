project(grelay C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.56 gobject-2.0>=2.56 gio-2.0>=2.56)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)

add_executable(grelay
  src/grelay_main.c
)

target_link_libraries(grelay
  PRIVATE
    PkgConfig::GLIB
    PkgConfig::SOUP
    nostr
    nostr_json
    libgo
)

target_include_directories(grelay PRIVATE 
  ${CMAKE_SOURCE_DIR}/libnostr/include
)

# Ensure nostrdb storage is linked so its constructor can register the driver
if (TARGET nostrdb_storage)
  target_link_libraries(grelay PRIVATE nostrdb_storage)
  if (APPLE)
    # On macOS, force load the archive to keep constructor sections
    target_link_options(grelay PRIVATE -Wl,-force_load,$<TARGET_FILE:nostrdb_storage>)
  else()
    # On ELF platforms, whole-archive just for this lib
    target_link_options(grelay PRIVATE -Wl,--whole-archive $<TARGET_FILE:nostrdb_storage> -Wl,--no-whole-archive)
  endif()
  # Also link vendored static archives directly so ndb_* resolve even if not pulled transitively
  set(_NDB_VENDOR_DIR ${CMAKE_SOURCE_DIR}/third_party/nostrdb)
  if (EXISTS ${_NDB_VENDOR_DIR}/libnostrdb.a)
    # Add the archive to the link line and force-retain objects
    target_link_libraries(grelay PRIVATE ${_NDB_VENDOR_DIR}/libnostrdb.a)
    # Ensure the linker does not drop required objects from the static archive
    if (APPLE)
      target_link_options(grelay PRIVATE -Wl,-force_load,${_NDB_VENDOR_DIR}/libnostrdb.a)
    else()
      target_link_options(grelay PRIVATE -Wl,--whole-archive ${_NDB_VENDOR_DIR}/libnostrdb.a -Wl,--no-whole-archive)
    endif()
  endif()
  if (EXISTS ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
    target_link_libraries(grelay PRIVATE ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
    if (APPLE)
      target_link_libraries(grelay PRIVATE ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
    else()
      target_link_options(grelay PRIVATE -Wl,--whole-archive ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a -Wl,--no-whole-archive)
    endif()
  endif()
endif()

# nsync dependency required via libgo headers used transitively
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)
target_include_directories(grelay PRIVATE ${NSYNC_INCLUDE_DIR})
find_library(NSYNC_LIB nsync REQUIRED)
target_link_libraries(grelay PRIVATE ${NSYNC_LIB})

# On macOS, link Security framework properly via link options
if(APPLE)
  target_link_options(grelay PRIVATE "SHELL:-framework Security")
endif()

# Apply sanitizer link flags so TSAN/ASAN DSOs are present when needed
if (COMMAND apply_sanitizers)
  apply_sanitizers(grelay)
endif()

install(TARGETS grelay RUNTIME DESTINATION bin)
