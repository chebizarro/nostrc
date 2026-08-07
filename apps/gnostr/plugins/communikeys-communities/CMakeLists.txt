set(PLUGIN_NAME communikeys-communities)
set(GNOSTR_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
add_library(${PLUGIN_NAME} MODULE
  communikeys-communities-plugin.c
  gn-communikeys-community-service.c
  model/gn-communikeys-community-item.c
  ui/gn-communikeys-communities-panel.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/gnostr-plugin-api.c
)
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
install(TARGETS ${PLUGIN_NAME}
  LIBRARY DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr/plugins/${PLUGIN_NAME})
install(FILES ${PLUGIN_NAME}.plugin
  DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr/plugins/${PLUGIN_NAME})
