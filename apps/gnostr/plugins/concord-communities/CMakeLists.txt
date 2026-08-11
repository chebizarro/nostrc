set(PLUGIN_NAME concord-communities)
set(GNOSTR_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")

set(CONCORD_MODEL_SOURCES
  model/gn-concord-community-item.c
  model/gn-concord-channel-item.c
  model/gn-concord-message-item.c
)
set(CONCORD_UI_SOURCES
  ui/gn-concord-communities-panel.c
  ui/gn-concord-community-row.c
  ui/gn-concord-community-view.c
  ui/gn-concord-composer.c
)

add_library(${PLUGIN_NAME} MODULE
  concord-communities-plugin.c
  gn-concord-community-service.c
  ${CONCORD_MODEL_SOURCES}
  ${CONCORD_UI_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/gnostr-plugin-api.c
)
target_compile_features(${PLUGIN_NAME} PRIVATE c_std_11)
target_include_directories(${PLUGIN_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${GNOSTR_APP_DIR}/src
  ${GNOSTR_APP_DIR}/src/util
  ${PROJECT_ROOT}/nips/nip-concord/include
  ${PROJECT_ROOT}/nips/nip19/include
  ${PROJECT_ROOT}/libnostr/include
  ${PROJECT_ROOT}/libgo/include
  ${CMAKE_BINARY_DIR}/apps/gnostr/generated
)
target_compile_definitions(${PLUGIN_NAME} PRIVATE GNOSTR_PLUGIN_BUILD=1)
pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  PkgConfig::GTK4 PkgConfig::GLIB PkgConfig::LIBPEAS
  PkgConfig::JSON_GLIB PkgConfig::LIBADWAITA nip_concord nip19
)
if(TARGET nostr)
  target_link_libraries(${PLUGIN_NAME} PRIVATE nostr)
endif()
if(TARGET libgo)
  target_link_libraries(${PLUGIN_NAME} PRIVATE libgo)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(${PLUGIN_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_target_properties(${PLUGIN_NAME} PROPERTIES
  PREFIX "lib"
  LIBRARY_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/apps/gnostr/plugins/${PLUGIN_NAME}"
)
if(APPLE)
  target_link_options(${PLUGIN_NAME} PRIVATE -undefined dynamic_lookup)
endif()
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.plugin
  ${CMAKE_BINARY_DIR}/apps/gnostr/plugins/${PLUGIN_NAME}/${PLUGIN_NAME}.plugin
  COPYONLY
)

if(BUILD_TESTING)
  set(CONCORD_TEST_COMMON
    gn-concord-community-service.c
    ${CONCORD_MODEL_SOURCES}
    tests/gn-concord-plugin-api-stubs.c
  )

  add_executable(concord-communities-service-test
    tests/test-concord-service.c
    ${CONCORD_TEST_COMMON}
  )
  add_executable(concord-communities-ui-test
    tests/test-concord-ui.c
    ${CONCORD_TEST_COMMON}
    ${CONCORD_UI_SOURCES}
  )

  foreach(test_target
      concord-communities-service-test
      concord-communities-ui-test)
    target_compile_features(${test_target} PRIVATE c_std_11)
    target_compile_definitions(${test_target} PRIVATE GNOSTR_PLUGIN_BUILD=1)
    target_include_directories(${test_target} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${GNOSTR_APP_DIR}/src
      ${GNOSTR_APP_DIR}/src/util
      ${PROJECT_ROOT}/nips/nip-concord/include
      ${PROJECT_ROOT}/nips/nip19/include
      ${PROJECT_ROOT}/nips/nip44/include
      ${PROJECT_ROOT}/libnostr/include
      ${PROJECT_ROOT}/libgo/include
      ${CMAKE_BINARY_DIR}/apps/gnostr/generated
    )
    # The offline backend stands in for the host's signer, so it does the same
    # NIP-44 self-ECDH in-process that the real signer does out of it.
    target_link_libraries(${test_target} PRIVATE
      PkgConfig::GTK4 PkgConfig::GLIB PkgConfig::JSON_GLIB
      PkgConfig::LIBADWAITA nip_concord nip19 nostr_nip44_core
    )
    if(TARGET nostr)
      target_link_libraries(${test_target} PRIVATE nostr)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${test_target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endforeach()

  add_custom_target(concord-communities-tests
    DEPENDS
      concord-communities-service-test
      concord-communities-ui-test
  )
  add_test(NAME concord-communities-service
    COMMAND concord-communities-service-test)
  add_test(NAME concord-communities-ui
    COMMAND concord-communities-ui-test)
  set_tests_properties(
    concord-communities-service concord-communities-ui
    PROPERTIES TIMEOUT 30 LABELS "concord;plugin;unit;gnostr")
endif()

install(TARGETS ${PLUGIN_NAME}
  LIBRARY DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr/plugins/${PLUGIN_NAME})
install(FILES ${PLUGIN_NAME}.plugin
  DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr/plugins/${PLUGIN_NAME})
