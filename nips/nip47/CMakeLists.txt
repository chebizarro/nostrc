project(nip47)

set(NIP47_INC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(NIP47_CORE_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nwc_uri.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nwc_info.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nwc_envelope.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nwc_client.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nwc_wallet.c
)

add_library(nostr_nip47_core ${NIP47_CORE_SRC})
target_include_directories(nostr_nip47_core PUBLIC ${NIP47_INC})
# Link to core nostr primitives and encryption helpers
if (TARGET nip44)
  target_link_libraries(nostr_nip47_core PUBLIC nostr nip44 nip04)
else()
  target_link_libraries(nostr_nip47_core PUBLIC nostr nip04)
endif()

# Optional GLib layer if files exist
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/nwc_client_g.c)
  add_library(nostr_nip47_glib ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/nwc_client_g.c ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/nwc_wallet_g.c)
  target_include_directories(nostr_nip47_glib PUBLIC ${NIP47_INC})
  if (TARGET nostr_glib)
    target_link_libraries(nostr_nip47_glib PUBLIC nostr_glib nostr_nip47_core)
  else()
    target_link_libraries(nostr_nip47_glib PUBLIC nostr_nip47_core)
  endif()
endif()

# Tests: spec linkage/layout guard
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spec_source.c)
  add_executable(test_nip47_spec_source tests/test_spec_source.c)
  target_link_libraries(test_nip47_spec_source PRIVATE nostr_nip47_core)
  add_test(NAME test_nip47_spec_source COMMAND test_nip47_spec_source)
endif()

# URI tests
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nwc_uri.c)
  add_executable(test_nip47_nwc_uri tests/test_nwc_uri.c)
  target_link_libraries(test_nip47_nwc_uri PRIVATE nostr_nip47_core)
  add_test(NAME test_nip47_nwc_uri COMMAND test_nip47_nwc_uri)
endif()

# Info tests
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nwc_info.c)
  add_executable(test_nip47_nwc_info tests/test_nwc_info.c)
  target_link_libraries(test_nip47_nwc_info PRIVATE nostr_nip47_core)
  add_test(NAME test_nip47_nwc_info COMMAND test_nip47_nwc_info)
endif()

# Envelope tests
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nwc_envelope.c)
  add_executable(test_nip47_nwc_envelope tests/test_nwc_envelope.c)
  target_link_libraries(test_nip47_nwc_envelope PRIVATE nostr_nip47_core)
  add_test(NAME test_nip47_nwc_envelope COMMAND test_nip47_nwc_envelope)
endif()

# Session tests
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nwc_session.c)
  add_executable(test_nip47_nwc_session tests/test_nwc_session.c)
  target_link_libraries(test_nip47_nwc_session PRIVATE nostr_nip47_core)
  add_test(NAME test_nip47_nwc_session COMMAND test_nip47_nwc_session)
endif()

# Examples
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/nwc_client_example.c)
  add_executable(nwc_client_example examples/nwc_client_example.c)
  target_link_libraries(nwc_client_example PRIVATE nostr_nip47_core)
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/nwc_wallet_example.c)
  add_executable(nwc_wallet_example examples/nwc_wallet_example.c)
  target_link_libraries(nwc_wallet_example PRIVATE nostr_nip47_core)
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/nwc_error_example.c)
  add_executable(nwc_error_example examples/nwc_error_example.c)
  target_link_libraries(nwc_error_example PRIVATE nostr_nip47_core)
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/glib_client_example.c)
  add_executable(glib_client_example examples/glib_client_example.c)
  target_link_libraries(glib_client_example PRIVATE nostr_nip47_glib)
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/glib_wallet_example.c)
  add_executable(glib_wallet_example examples/glib_wallet_example.c)
  target_link_libraries(glib_wallet_example PRIVATE nostr_nip47_glib)
endif()

# Install headers
install(DIRECTORY ${NIP47_INC}/ DESTINATION include/nostr/nip47)
