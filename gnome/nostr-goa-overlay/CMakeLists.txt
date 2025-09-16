cmake_minimum_required(VERSION 3.18)
project(nostr_goa_overlay C)

include(ExternalProject)

# Prefix for user-scoped installs by default
if (NOT DEFINED USER_LOCAL_PREFIX)
  if (DEFINED ENV{HOME})
    set(USER_LOCAL_PREFIX "$ENV{HOME}/.local")
  else()
    set(USER_LOCAL_PREFIX "${CMAKE_BINARY_DIR}/_local")
  endif()
endif()
message(STATUS "Installing overlay (system GOA) to: ${USER_LOCAL_PREFIX}")

# Build overlay (provider + overlay assets) with Meson
ExternalProject_Add(nostr_goa_overlay_build
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  CONFIGURE_COMMAND meson setup _build --prefix=${USER_LOCAL_PREFIX}
  BUILD_COMMAND meson compile -C _build
  INSTALL_COMMAND meson install -C _build
  BUILD_IN_SOURCE 1
)

# Convenience target
add_custom_target(install_nostr_goa_overlay ALL DEPENDS nostr_goa_overlay_build)
