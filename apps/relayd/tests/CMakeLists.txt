add_executable(relayd_ingress_security test_ingress_security.c)
target_link_libraries(relayd_ingress_security PRIVATE relay_security nostr nostr_json)
target_include_directories(relayd_ingress_security PRIVATE
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/apps/relayd/include
)
add_test(NAME relayd_ingress_security COMMAND relayd_ingress_security)
set_tests_properties(relayd_ingress_security PROPERTIES
  TIMEOUT 30
  LABELS "security;relay;ingress"
)

add_executable(relayd_rate_limit_security
  test_rate_limit_security.c
  ../src/relayd_config.c
)
target_link_libraries(relayd_rate_limit_security PRIVATE relay_security nostr)
target_include_directories(relayd_rate_limit_security PRIVATE
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/apps/relayd/include
)
add_test(NAME relayd_rate_limit_security COMMAND relayd_rate_limit_security)
set_tests_properties(relayd_rate_limit_security PROPERTIES
  TIMEOUT 20
  LABELS "security;relay;rate-limit"
)

# Unit coverage for the cfg.listen host+port parser (D1 regression).
add_executable(relayd_bind_iface
  test_bind_iface.c
  ../src/relayd_config.c
)
target_link_libraries(relayd_bind_iface PRIVATE relay_security nostr)
target_include_directories(relayd_bind_iface PRIVATE
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/apps/relayd/include
)
add_test(NAME relayd_bind_iface COMMAND relayd_bind_iface)
set_tests_properties(relayd_bind_iface PROPERTIES
  TIMEOUT 10
  LABELS "relay;config;bind"
)

# End-to-end bind assertion: start the built daemon and inspect `ss -ltn`.
# ss(1) ships with iproute2, so this test is Linux-only. On other platforms
# CTest simply does not register it.
if(UNIX AND NOT APPLE AND NOT CYGWIN)
  find_program(SS_EXECUTABLE ss)
  find_program(BASH_EXECUTABLE bash)
  if(SS_EXECUTABLE AND BASH_EXECUTABLE)
    add_test(
      NAME relayd_bind_iface_integ
      COMMAND ${BASH_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/run_bind_iface_integ.sh
              $<TARGET_FILE:nostrc-relayd>
    )
    set_tests_properties(relayd_bind_iface_integ PROPERTIES
      TIMEOUT 30
      SKIP_RETURN_CODE 77
      LABELS "relay;bind;integration"
    )
  endif()
endif()

# Session-relay peercred acceptance: spawns the built binary, asserts
# fallback-bound socket is 0600, and round-trips a same-UID connect.
# Linux-only (SO_PEERCRED); CTest reports SKIP on other platforms.
if(UNIX AND NOT APPLE AND NOT CYGWIN)
  add_executable(test_session_relay_peercred test_session_relay_peercred.c)
  target_include_directories(test_session_relay_peercred PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/relayd/include)
  add_test(
    NAME relayd_session_relay_peercred
    COMMAND test_session_relay_peercred
  )
  set_tests_properties(relayd_session_relay_peercred PROPERTIES
    TIMEOUT 30
    SKIP_RETURN_CODE 77
    ENVIRONMENT
      "NOSTR_SESSION_RELAYD=$<TARGET_FILE:nostr-session-relayd>"
    LABELS "relay;session-relay;peercred;integration"
  )
endif()

# org.nostr.SessionRelay1: spawn the real daemon on a private GTestDBus
# session bus (headless; fallback-binds relay.sock in a temp runtime dir),
# check GetStats / properties / introspection against the shipped XML and
# the --stats CLI (nostrc-janr).
if(UNIX AND NOT APPLE AND NOT CYGWIN AND SESSION_RELAYD_GIO_FOUND)
  add_executable(test_session_relay_dbus test_session_relay_dbus.c)
  target_link_libraries(test_session_relay_dbus PRIVATE PkgConfig::SESSION_RELAYD_GIO)
  target_compile_definitions(test_session_relay_dbus PRIVATE
    NSR_INTERFACE_XML="${CMAKE_SOURCE_DIR}/gnome/dbus/org.nostr.SessionRelay1.xml")
  add_test(NAME relayd_session_relay_dbus COMMAND test_session_relay_dbus)
  set_tests_properties(relayd_session_relay_dbus PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "NOSTR_SESSION_RELAYD=$<TARGET_FILE:nostr-session-relayd>"
    LABELS "relay;session-relay;dbus;integration")
endif()

if(COMMAND apply_sanitizers)
  apply_sanitizers(relayd_ingress_security)
  apply_sanitizers(relayd_rate_limit_security)
  apply_sanitizers(relayd_bind_iface)
endif()
