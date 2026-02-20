# SPDX-License-Identifier: GPL-3.0-or-later
# CMakeLists.txt for MLS Group Messaging Plugin

set(PLUGIN_NAME mls-groups)

# Get the gnostr app directory (two levels up from plugin)
set(GNOSTR_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
# Get the project root
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")

# Plugin sources
set(PLUGIN_SOURCES
    mls-groups-plugin.c
    gn-marmot-service.c
    gn-key-package-manager.c
    gn-mls-event-router.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/gnostr-plugin-api.c
)

# Create shared library
add_library(${PLUGIN_NAME} MODULE ${PLUGIN_SOURCES})

# Include directories
target_include_directories(${PLUGIN_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GNOSTR_APP_DIR}/src
    ${GNOSTR_APP_DIR}/src/util
    ${PROJECT_ROOT}/libmarmot/include
    ${PROJECT_ROOT}/marmot-gobject/include
    ${PROJECT_ROOT}/nips/nip44/include
    ${PROJECT_ROOT}/nips/nip59/include
    ${PROJECT_ROOT}/nostr-gobject/include
    ${PROJECT_ROOT}/libnostr/include
    ${PROJECT_ROOT}/libgo/include
    ${CMAKE_BINARY_DIR}/apps/gnostr/generated
    ${CMAKE_BINARY_DIR}/marmot-gobject/include
)

# Plugin build mode - excludes host-only code from gnostr-plugin-api.c
target_compile_definitions(${PLUGIN_NAME} PRIVATE GNOSTR_PLUGIN_BUILD=1)

# Link libraries
target_link_libraries(${PLUGIN_NAME} PRIVATE
    PkgConfig::GTK4
    PkgConfig::GLIB
    PkgConfig::LIBPEAS
    PkgConfig::JSON_GLIB
)

# Link libgo for include paths
if (TARGET libgo)
    target_link_libraries(${PLUGIN_NAME} PRIVATE libgo)
endif()

# Link marmot-gobject for MLS operations
if (TARGET marmot-gobject)
    target_link_libraries(${PLUGIN_NAME} PRIVATE marmot-gobject)
endif()

# Link libmarmot
if (TARGET marmot)
    target_link_libraries(${PLUGIN_NAME} PRIVATE marmot)
endif()

# Link NIP-44 library for encryption
if (TARGET nip44)
    target_link_libraries(${PLUGIN_NAME} PRIVATE nip44)
endif()

# Link NIP-59 library for gift wrapping
if (TARGET nip59)
    target_link_libraries(${PLUGIN_NAME} PRIVATE nip59)
endif()

# Set library properties
set_target_properties(${PLUGIN_NAME} PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/gnostr/plugins/${PLUGIN_NAME}"
)

# Allow undefined symbols - they're resolved when the plugin is loaded by the host app
if(APPLE)
    target_link_options(${PLUGIN_NAME} PRIVATE -undefined dynamic_lookup)
endif()

# Copy plugin file to output directory
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.plugin
    ${CMAKE_BINARY_DIR}/apps/gnostr/plugins/${PLUGIN_NAME}/${PLUGIN_NAME}.plugin
    COPYONLY
)

# Install plugin
install(TARGETS ${PLUGIN_NAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr/plugins/${PLUGIN_NAME}
)

install(FILES ${PLUGIN_NAME}.plugin
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr/plugins/${PLUGIN_NAME}
)
