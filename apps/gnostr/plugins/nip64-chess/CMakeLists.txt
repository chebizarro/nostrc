# SPDX-License-Identifier: GPL-3.0-or-later
# CMakeLists.txt for NIP-64 Chess Plugin

set(PLUGIN_NAME nip64-chess)

# Get the gnostr app directory (two levels up from plugin)
set(GNOSTR_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
# Get the project root (four levels up from plugin: plugins/nip64-chess -> apps/gnostr -> apps -> rig)
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")

# Plugin sources
set(PLUGIN_SOURCES
    nip64-chess-plugin.c
    ${GNOSTR_APP_DIR}/src/gnostr-plugin-api.c
    ${GNOSTR_APP_DIR}/src/util/chess_engine.c
    ${GNOSTR_APP_DIR}/src/util/nip64_chess.c
    ${GNOSTR_APP_DIR}/src/ui/gnostr-chess-board.c
    ${GNOSTR_APP_DIR}/src/ui/gnostr-chess-session.c
    ${GNOSTR_APP_DIR}/src/ui/gnostr-chess-game-view.c
    ${GNOSTR_APP_DIR}/src/ui/gnostr-chess-new-game-dialog.c
    ${GNOSTR_APP_DIR}/src/ui/gnostr-chess-publish-dialog.c
)

# Create shared library
add_library(${PLUGIN_NAME} MODULE ${PLUGIN_SOURCES})

# Include directories
target_include_directories(${PLUGIN_NAME} PRIVATE
    ${GNOSTR_APP_DIR}/src
    ${GNOSTR_APP_DIR}/src/ui
    ${GNOSTR_APP_DIR}/src/util
    ${GNOSTR_APP_DIR}/src/ipc
    ${PROJECT_ROOT}/nostr-gobject/include
    ${PROJECT_ROOT}/libnostr/include
    ${PROJECT_ROOT}/libnostr/generated
    ${PROJECT_ROOT}/libgo/include
    ${PROJECT_ROOT}/nips/nip46/include
    ${CMAKE_BINARY_DIR}/apps/gnostr/generated
    /opt/homebrew/opt/nsync/include
    /opt/homebrew/include
)

# Plugin build mode - excludes host-only code from gnostr-plugin-api.c
target_compile_definitions(${PLUGIN_NAME} PRIVATE GNOSTR_PLUGIN_BUILD=1)

# Find libadwaita
pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)

# Link libraries
target_link_libraries(${PLUGIN_NAME} PRIVATE
    PkgConfig::GTK4
    PkgConfig::GLIB
    PkgConfig::LIBPEAS
    PkgConfig::JSON_GLIB
    PkgConfig::LIBADWAITA
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
