# SPDX-License-Identifier: GPL-3.0-or-later
# CMakeLists.txt for NIP-29 Relay Groups Plugin

set(PLUGIN_NAME nip29-groups)

set(GNOSTR_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")

set(PLUGIN_SOURCES
    nip29-groups-plugin.c
    gn-nip29-group-service.c
    gn-nip29-events.c
    gn-nip29-migration.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/gnostr-plugin-api.c

    # GObject models
    model/gn-nip29-group-item.c
    model/gn-nip29-message-item.c
    model/gn-nip29-group-list-model.c
    model/gn-nip29-message-list-model.c

    # UI widgets
    ui/gn-nip29-groups-panel.c
    ui/gn-nip29-group-chat-view.c
    ui/gn-nip29-group-list-row.c
    ui/gn-nip29-message-row.c
    ui/gn-nip29-composer.c
    ui/gn-nip29-add-group-dialog.c
)

add_library(${PLUGIN_NAME} MODULE ${PLUGIN_SOURCES})

target_include_directories(${PLUGIN_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GNOSTR_APP_DIR}/src
    ${GNOSTR_APP_DIR}/src/util
    ${PROJECT_ROOT}/nips/nip29/include
    ${PROJECT_ROOT}/libnostr/include
    ${PROJECT_ROOT}/nostr-gobject/include
    ${PROJECT_ROOT}/libgo/include
    ${CMAKE_BINARY_DIR}/apps/gnostr/generated
)

target_compile_definitions(${PLUGIN_NAME} PRIVATE GNOSTR_PLUGIN_BUILD=1)

# Find libadwaita for UI widgets
pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)

target_link_libraries(${PLUGIN_NAME} PRIVATE
    PkgConfig::GTK4
    PkgConfig::GLIB
    PkgConfig::LIBPEAS
    PkgConfig::JSON_GLIB
    PkgConfig::LIBADWAITA
)

if (NOT TARGET nip29)
    message(FATAL_ERROR "nip29-groups plugin requires the nip29 target")
endif()
target_link_libraries(${PLUGIN_NAME} PRIVATE nip29)
if (TARGET nostr)
    target_link_libraries(${PLUGIN_NAME} PRIVATE nostr)
endif()
if (TARGET libgo)
    target_link_libraries(${PLUGIN_NAME} PRIVATE libgo)
endif()

set_target_properties(${PLUGIN_NAME} PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/gnostr/plugins/${PLUGIN_NAME}"
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
    LIBRARY DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr/plugins/${PLUGIN_NAME}
)

install(FILES ${PLUGIN_NAME}.plugin
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr/plugins/${PLUGIN_NAME}
)

# nostrc-4gf4: the moderation events the plugin builds (create-group has
# only `h`; metadata goes in edit-metadata; update-pin-list).
if(BUILD_TESTING)
    add_executable(gnostr-test-nip29-events
        tests/test_nip29_events.c
        gn-nip29-events.c)
    target_include_directories(gnostr-test-nip29-events PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_ROOT}/libnostr/include)
    target_link_libraries(gnostr-test-nip29-events PRIVATE PkgConfig::GLIB PkgConfig::JSON_GLIB)
    add_test(NAME gnostr-test-nip29-events COMMAND gnostr-test-nip29-events)
    set_tests_properties(gnostr-test-nip29-events PROPERTIES
        TIMEOUT 30
        LABELS "nip29;plugin;unit;gnostr")
endif()

# nostrc-7n4t: migration/fork detection from trusted authors' kind:10009.
if(BUILD_TESTING AND TARGET nostr)
    add_executable(gnostr-test-nip29-migration
        tests/test_nip29_migration.c
        gn-nip29-migration.c)
    target_include_directories(gnostr-test-nip29-migration PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_ROOT}/libnostr/include)
    target_link_libraries(gnostr-test-nip29-migration PRIVATE PkgConfig::GLIB nostr)
    add_test(NAME gnostr-test-nip29-migration COMMAND gnostr-test-nip29-migration)
    set_tests_properties(gnostr-test-nip29-migration PROPERTIES
        TIMEOUT 30
        LABELS "nip29;plugin;unit;gnostr")
endif()
