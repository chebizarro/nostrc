# GNOME Shell extension "Nostr" (nostr@nostr.org), bead nostrc-33f1.
# Pure data (GJS + GSettings schema): nothing is compiled.
include(GNUInstallDirs)

set(NOSTR_SHELL_EXTENSION_UUID "nostr@nostr.org")
set(_nse_dir "${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${NOSTR_SHELL_EXTENSION_UUID}")
set(_nse_schema "schemas/org.gnome.shell.extensions.nostr.gschema.xml")

install(FILES metadata.json extension.js prefs.js DESTINATION ${_nse_dir})
# Every module under lib/ ships: an explicit list once left out dm.js and
# the installed extension failed to import it (nostrc-51ax).
install(DIRECTORY lib/ DESTINATION ${_nse_dir}/lib FILES_MATCHING PATTERN "*.js")
install(FILES icons/nostr-relay-symbolic.svg DESTINATION ${_nse_dir}/icons)
# System-wide schema: Extension.getSettings() falls back to the default
# schema source when the extension directory has no schemas/ subdir.
install(FILES ${_nse_schema} DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/gnome-shell-extension-nostr)

# Distro packages compile schemas from their glib triggers (DESTDIR set);
# a plain `cmake --install` compiles here.
install(CODE "
  if (\"\$ENV{DESTDIR}\" STREQUAL \"\")
    find_program(_nse_gcs glib-compile-schemas)
    if (_nse_gcs)
      message(STATUS \"Compiling GSettings schemas in ${CMAKE_INSTALL_FULL_DATADIR}/glib-2.0/schemas\")
      execute_process(COMMAND \${_nse_gcs} \"${CMAKE_INSTALL_FULL_DATADIR}/glib-2.0/schemas\")
    endif()
  endif()
")

# `make nostr-shell-extension-zip` → nostr@nostr.org.shell-extension.zip for
# `gnome-extensions install` / Extension Manager (per-user install).
find_program(NSE_GNOME_EXTENSIONS gnome-extensions)
if (NSE_GNOME_EXTENSIONS)
  add_custom_target(nostr-shell-extension-zip
    COMMAND ${NSE_GNOME_EXTENSIONS} pack --force
            --extra-source=lib --extra-source=icons
            --schema=${_nse_schema}
            --out-dir=${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Packing ${NOSTR_SHELL_EXTENSION_UUID}.shell-extension.zip"
    VERBATIM)
endif()

if (BUILD_TESTING)
  find_program(NSE_GLIB_COMPILE_SCHEMAS glib-compile-schemas)
  if (NSE_GLIB_COMPILE_SCHEMAS)
    add_test(NAME nostr-shell-extension-schema
             COMMAND ${NSE_GLIB_COMPILE_SCHEMAS} --strict --dry-run
                     ${CMAKE_CURRENT_SOURCE_DIR}/schemas)
  endif()
  find_program(NSE_NODE node)
  if (NSE_NODE)
    add_test(NAME nostr-shell-extension-state-node
             COMMAND ${NSE_NODE} tests/run-tests.js
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif()
  find_program(NSE_GJS gjs)
  if (NSE_GJS)
    add_test(NAME nostr-shell-extension-state-gjs
             COMMAND ${NSE_GJS} -m tests/run-tests.js
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    find_program(NSE_DBUS_RUN_SESSION dbus-run-session)
    if (NSE_DBUS_RUN_SESSION AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
      add_test(NAME nostr-shell-extension-monitors
               COMMAND sh tests/run-monitors.sh ${NSE_GJS}
               WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
      set_tests_properties(nostr-shell-extension-monitors PROPERTIES TIMEOUT 120)
    endif()
  endif()
endif()
