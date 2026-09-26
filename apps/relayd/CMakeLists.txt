project(nostrc_relayd C)

include_directories(
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/apps/relayd/include
  ${CMAKE_SOURCE_DIR}/nips/nip11/include
  ${CMAKE_SOURCE_DIR}/nips/nip86/include
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED libwebsockets)

find_package(OpenSSL REQUIRED)
find_library(SECP256K1_LIB NAMES secp256k1 REQUIRED)
# nsync resolved centrally by cmake/FindOrVendorNsync.cmake (system or
# vendored copy from third_party/nsync).  Guard so this file works if
# apps/relayd is ever configured as a standalone project (nostrc-dd5y).
if(NOT DEFINED _NOSTR_FIND_OR_VENDOR_NSYNC_DONE)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
  include(FindOrVendorNsync)
endif()
include_directories(${NSYNC_INCLUDE_DIR})

add_library(relay_security STATIC
  src/relay_policy.c
  src/rate_limit.c
  src/relay_ingress.c
)
target_include_directories(relay_security PUBLIC
  ${CMAKE_SOURCE_DIR}/apps/relayd/include
  ${CMAKE_SOURCE_DIR}/libnostr/include
)
target_link_libraries(relay_security
  PUBLIC nostr
  PRIVATE OpenSSL::Crypto
)

# --- libnostr-relay-server -----------------------------------------------
# Reusable relay-server core factored out of nostrc-relayd (#12). Static on
# purpose: this library is in-tree only (no external ABI to maintain) and the
# two consumers (`nostrc-relayd` today; the per-user session relay in Wave 3)
# both live in this repo. Public entry point:
#     apps/relayd/include/nostr-relay-server.h -> nostr_relay_server_run()
add_library(nostr-relay-server STATIC
  src/relay_server.c
  src/relayd_config.c
  src/policy_decider.c
  src/protocol_nip01.c
  src/protocol_nip11.c
  src/protocol_nip42.c
  src/protocol_nip45.c
  src/protocol_nip50.c
  src/protocol_nip77.c
  src/retention.c
  src/metrics.c
)
target_include_directories(nostr-relay-server PUBLIC
  ${CMAKE_SOURCE_DIR}/apps/relayd/include
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/nips/nip11/include
  ${CMAKE_SOURCE_DIR}/nips/nip86/include
)
target_include_directories(nostr-relay-server PRIVATE ${LWS_INCLUDE_DIRS})
target_link_libraries(nostr-relay-server
  PUBLIC
    relay_security
    nostr
  PRIVATE
    nostr_json
    ${NSYNC_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SECP256K1_LIB}
)
target_link_directories(nostr-relay-server PUBLIC ${LWS_LIBRARY_DIRS})
target_link_libraries(nostr-relay-server PUBLIC ${LWS_LIBRARIES})

# Back-compat alias for existing test targets and any downstream in-tree
# consumers that used to link `relayd_core` for policy helpers + metrics.
# Both symbols now live inside nostr-relay-server.
add_library(relayd_core ALIAS nostr-relay-server)

add_executable(nostrc-relayd
  src/relayd_main.c
)

target_include_directories(nostrc-relayd PRIVATE ${LWS_INCLUDE_DIRS})
target_link_directories(nostrc-relayd PRIVATE ${LWS_LIBRARY_DIRS})

target_link_libraries(nostrc-relayd
  PRIVATE
    nostr-relay-server
    nostr
    nostr_json
    ${NSYNC_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SECP256K1_LIB}
)

# --- nostr-session-relayd (Wave 3 / #13) ----------------------------------
# Per-user session relay: adopts a Unix-domain listen fd (systemd socket
# activation preferred; fallback bind under $XDG_RUNTIME_DIR/nostr/). Shares
# libnostr-relay-server with the system daemon; the differences are the
# listener (Unix-fd vs TCP), the storage path (per-user), and the sd_notify
# handshake.
add_executable(nostr-session-relayd
  src/session/relayd_session_main.c
  src/session/session_routing.c
)
target_include_directories(nostr-session-relayd PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/session
  ${LWS_INCLUDE_DIRS}
)
target_link_directories(nostr-session-relayd PRIVATE ${LWS_LIBRARY_DIRS})
target_link_libraries(nostr-session-relayd
  PRIVATE
    nostr-relay-server
    nostr
    nostr_json
    ${NSYNC_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SECP256K1_LIB}
)

# Optional libsystemd dependency: sd_listen_fds + sd_notify + sd_is_socket_unix.
# The session daemon falls back to a stub implementation when systemd headers
# are absent (macOS dev builds), but the Linux install must link libsystemd
# so socket activation works.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SESSION_RELAYD_SYSTEMD QUIET IMPORTED_TARGET libsystemd)
  if(SESSION_RELAYD_SYSTEMD_FOUND)
    target_link_libraries(nostr-session-relayd PRIVATE PkgConfig::SESSION_RELAYD_SYSTEMD)
  endif()
endif()

# Link the same nostrdb storage the system daemon uses so the per-user
# ~/.local/share/nostr/session-relay/ path can back a real store.
if (TARGET nostrdb_storage)
  target_link_libraries(nostr-session-relayd PRIVATE nostrdb_storage)
  if (NOT TARGET nostrdb)
    set(_NDB_VENDOR_DIR ${CMAKE_SOURCE_DIR}/third_party/nostrdb)
    if (EXISTS ${_NDB_VENDOR_DIR}/libnostrdb.a)
      if (APPLE)
        target_link_options(nostr-session-relayd PRIVATE -Wl,-force_load,${_NDB_VENDOR_DIR}/libnostrdb.a)
        target_link_libraries(nostr-session-relayd PRIVATE "-framework Security")
      else()
        target_link_options(nostr-session-relayd PRIVATE -Wl,--whole-archive ${_NDB_VENDOR_DIR}/libnostrdb.a -Wl,--no-whole-archive)
      endif()
    endif()
    if (EXISTS ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
      if (APPLE)
        target_link_libraries(nostr-session-relayd PRIVATE ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
      else()
        target_link_options(nostr-session-relayd PRIVATE -Wl,--whole-archive ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a -Wl,--no-whole-archive)
      endif()
    endif()
  endif()
endif()

# Link nostrdb storage component if available
if (TARGET nostrdb_storage)
  target_link_libraries(nostrc-relayd PRIVATE nostrdb_storage)
  # If the in-tree CMake target 'nostrdb' exists, rely on it for NDB symbols.
  # Only fall back to vendored static archives when 'nostrdb' target is absent.
  if (NOT TARGET nostrdb)
    # Also link vendored static archives directly so ndb_* symbols resolve
    set(_NDB_VENDOR_DIR ${CMAKE_SOURCE_DIR}/third_party/nostrdb)
    if (EXISTS ${_NDB_VENDOR_DIR}/libnostrdb.a)
      if (APPLE)
        target_link_options(nostrc-relayd PRIVATE -Wl,-force_load,${_NDB_VENDOR_DIR}/libnostrdb.a)
        # nostrdb uses Security framework for SecRandomCopyBytes on macOS
        target_link_libraries(nostrc-relayd PRIVATE "-framework Security")
      else()
        target_link_options(nostrc-relayd PRIVATE -Wl,--whole-archive ${_NDB_VENDOR_DIR}/libnostrdb.a -Wl,--no-whole-archive)
      endif()
    endif()
    if (EXISTS ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
      if (APPLE)
        target_link_libraries(nostrc-relayd PRIVATE ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
      else()
        target_link_options(nostrc-relayd PRIVATE -Wl,--whole-archive ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a -Wl,--no-whole-archive)
      endif()
    endif()
  endif()
endif()

# Link NIP-11 module if available and set compile define. The NIP-11 handler
# lives inside libnostr-relay-server, so the module is linked to the lib and
# the HAVE_NIP11 define propagates to any consumer through PUBLIC.
if (TARGET nip11)
  target_link_libraries(nostr-relay-server PUBLIC nip11)
  target_compile_definitions(nostr-relay-server PUBLIC HAVE_NIP11=1)
endif()

# NIP-86 integration intentionally disabled for relayd link; relayd has
# compile-time fallbacks and guards to avoid referencing nip86 symbols.

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

include(GNUInstallDirs)
# nostrc-relayd is a network daemon: install under sbin per FHS 3.0.
install(TARGETS nostrc-relayd
  RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
# nostr-session-relayd is a per-user session daemon (not a network daemon):
# ship under libexec so it does not appear on user PATH by mistake — systemd
# --user activates it by absolute path.
install(TARGETS nostr-session-relayd
  RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/relay.toml.example)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/relay.toml.example
    DESTINATION ${CMAKE_INSTALL_DATADIR}/nostrc)
endif()

# --- systemd system unit ---------------------------------------------------
# Install nostr-relayd.service into the pkg-config-reported systemd system
# unit directory when systemd.pc is available, otherwise a sane fallback under
# CMAKE_INSTALL_PREFIX. Modeled on gnome/nostr-homed/systemd/nostr-authd.
set(NOSTR_RELAYD_SYSTEMD_SYSTEM_UNIT_DIR "")
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_get_variable(NOSTR_RELAYD_SYSTEMD_SYSTEM_UNIT_DIR systemd systemdsystemunitdir)
endif()
if (NOT NOSTR_RELAYD_SYSTEMD_SYSTEM_UNIT_DIR)
  set(NOSTR_RELAYD_SYSTEMD_SYSTEM_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system")
endif()

# ExecStart carries no CLI args because relayd_main.c currently ignores argv
# and reads "relay.toml" from the current working directory. WorkingDirectory
# below anchors that lookup at /etc/nostrc so admins drop relay.toml alongside
# the shipped share/nostrc/relay.toml.example.
set(NOSTR_RELAYD_EXEC "${CMAKE_INSTALL_FULL_SBINDIR}/nostrc-relayd")
configure_file(systemd/nostr-relayd.service.in
  "${CMAKE_CURRENT_BINARY_DIR}/nostr-relayd.service" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostr-relayd.service"
  DESTINATION "${NOSTR_RELAYD_SYSTEMD_SYSTEM_UNIT_DIR}")

# Apply global sanitizer flags to ensure TSAN/ASAN/UBSAN DSOs are linked
if (COMMAND apply_sanitizers)
  apply_sanitizers(nostrc-relayd)
endif()

# Ensure sanitizer runtimes are linked for the executable as well (AppleClang needs the flag on the final link)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  if (GO_ENABLE_TSAN)
    target_link_options(nostrc-relayd PRIVATE -fsanitize=thread)
    if (APPLE)
      # Also add as a link library to place it after static archives on Apple ld
      target_link_libraries(nostrc-relayd PRIVATE -fsanitize=thread)
    endif()
  else()
    if (GO_ENABLE_ASAN)
      target_link_options(nostrc-relayd PRIVATE -fsanitize=address)
      if (APPLE)
        target_link_libraries(nostrc-relayd PRIVATE -fsanitize=address)
      endif()
    endif()
    if (GO_ENABLE_UBSAN)
      target_link_options(nostrc-relayd PRIVATE -fsanitize=undefined)
      if (APPLE)
        target_link_libraries(nostrc-relayd PRIVATE -fsanitize=undefined)
      endif()
    endif()
  endif()
endif()
