set(PLUGIN_NAME communikeys-communities)
set(GNOSTR_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
set(COMMUNIKEYS_MODEL_SOURCES
  model/gn-communikeys-community-item.c
  model/gn-communikeys-section-item.c
  model/gn-communikeys-message-item.c
  model/gn-communikeys-targeted-item.c
)
set(COMMUNIKEYS_UI_SOURCES
  ui/gn-communikeys-communities-panel.c
  ui/gn-communikeys-community-row.c
  ui/gn-communikeys-community-view.c
  ui/gn-communikeys-composer.c
)
add_library(${PLUGIN_NAME} MODULE
  communikeys-communities-plugin.c
  gn-communikeys-community-service.c
  ${COMMUNIKEYS_MODEL_SOURCES}
  ${COMMUNIKEYS_UI_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/gnostr-plugin-api.c
)
target_compile_features(${PLUGIN_NAME} PRIVATE c_std_11)
target_include_directories(${PLUGIN_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${GNOSTR_APP_DIR}/src
  ${GNOSTR_APP_DIR}/src/util
  ${PROJECT_ROOT}/nips/nip-communikeys/include
  ${PROJECT_ROOT}/libnostr/include
  ${PROJECT_ROOT}/libgo/include
  ${CMAKE_BINARY_DIR}/apps/gnostr/generated
)
target_compile_definitions(${PLUGIN_NAME} PRIVATE GNOSTR_PLUGIN_BUILD=1)
pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  PkgConfig::GTK4 PkgConfig::GLIB PkgConfig::LIBPEAS
  PkgConfig::JSON_GLIB PkgConfig::LIBADWAITA nip_communikeys
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
  set(COMMUNIKEYS_TEST_COMMON
    gn-communikeys-community-service.c
    ${COMMUNIKEYS_MODEL_SOURCES}
    tests/gn-communikeys-plugin-api-stubs.c
  )

  add_executable(communikeys-communities-service-test
    tests/test-communikeys-service.c
    ${COMMUNIKEYS_TEST_COMMON}
  )
  add_executable(communikeys-communities-ui-test
    tests/test-communikeys-ui.c
    ${COMMUNIKEYS_TEST_COMMON}
    ${COMMUNIKEYS_UI_SOURCES}
  )

  foreach(test_target
      communikeys-communities-service-test
      communikeys-communities-ui-test)
    target_compile_features(${test_target} PRIVATE c_std_11)
    target_compile_definitions(${test_target} PRIVATE GNOSTR_PLUGIN_BUILD=1)
    target_include_directories(${test_target} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${GNOSTR_APP_DIR}/src
      ${GNOSTR_APP_DIR}/src/util
      ${PROJECT_ROOT}/nips/nip-communikeys/include
      ${PROJECT_ROOT}/libnostr/include
      ${PROJECT_ROOT}/libgo/include
      ${CMAKE_BINARY_DIR}/apps/gnostr/generated
    )
    target_link_libraries(${test_target} PRIVATE
      PkgConfig::GTK4 PkgConfig::GLIB PkgConfig::JSON_GLIB
      PkgConfig::LIBADWAITA nip_communikeys
    )
    if(TARGET nostr)
      target_link_libraries(${test_target} PRIVATE nostr)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${test_target} PRIVATE
        -Wall -Wextra -Wpedantic)
    endif()
  endforeach()

  add_custom_target(communikeys-communities-tests
    DEPENDS
      communikeys-communities-service-test
      communikeys-communities-ui-test
  )
  add_test(NAME communikeys-communities-service
    COMMAND communikeys-communities-service-test)
  add_test(NAME communikeys-communities-ui
    COMMAND communikeys-communities-ui-test)
  set_tests_properties(
    communikeys-communities-service communikeys-communities-ui
    PROPERTIES TIMEOUT 30 LABELS "communikeys;plugin;unit;gnostr")
endif()
install(TARGETS ${PLUGIN_NAME}
  LIBRARY DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr/plugins/${PLUGIN_NAME})
install(FILES ${PLUGIN_NAME}.plugin
  DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr/plugins/${PLUGIN_NAME})
