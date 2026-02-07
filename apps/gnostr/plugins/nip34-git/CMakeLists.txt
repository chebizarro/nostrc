# SPDX-License-Identifier: GPL-3.0-or-later
# CMakeLists.txt for NIP-34 Git Repository Plugin

set(PLUGIN_NAME nip34-git)

# Get the gnostr app directory (two levels up from plugin)
set(GNOSTR_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
# Get the project root
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")

# Find libgit2 for git client functionality
pkg_check_modules(LIBGIT2 libgit2)

# Plugin sources
set(PLUGIN_SOURCES
    nip34-git-plugin.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/gnostr-plugin-api.c
)

# Add libgit2-dependent sources if libgit2 is found
if(LIBGIT2_FOUND)
    list(APPEND PLUGIN_SOURCES
        gnostr-git-client.c
    )
endif()

# Create shared library
add_library(${PLUGIN_NAME} MODULE ${PLUGIN_SOURCES})

# Include directories
target_include_directories(${PLUGIN_NAME} PRIVATE
    ${GNOSTR_APP_DIR}/src
    ${PROJECT_ROOT}/nips/nip34/include
    ${PROJECT_ROOT}/nips/nip46/include
    ${PROJECT_ROOT}/gobject/include
    ${PROJECT_ROOT}/libnostr/include
    ${PROJECT_ROOT}/libgo/include
    ${CMAKE_BINARY_DIR}/apps/gnostr/generated
)

# Plugin build mode - excludes host-only code from gnostr-plugin-api.c
target_compile_definitions(${PLUGIN_NAME} PRIVATE GNOSTR_PLUGIN_BUILD=1)

# Add libgit2 compile definitions and includes if found
if(LIBGIT2_FOUND)
    target_compile_definitions(${PLUGIN_NAME} PRIVATE HAVE_LIBGIT2=1)
    target_include_directories(${PLUGIN_NAME} PRIVATE ${LIBGIT2_INCLUDE_DIRS})
endif()

# Link libraries
target_link_libraries(${PLUGIN_NAME} PRIVATE
    PkgConfig::GTK4
    PkgConfig::GLIB
    PkgConfig::LIBPEAS
    PkgConfig::JSON_GLIB
)

# Link libgo for include paths (nsync, etc)
if (TARGET libgo)
    target_link_libraries(${PLUGIN_NAME} PRIVATE libgo)
endif()

# Link NIP-34 library if available
if (TARGET nostr_nip34_core)
    target_link_libraries(${PLUGIN_NAME} PRIVATE nostr_nip34_core)
endif()

# Link libgit2 if found
if(LIBGIT2_FOUND)
    target_link_libraries(${PLUGIN_NAME} PRIVATE ${LIBGIT2_LIBRARIES})
    target_link_directories(${PLUGIN_NAME} PRIVATE ${LIBGIT2_LIBRARY_DIRS})
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
