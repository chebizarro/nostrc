project(grelay C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.56 gobject-2.0>=2.56 gio-2.0>=2.56)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)

# grelay shares the same ingress security implementation as relayd. Keep it
# available even in builds that disable the relayd executable.
if(NOT TARGET relay_security)
  add_library(relay_security STATIC
    ${CMAKE_SOURCE_DIR}/apps/relayd/src/relay_policy.c
    ${CMAKE_SOURCE_DIR}/apps/relayd/src/rate_limit.c
    ${CMAKE_SOURCE_DIR}/apps/relayd/src/relay_ingress.c
  )
  target_include_directories(relay_security PUBLIC
    ${CMAKE_SOURCE_DIR}/apps/relayd/include
    ${CMAKE_SOURCE_DIR}/libnostr/include
  )
  target_link_libraries(relay_security PUBLIC nostr PRIVATE OpenSSL::Crypto)
endif()

add_executable(grelay
  src/grelay_main.c
)

target_link_libraries(grelay
  PRIVATE
    PkgConfig::GLIB
    PkgConfig::SOUP
    nostr
    nostr_json
    relay_security
)

target_include_directories(grelay PRIVATE 
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/apps/relayd/include
)

# Ensure nostrdb storage is linked so its constructor can register the driver
set(_NDB_VENDOR_DIR ${CMAKE_SOURCE_DIR}/third_party/nostrdb)
if (TARGET nostrdb_storage)
  target_link_libraries(grelay PRIVATE nostrdb_storage)
  # Only fall back to vendored static archives if the in-tree 'nostrdb' target is NOT present
  if (NOT TARGET nostrdb)
    if (EXISTS ${_NDB_VENDOR_DIR}/libnostrdb.a)
      if (APPLE)
        target_link_options(grelay PRIVATE -Wl,-force_load,$<TARGET_FILE:nostrdb_storage>)
        target_link_options(grelay PRIVATE -Wl,-force_load,${_NDB_VENDOR_DIR}/libnostrdb.a)
        # Link LMDB on macOS as well
        if (EXISTS ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
          target_link_libraries(grelay PRIVATE ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
        endif()
      else()
        # Group archives to satisfy lld with potential cyclic refs
        target_link_options(grelay PRIVATE -Wl,--start-group)
        target_link_libraries(grelay PRIVATE -Wl,--whole-archive $<TARGET_FILE:nostrdb_storage> ${_NDB_VENDOR_DIR}/libnostrdb.a -Wl,--no-whole-archive)
        if (EXISTS ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
          target_link_libraries(grelay PRIVATE ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
        endif()
        target_link_options(grelay PRIVATE -Wl,--end-group)
      endif()
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

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(TARGETS grelay RUNTIME DESTINATION bin)
