# nostr-nautilus — GNOME Files context-menu items for Nostr (nostrc-xlf3).
#
# A libnautilus-extension-4 (Nautilus 43+) module that shells out to the
# nostr-share and nostr-seal CLIs; it contains no Nostr code of its own.
#
#   nostr_nautilus_core  gating + argv (GLib/GIO only; builds everywhere,
#                        compiles nostr-share's pure ns-kind.c classifier)
#   nostr-nautilus       the MODULE (needs libnautilus-extension-4)
#   nostr-nautilus-seal  libexec helper: npub entry for "Encrypt for…"
#                        (needs libadwaita; nostr-seal-gtk only decrypts)
cmake_minimum_required(VERSION 3.20)

find_package(PkgConfig REQUIRED)
include(GNUInstallDirs)
pkg_check_modules(NN_GIO REQUIRED IMPORTED_TARGET glib-2.0>=2.74 gio-2.0>=2.74)

set(_nn_share_src ${CMAKE_CURRENT_SOURCE_DIR}/../nostr-share/src)
set(_nn_warn -Wall -Wextra -Werror)
set(NN_SEAL_HELPER_PATH ${CMAKE_INSTALL_FULL_LIBEXECDIR}/nostr-nautilus-seal)

add_library(nostr_nautilus_core STATIC
  src/nn-core.c
  src/nn-launch.c
  ${_nn_share_src}/ns-kind.c)
target_include_directories(nostr_nautilus_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src ${_nn_share_src})
target_compile_options(nostr_nautilus_core PRIVATE ${_nn_warn})
target_link_libraries(nostr_nautilus_core PUBLIC PkgConfig::NN_GIO)
set_property(TARGET nostr_nautilus_core PROPERTY POSITION_INDEPENDENT_CODE ON)

# --- The Nautilus module --------------------------------------------------
pkg_check_modules(NN_EXT QUIET IMPORTED_TARGET libnautilus-extension-4>=43 gio-unix-2.0)
# GDK supplies the launch context (startup notification / xdg-activation
# token) so helper windows are raised; Nautilus 43+ is GTK 4 already.
pkg_check_modules(NN_GTK QUIET IMPORTED_TARGET gtk4>=4.10)

if(NN_EXT_FOUND)
  # Relative to the install prefix (Debian: lib/<triplet>, Fedora: lib64)
  # so staged and non-/usr installs work; the default matches
  # `pkg-config --variable=extensiondir libnautilus-extension-4`.
  set(NOSTR_NAUTILUS_EXTENSION_DIR "${CMAKE_INSTALL_LIBDIR}/nautilus/extensions-4"
      CACHE STRING "Where the nostr-nautilus module is installed")
  pkg_get_variable(_nn_pc_extdir libnautilus-extension-4 extensiondir)
  if(_nn_pc_extdir AND NOT _nn_pc_extdir STREQUAL "${CMAKE_INSTALL_PREFIX}/${NOSTR_NAUTILUS_EXTENSION_DIR}"
     AND NOT _nn_pc_extdir STREQUAL "${NOSTR_NAUTILUS_EXTENSION_DIR}")
    message(STATUS "nostr-nautilus: installing to ${NOSTR_NAUTILUS_EXTENSION_DIR}; "
                   "Nautilus looks in ${_nn_pc_extdir}")
  endif()

  add_library(nostr-nautilus MODULE src/nostr-nautilus.c)
  set_target_properties(nostr-nautilus PROPERTIES PREFIX "lib" OUTPUT_NAME "nostr-nautilus"
                        C_VISIBILITY_PRESET hidden)
  target_compile_options(nostr-nautilus PRIVATE ${_nn_warn})
  target_compile_definitions(nostr-nautilus PRIVATE
    "NN_SEAL_HELPER_PATH=\"${NN_SEAL_HELPER_PATH}\"")
  target_link_libraries(nostr-nautilus PRIVATE nostr_nautilus_core PkgConfig::NN_EXT)
  if(NN_GTK_FOUND)
    target_compile_definitions(nostr_nautilus_core PRIVATE NN_HAVE_GDK)
    target_link_libraries(nostr_nautilus_core PRIVATE PkgConfig::NN_GTK)
  endif()
  install(TARGETS nostr-nautilus LIBRARY DESTINATION ${NOSTR_NAUTILUS_EXTENSION_DIR})
  message(STATUS "nostr-nautilus: building the Files extension (GDK launch context: ${NN_GTK_FOUND})")
else()
  message(STATUS "nostr-nautilus: libnautilus-extension-4 not found — core + tests only")
endif()

# --- Encrypt dialog (libexec) ---------------------------------------------
option(NOSTR_NAUTILUS_ENABLE_SEAL_HELPER "Build the nostr-nautilus-seal npub entry dialog" ON)
if(NOSTR_NAUTILUS_ENABLE_SEAL_HELPER)
  pkg_check_modules(NN_ADW QUIET IMPORTED_TARGET libadwaita-1>=1.4 gtk4>=4.10)
endif()
if(NOSTR_NAUTILUS_ENABLE_SEAL_HELPER AND NN_ADW_FOUND)
  add_executable(nostr-nautilus-seal src/nn-seal-helper.c)
  target_compile_options(nostr-nautilus-seal PRIVATE ${_nn_warn})
  target_link_libraries(nostr-nautilus-seal PRIVATE nostr_nautilus_core PkgConfig::NN_ADW)
  install(TARGETS nostr-nautilus-seal RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
else()
  message(STATUS "nostr-nautilus: no libadwaita >= 1.4 — \"Encrypt for Nostr Contact…\" disabled")
endif()

install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nostr-nautilus)

# --- Tests -----------------------------------------------------------------
if(BUILD_TESTING)
  add_executable(test-nostr-nautilus-core tests/test-nn-core.c)
  target_compile_options(test-nostr-nautilus-core PRIVATE ${_nn_warn})
  target_link_libraries(test-nostr-nautilus-core PRIVATE nostr_nautilus_core)
  if(TARGET nostr-seal)
    # Run the dialog's argv against the real CLI.
    target_compile_definitions(test-nostr-nautilus-core PRIVATE
      "NN_SEAL_CLI_PATH=\"$<TARGET_FILE:nostr-seal>\"")
    add_dependencies(test-nostr-nautilus-core nostr-seal)
  endif()
  if(UNIX AND NOT APPLE)
    pkg_check_modules(NN_GIO_UNIX QUIET IMPORTED_TARGET gio-unix-2.0)
    if(NN_GIO_UNIX_FOUND)
      target_link_libraries(test-nostr-nautilus-core PRIVATE PkgConfig::NN_GIO_UNIX)
    endif()
  endif()
  add_test(NAME nostr_nautilus_core COMMAND test-nostr-nautilus-core)
  set_tests_properties(nostr_nautilus_core PROPERTIES LABELS "nostr-nautilus" TIMEOUT 60)

  if(NN_EXT_FOUND)
    # The module's own sources against fake NautilusFileInfo objects.
    add_executable(test-nostr-nautilus-provider tests/test-nn-provider.c src/nostr-nautilus.c)
    target_compile_options(test-nostr-nautilus-provider PRIVATE ${_nn_warn})
    target_compile_definitions(test-nostr-nautilus-provider PRIVATE
      "NN_SEAL_HELPER_PATH=\"/nonexistent/nostr-nautilus-seal\""
      "NN_SHARE_DESKTOP_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/../nostr-share/data/org.nostr.Share.desktop\"")
    target_link_libraries(test-nostr-nautilus-provider PRIVATE nostr_nautilus_core PkgConfig::NN_EXT)
    add_test(NAME nostr_nautilus_provider COMMAND test-nostr-nautilus-provider)
    set_tests_properties(nostr_nautilus_provider PROPERTIES LABELS "nostr-nautilus" TIMEOUT 60)
  endif()
endif()
