# SPDX-License-Identifier: GPL-3.0-or-later
# CMakeLists.txt for NIP-47 Nostr Wallet Connect Plugin

set(PLUGIN_NAME nip47-nwc)

# Get the gnostr app directory (two levels up from plugin)
set(GNOSTR_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
# Get the project root (four levels up from plugin)
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")

# Plugin sources
set(PLUGIN_SOURCES
    nip47-nwc-plugin.c
    ${GNOSTR_APP_DIR}/src/gnostr-plugin-api.c
)

# Create shared library
add_library(${PLUGIN_NAME} MODULE ${PLUGIN_SOURCES})

# Include directories
target_include_directories(${PLUGIN_NAME} PRIVATE
    ${GNOSTR_APP_DIR}/src
    ${GNOSTR_APP_DIR}/src/util
    ${PROJECT_ROOT}/nostr-gobject/include
    ${PROJECT_ROOT}/libnostr/include
    ${PROJECT_ROOT}/libnostr/generated
    ${PROJECT_ROOT}/nips/nip47/include
    ${PROJECT_ROOT}/nips/nip04/include
    ${CMAKE_BINARY_DIR}/apps/gnostr/generated
)
# nsync headers (cache var set by libnostr's find_path)
if(NOT NSYNC_INCLUDE_DIR)
  find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)
endif()
target_include_directories(${PLUGIN_NAME} PRIVATE ${NSYNC_INCLUDE_DIR})

# Plugin build mode - excludes host-only code from gnostr-plugin-api.c
target_compile_definitions(${PLUGIN_NAME} PRIVATE GNOSTR_PLUGIN_BUILD=1)

# Link libraries
target_link_libraries(${PLUGIN_NAME} PRIVATE
    PkgConfig::GTK4
    PkgConfig::GLIB
    PkgConfig::LIBPEAS
)

# Link nostr libraries when available as targets
if(TARGET nostr)
    target_link_libraries(${PLUGIN_NAME} PRIVATE nostr)
endif()
if(TARGET nostr_nip47_core)
    target_link_libraries(${PLUGIN_NAME} PRIVATE nostr_nip47_core)
endif()
if(TARGET nip04)
    target_link_libraries(${PLUGIN_NAME} PRIVATE nip04)
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
