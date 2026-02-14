# SPDX-License-Identifier: GPL-3.0-or-later
# CMakeLists.txt for NIP-99 Marketplace Plugin
#
# Provides classified listing browsing and publishing

set(PLUGIN_NAME nip99-marketplace)

# Get the gnostr app directory (two levels up from plugin)
set(GNOSTR_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
# Get the project root
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")

# Plugin sources - include plugin API directly
set(PLUGIN_SOURCES
    nip99-marketplace-plugin.c
    ${GNOSTR_APP_DIR}/src/gnostr-plugin-api.c
)

# Create shared library
add_library(${PLUGIN_NAME} MODULE ${PLUGIN_SOURCES})

# Include directories
target_include_directories(${PLUGIN_NAME} PRIVATE
    ${GNOSTR_APP_DIR}/src
    ${GNOSTR_APP_DIR}/src/util
    ${GNOSTR_APP_DIR}/src/ui
    ${PROJECT_ROOT}/nips/nip46/include
    ${PROJECT_ROOT}/nostr-gobject/include
    ${PROJECT_ROOT}/libnostr/include
    ${PROJECT_ROOT}/libgo/include
    ${PROJECT_ROOT}/libjson/include
    ${CMAKE_BINARY_DIR}/apps/gnostr/generated
    ${_NOSTR_CONFIG_DIR}
)

# Plugin build mode - excludes host-only code from gnostr-plugin-api.c
target_compile_definitions(${PLUGIN_NAME} PRIVATE GNOSTR_PLUGIN_BUILD=1)

# Link libraries
target_link_libraries(${PLUGIN_NAME} PRIVATE
    PkgConfig::GTK4
    PkgConfig::GLIB
    PkgConfig::ADWAITA
    PkgConfig::LIBPEAS
    PkgConfig::JSON_GLIB
)

# Link libgo for include paths (nsync, etc)
if (TARGET libgo)
    target_link_libraries(${PLUGIN_NAME} PRIVATE libgo)
endif()

# Allow undefined symbols - they're resolved when the plugin is loaded by the host app
if(APPLE)
    target_link_options(${PLUGIN_NAME} PRIVATE -undefined dynamic_lookup)
endif()

# Set library properties
set_target_properties(${PLUGIN_NAME} PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/gnostr/plugins/${PLUGIN_NAME}"
)

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
