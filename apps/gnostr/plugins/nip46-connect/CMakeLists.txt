# SPDX-License-Identifier: GPL-3.0-or-later
# CMakeLists.txt for NIP-46 Nostr Connect Plugin

set(PLUGIN_NAME nip46-connect)

# Plugin sources
set(PLUGIN_SOURCES
    nip46-connect-plugin.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/gnostr-plugin-api.c
)

# Create shared library
add_library(${PLUGIN_NAME} MODULE ${PLUGIN_SOURCES})

# Include directories
target_include_directories(${PLUGIN_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/gnostr/src
    ${CMAKE_SOURCE_DIR}/nips/nip46/include
)

# Plugin build mode - excludes host-only code from gnostr-plugin-api.c
target_compile_definitions(${PLUGIN_NAME} PRIVATE GNOSTR_PLUGIN_BUILD=1)

# Link libraries
target_link_libraries(${PLUGIN_NAME} PRIVATE
    PkgConfig::GTK4
    PkgConfig::GLIB
    PkgConfig::LIBPEAS
)

# Link NIP-46 library if available
if (TARGET nostr_nip46_core)
    target_link_libraries(${PLUGIN_NAME} PRIVATE nostr_nip46_core)
endif()

# Set library properties
set_target_properties(${PLUGIN_NAME} PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/${PLUGIN_NAME}"
)

# Copy plugin file to output directory
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.plugin
    ${CMAKE_BINARY_DIR}/plugins/${PLUGIN_NAME}/${PLUGIN_NAME}.plugin
    COPYONLY
)

# Install plugin
install(TARGETS ${PLUGIN_NAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr/plugins/${PLUGIN_NAME}
)

install(FILES ${PLUGIN_NAME}.plugin
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr/plugins/${PLUGIN_NAME}
)
