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

if(COMMAND apply_sanitizers)
  apply_sanitizers(relayd_ingress_security)
  apply_sanitizers(relayd_rate_limit_security)
  apply_sanitizers(relayd_bind_iface)
endif()
