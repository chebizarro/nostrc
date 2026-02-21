# GNostr apps umbrella

option(ENABLE_APPS "Build GNostr apps" ON)

if (NOT ENABLE_APPS)
  message(STATUS "GNostr apps disabled (ENABLE_APPS=OFF)")
  return()
endif()

# Common deps detection via pkg-config
find_package(PkgConfig QUIET)
if (NOT PKG_CONFIG_FOUND)
  message(STATUS "pkg-config not found; skipping GNostr apps")
  return()
endif()

# Check GTK4, GLib/GIO, libsecret, sqlite3
pkg_check_modules(GTK4 QUIET IMPORTED_TARGET gtk4)
pkg_check_modules(GLIB QUIET IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)
pkg_check_modules(SQLITE3 QUIET IMPORTED_TARGET sqlite3)

if (NOT GTK4_FOUND OR NOT GLIB_FOUND)
  message(STATUS "GTK4/GLib not available; skipping GNostr desktop apps")
endif()

add_subdirectory(relayctl)
add_subdirectory(grelay)

# Blossom Cache - standalone local Blossom cache server (requires GLib + libsoup3 + sqlite3)
option(ENABLE_BLOSSOM_CACHE "Build blossom-cache local Blossom cache server" ON)
if (ENABLE_BLOSSOM_CACHE AND GLIB_FOUND)
  pkg_check_modules(BLOSSOM_SOUP3 QUIET IMPORTED_TARGET libsoup-3.0)
  if (BLOSSOM_SOUP3_FOUND AND SQLITE3_FOUND)
    add_subdirectory(blossom-cache)
  else()
    message(STATUS "blossom-cache: skipped (requires libsoup-3.0 and sqlite3)")
  endif()
endif()

if (GTK4_FOUND AND GLIB_FOUND)
  add_subdirectory(gnostr-signer)
  add_subdirectory(gnostr)
endif()
