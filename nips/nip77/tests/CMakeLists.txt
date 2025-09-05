# NIP-77 tests harness (unit + integration)

# Minimal unit test placeholders to integrate with CTest
add_executable(test_neg_varint test_neg_varint.c)
target_link_libraries(test_neg_varint PRIVATE nostr_nip77)
add_test(NAME test_neg_varint COMMAND test_neg_varint)

add_executable(test_neg_bound test_neg_bound.c)
target_link_libraries(test_neg_bound PRIVATE nostr_nip77)
add_test(NAME test_neg_bound COMMAND test_neg_bound)

add_executable(test_neg_fingerprint test_neg_fingerprint.c)
target_link_libraries(test_neg_fingerprint PRIVATE nostr_nip77)
add_test(NAME test_neg_fingerprint COMMAND test_neg_fingerprint)

add_executable(test_neg_message test_neg_message.c)
target_link_libraries(test_neg_message PRIVATE nostr_nip77)
add_test(NAME test_neg_message COMMAND test_neg_message)

add_executable(test_neg_session test_neg_session.c)
target_link_libraries(test_neg_session PRIVATE nostr_nip77)
add_test(NAME test_neg_session COMMAND test_neg_session)

# Synthetic negotiation scenarios (IdList, Split)
add_executable(test_neg_session_synth test_neg_session_synth.c)
target_link_libraries(test_neg_session_synth PRIVATE nostr_nip77)
add_test(NAME test_neg_session_synth COMMAND test_neg_session_synth)

add_executable(test_neg_session_caps test_neg_session_caps.c)
target_link_libraries(test_neg_session_caps PRIVATE nostr_nip77)
add_test(NAME test_neg_session_caps COMMAND test_neg_session_caps)

if(TARGET nostr_nip77_ndb)
  add_executable(test_ndb_integration test_ndb_integration.c)
  target_link_libraries(test_ndb_integration PRIVATE nostr_nip77 nostr_nip77_ndb)
  add_test(NAME test_ndb_integration COMMAND test_ndb_integration)

  add_executable(test_ndb_ordering test_ndb_ordering.c)
  target_link_libraries(test_ndb_ordering PRIVATE nostr_nip77 nostr_nip77_ndb nostrdb)
  add_test(NAME test_ndb_ordering COMMAND test_ndb_ordering)

  add_executable(test_ndb_tie_break test_ndb_tie_break.c)
  target_link_libraries(test_ndb_tie_break PRIVATE nostr_nip77 nostr_nip77_ndb nostrdb)
  add_test(NAME test_ndb_tie_break COMMAND test_ndb_tie_break)
endif()
