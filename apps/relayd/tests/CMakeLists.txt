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

if(COMMAND apply_sanitizers)
  apply_sanitizers(relayd_ingress_security)
  apply_sanitizers(relayd_rate_limit_security)
endif()
