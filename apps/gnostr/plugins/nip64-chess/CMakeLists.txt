# SPDX-License-Identifier: GPL-3.0-or-later
# CMakeLists.txt for NIP-64 Chess Plugin

set(PLUGIN_NAME nip64-chess)

# Plugin sources
set(PLUGIN_SOURCES
    nip64-chess-plugin.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/gnostr-plugin-api.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/util/chess_engine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/util/nip64_chess.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/ui/gnostr-chess-board.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/ui/gnostr-chess-session.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/ui/gnostr-chess-game-view.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/ui/gnostr-chess-new-game-dialog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/ui/gnostr-chess-publish-dialog.c
)

# Create shared library
add_library(${PLUGIN_NAME} MODULE ${PLUGIN_SOURCES})

# Include directories
target_include_directories(${PLUGIN_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/gnostr/src
    ${CMAKE_SOURCE_DIR}/apps/gnostr/src/ui
    ${CMAKE_SOURCE_DIR}/apps/gnostr/src/util
    ${CMAKE_SOURCE_DIR}/apps/gnostr/src/ipc
    ${CMAKE_SOURCE_DIR}/gobject/include
    ${CMAKE_SOURCE_DIR}/libnostr/include
    ${CMAKE_SOURCE_DIR}/libgo/include
    ${CMAKE_SOURCE_DIR}/nips/nip46/include
    ${CMAKE_BINARY_DIR}/apps/gnostr/generated
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
