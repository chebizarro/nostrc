add_executable(grelay_ingress_security test_ingress_security.c)
target_link_libraries(grelay_ingress_security PRIVATE
  relay_security
  nostr
  nostr_json
)
target_include_directories(grelay_ingress_security PRIVATE
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/apps/relayd/include
)
add_test(NAME grelay_ingress_security COMMAND grelay_ingress_security)
set_tests_properties(grelay_ingress_security PROPERTIES
  TIMEOUT 20
  LABELS "security;relay;ingress;grelay"
)

if(COMMAND apply_sanitizers)
  apply_sanitizers(grelay_ingress_security)
endif()
