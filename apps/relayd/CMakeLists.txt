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

# org.nostr.SessionRelay1 read-only stats on the session bus (nostrc-janr).
# Optional: without GIO the daemon builds exactly as before and --stats
# reports it is unsupported. GIO is not an auth-surface dependency (the
# dep-purity gate covers nostr-authd / pam_nostr only).
pkg_check_modules(SESSION_RELAYD_GIO QUIET IMPORTED_TARGET gio-2.0>=2.76)
if(SESSION_RELAYD_GIO_FOUND)
  target_sources(nostr-session-relayd PRIVATE src/session/session_dbus.c)
  target_compile_definitions(nostr-session-relayd PRIVATE NSR_HAVE_GDBUS=1)
  target_link_libraries(nostr-session-relayd PRIVATE PkgConfig::SESSION_RELAYD_GIO)
  install(FILES ${CMAKE_SOURCE_DIR}/gnome/dbus/org.nostr.SessionRelay1.xml
          DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/interfaces)
else()
  target_sources(nostr-session-relayd PRIVATE src/session/session_dbus.c)
  message(STATUS "nostr-session-relayd: gio-2.0 not found; org.nostr.SessionRelay1 disabled")
endif()
# --- Upstream federation (nostrc-7d96) -------------------------------------
# Store-and-forward of what local apps write to relay.sock to the user's own
# upstream relays (NIP-65 write / NIP-17 inbox / NIP-29 group relay). Outbound
# engine: gnome/libnostr-publish (libsoup-3 transport, org.nostr.Signer for
# NIP-42 AUTH); outbox: SQLite. The top-level CMakeLists configures
# libnostr-publish after apps/relayd, so the wiring runs deferred at the end
# of the top-level directory and only when the nostr-publish target exists
# (ENABLE_LIBNOSTR_PUBLISH / ENABLE_NOSTR_DAV / ...). Without it the session
# relay builds exactly as before and reports FederationState "unavailable".
# nostr-authd / pam_nostr are untouched (scripts/check-authd-dep-purity.sh).
set(NSR_RELAYD_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")
function(nsr_wire_federation)
  set(_dir "${NSR_RELAYD_SOURCE_DIR}")
  if(NOT TARGET nostr-publish OR NOT TARGET nostr-session-relayd)
    message(STATUS "nostr-session-relayd: libnostr-publish not built; upstream federation disabled")
    return()
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(NSR_FED_GIO QUIET IMPORTED_TARGET gio-2.0>=2.76)
  pkg_check_modules(NSR_FED_SQLITE QUIET IMPORTED_TARGET sqlite3)
  if(NOT NSR_FED_GIO_FOUND OR NOT NSR_FED_SQLITE_FOUND)
    message(STATUS "nostr-session-relayd: gio-2.0/sqlite3 missing; upstream federation disabled")
    return()
  endif()
  add_library(nsr_federation STATIC
    ${_dir}/src/session/session_fed_policy.c
    ${_dir}/src/session/session_outbox.c
    ${_dir}/src/session/session_federation.c
    ${_dir}/src/session/session_tee_storage.c
    ${_dir}/src/session/session_routing.c
  )
  target_include_directories(nsr_federation PUBLIC
    ${_dir}/src/session
    ${_dir}/include
    ${CMAKE_SOURCE_DIR}/libnostr/include
  )
  target_link_libraries(nsr_federation PUBLIC
    nostr-publish nostr nostr_json PkgConfig::NSR_FED_GIO PkgConfig::NSR_FED_SQLITE)
  target_compile_options(nsr_federation PRIVATE -Wall -Wextra -Wformat=2)
  target_compile_definitions(nostr-session-relayd PRIVATE NSR_HAVE_FEDERATION=1)
  target_link_libraries(nostr-session-relayd PRIVATE nsr_federation)
  message(STATUS "nostr-session-relayd: upstream federation enabled (libnostr-publish + sqlite3)")

  if(BUILD_TESTING)
    set(_t ${_dir}/tests)
    foreach(_unit fed_policy outbox)
      add_executable(test_session_${_unit} ${_t}/test_session_${_unit}.c)
      target_link_libraries(test_session_${_unit} PRIVATE nsr_federation)
      target_compile_definitions(test_session_${_unit} PRIVATE
        NSR_CONF_EXAMPLE="${_dir}/session-relay.conf.example")
      add_test(NAME relayd_session_${_unit} COMMAND test_session_${_unit})
      set_tests_properties(relayd_session_${_unit} PROPERTIES
        TIMEOUT 60 LABELS "relay;session-relay;federation;unit")
    endforeach()
    pkg_check_modules(NSR_FED_SOUP QUIET IMPORTED_TARGET libsoup-3.0)
    if(NSR_FED_SOUP_FOUND)
      # In-process engine against fake remote relays (accept / reject /
      # demand AUTH / drop connection / gift-wrap lane) on 127.0.0.1.
      add_executable(test_session_federation ${_t}/test_session_federation.c)
      target_link_libraries(test_session_federation PRIVATE
        nsr_federation PkgConfig::NSR_FED_SOUP)
      add_test(NAME relayd_session_federation COMMAND test_session_federation)
      set_tests_properties(relayd_session_federation PROPERTIES
        TIMEOUT 120 LABELS "relay;session-relay;federation;integration")
      # The packaged daemon end to end: relay.sock -> outbox -> fake remote,
      # observed over org.nostr.SessionRelay1 on a private GTestDBus bus with
      # a fake org.nostr.Signer (GetPublicKey + NIP-42 AUTH signing).
      if(UNIX AND NOT APPLE AND TARGET nostrdb_storage)
        add_executable(test_session_relay_federation ${_t}/test_session_relay_federation.c)
        target_link_libraries(test_session_relay_federation PRIVATE
          nostr nostr_json PkgConfig::NSR_FED_GIO PkgConfig::NSR_FED_SOUP)
        target_include_directories(test_session_relay_federation PRIVATE
          ${CMAKE_SOURCE_DIR}/libnostr/include)
        add_test(NAME relayd_session_relay_federation COMMAND test_session_relay_federation)
        set_tests_properties(relayd_session_relay_federation PROPERTIES
          TIMEOUT 120 SKIP_RETURN_CODE 77
          ENVIRONMENT "NOSTR_SESSION_RELAYD=$<TARGET_FILE:nostr-session-relayd>"
          LABELS "relay;session-relay;federation;integration")
      endif()
    endif()
  endif()
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL nsr_wire_federation)

# The tree has no project VERSION; packagers may pass -DNOSTR_SESSION_RELAYD_VERSION=…
if(NOSTR_SESSION_RELAYD_VERSION)
  target_compile_definitions(nostr-session-relayd PRIVATE
    NSR_VERSION="${NOSTR_SESSION_RELAYD_VERSION}")
endif()

# nostrdb storage for both daemons (the session relay's store lives under
# ~/.local/share/nostr/session-relay/). components/nostrdb brings nostrdb
# itself -- libnostr's in-tree target, or a private static copy when libnostr
# has no nostrdb backend. The mains register the driver explicitly: its
# self-registering constructor sits in a static archive member that nothing
# else references, so the linker would otherwise drop it.
if (TARGET nostrdb_storage)
  foreach(_relayd_exe nostr-session-relayd nostrc-relayd)
    target_link_libraries(${_relayd_exe} PRIVATE nostrdb_storage)
    target_compile_definitions(${_relayd_exe} PRIVATE NOSTRC_HAVE_NOSTRDB_STORAGE=1)
  endforeach()
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
