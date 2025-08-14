
project(nip46)

set(NIP46_INC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(NIP46_CORE_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip46_session.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip46_uri.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip46_envelope.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip46_msg.c
)

add_library(nostr_nip46_core ${NIP46_CORE_SRC})
target_include_directories(nostr_nip46_core PUBLIC ${NIP46_INC})
# Ensure nip44/nip04 headers are visible and libraries linked
target_include_directories(nostr_nip46_core PUBLIC
  ${CMAKE_SOURCE_DIR}/nips/nip04/include
  ${CMAKE_SOURCE_DIR}/nips/nip44/include)
target_link_libraries(nostr_nip46_core PUBLIC nostr nip04 nostr_nip44_core nostr_json)

# Optional GLib layer if nostr_glib is available
set(NIP46_GLIB_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/nip46_client_g.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/nip46_bunker_g.c
)
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/nip46_client_g.c)
  add_library(nostr_nip46_glib ${NIP46_GLIB_SRC})
  target_include_directories(nostr_nip46_glib PUBLIC ${NIP46_INC})
  if (TARGET nostr_glib)
    target_link_libraries(nostr_nip46_glib PUBLIC nostr_glib nostr_nip46_core)
  else()
    target_link_libraries(nostr_nip46_glib PUBLIC nostr_nip46_core)
  endif()
endif()

# Tests
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_uri.c)
  add_executable(test_nip46_uri tests/test_uri.c)
  target_link_libraries(test_nip46_uri PRIVATE nostr_nip46_core)
  add_test(NAME test_nip46_uri COMMAND test_nip46_uri)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_envelope_build.c)
  add_executable(test_nip46_envelope_build tests/test_envelope_build.c)
  target_link_libraries(test_nip46_envelope_build PRIVATE nostr_nip46_core)
  add_test(NAME test_nip46_envelope_build COMMAND test_nip46_envelope_build)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_msg.c)
  add_executable(test_nip46_msg tests/test_msg.c)
  target_link_libraries(test_nip46_msg PRIVATE nostr_nip46_core)
  add_test(NAME test_nip46_msg COMMAND test_nip46_msg)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bunker_uri_issue.c)
  add_executable(test_nip46_bunker_uri_issue tests/test_bunker_uri_issue.c)
  target_link_libraries(test_nip46_bunker_uri_issue PRIVATE nostr_nip46_core)
  add_test(NAME test_nip46_bunker_uri_issue COMMAND test_nip46_bunker_uri_issue)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_client_connect.c)
  add_executable(test_nip46_client_connect tests/test_client_connect.c)
  target_link_libraries(test_nip46_client_connect PRIVATE nostr_nip46_core)
  add_test(NAME test_nip46_client_connect COMMAND test_nip46_client_connect)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_client_connect_edge.c)
  add_executable(test_nip46_client_connect_edge tests/test_client_connect_edge.c)
  target_link_libraries(test_nip46_client_connect_edge PRIVATE nostr_nip46_core)
  add_test(NAME test_nip46_client_connect_edge COMMAND test_nip46_client_connect_edge)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bunker_reply.c)
  add_executable(test_nip46_bunker_reply tests/test_bunker_reply.c)
  target_link_libraries(test_nip46_bunker_reply PRIVATE nostr_nip46_core)
  add_test(NAME test_nip46_bunker_reply COMMAND test_nip46_bunker_reply)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_client_api.c)
  add_executable(test_nip46_client_api tests/test_client_api.c)
  target_link_libraries(test_nip46_client_api PRIVATE nostr_nip46_core)
  add_test(NAME test_nip46_client_api COMMAND test_nip46_client_api)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bunker_roundtrip.c)
  add_executable(test_nip46_bunker_roundtrip tests/test_bunker_roundtrip.c)
  target_link_libraries(test_nip46_bunker_roundtrip PRIVATE nostr_nip46_core)
  add_test(NAME test_nip46_bunker_roundtrip COMMAND test_nip46_bunker_roundtrip)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bunker_connect_auth.c)
  add_executable(test_nip46_bunker_connect_auth tests/test_bunker_connect_auth.c)
  target_link_libraries(test_nip46_bunker_connect_auth PRIVATE nostr_nip46_core)
  add_test(NAME test_nip46_bunker_connect_auth COMMAND test_nip46_bunker_connect_auth)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bunker_sign_event_cb.c)
  add_executable(test_nip46_bunker_sign_event_cb tests/test_bunker_sign_event_cb.c)
  target_link_libraries(test_nip46_bunker_sign_event_cb PRIVATE nostr_nip46_core)
  add_test(NAME test_nip46_bunker_sign_event_cb COMMAND test_nip46_bunker_sign_event_cb)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bunker_sign_event_real.c)
  add_executable(test_nip46_bunker_sign_event_real tests/test_bunker_sign_event_real.c)
  target_link_libraries(test_nip46_bunker_sign_event_real PRIVATE nostr_nip46_core nostr_json)
  add_test(NAME test_nip46_bunker_sign_event_real COMMAND test_nip46_bunker_sign_event_real)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bunker_sign_event_acl_deny.c)
  add_executable(test_nip46_bunker_sign_event_acl_deny tests/test_bunker_sign_event_acl_deny.c)
  target_link_libraries(test_nip46_bunker_sign_event_acl_deny PRIVATE nostr_nip46_core nostr_json)
  add_test(NAME test_nip46_bunker_sign_event_acl_deny COMMAND test_nip46_bunker_sign_event_acl_deny)
endif()

# Examples
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/nostr-nip46-client.c)
  add_executable(nostr-nip46-client examples/nostr-nip46-client.c)
  target_link_libraries(nostr-nip46-client PRIVATE nostr_nip46_core nostr_json)
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/nostr-nip46-bunker.c)
  add_executable(nostr-nip46-bunker examples/nostr-nip46-bunker.c)
  target_link_libraries(nostr-nip46-bunker PRIVATE nostr_nip46_core nostr_json)
endif()

# Install headers
install(DIRECTORY ${NIP46_INC}/ DESTINATION include/nostr/nip46)

