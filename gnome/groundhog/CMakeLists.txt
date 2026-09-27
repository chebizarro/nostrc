cmake_minimum_required(VERSION 3.16)
project(groundhog VERSION 0.3.0 LANGUAGES C)

include(GNUInstallDirs)
if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  include(CTest)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(GROUNDHOG_DEPS REQUIRED IMPORTED_TARGET gtk4>=4.14 libadwaita-1>=1.5)
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources)
find_program(GLIB_COMPILE_SCHEMAS glib-compile-schemas)
if (NOT GLIB_COMPILE_RESOURCES OR NOT GLIB_COMPILE_SCHEMAS)
  message(FATAL_ERROR "Groundhog requires glib-compile-resources and glib-compile-schemas")
endif()

set(GROUNDHOG_DATA ${CMAKE_CURRENT_SOURCE_DIR}/data)
set(GROUNDHOG_RESOURCE_C ${CMAKE_CURRENT_BINARY_DIR}/groundhog-resources.c)
add_custom_command(
  OUTPUT ${GROUNDHOG_RESOURCE_C}
  COMMAND ${GLIB_COMPILE_RESOURCES} --generate-source --manual-register --c-name groundhog
          --sourcedir ${GROUNDHOG_DATA} --target ${GROUNDHOG_RESOURCE_C}
          ${GROUNDHOG_DATA}/groundhog.gresource.xml
  DEPENDS ${GROUNDHOG_DATA}/groundhog.gresource.xml
          ${GROUNDHOG_DATA}/style.css
          ${GROUNDHOG_DATA}/icons/org.nostr.Groundhog.svg
  VERBATIM)

add_executable(groundhog src/main.c src/app/gh-shell.c ${GROUNDHOG_RESOURCE_C})
target_include_directories(groundhog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
target_compile_definitions(groundhog PRIVATE
  GROUNDHOG_VERSION="${PROJECT_VERSION}"
  ADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_5
  ADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_5
  GDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14
  GDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14)
target_compile_options(groundhog PRIVATE -Wall -Wextra -Werror)
target_link_libraries(groundhog PRIVATE PkgConfig::GROUNDHOG_DEPS)

# Headless adaptive-layout/accessibility check for the shell chrome in
# src/app/gh-shell.c. It never touches GResource, GSettings, or the signer,
# so it needs no compiled resource bundle or installed schema; it only
# needs a GTK display connection, which is why it still self-skips (77)
# when one is unavailable, matching groundhog-launch below.
if (BUILD_TESTING)
  add_executable(test-groundhog-shell tests/app/test_shell_layout.c src/app/gh-shell.c)
  target_include_directories(test-groundhog-shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
  target_compile_definitions(test-groundhog-shell PRIVATE GROUNDHOG_HAVE_ACCOUNTS=0)
  target_compile_options(test-groundhog-shell PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(test-groundhog-shell PRIVATE PkgConfig::GROUNDHOG_DEPS)
  add_test(NAME groundhog-shell COMMAND test-groundhog-shell)
  set_tests_properties(groundhog-shell PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20
    ENVIRONMENT "G_DEBUG=fatal-criticals")
endif()

# The identity adapter consumes the in-tree NIP-19 and event validators. Keep
# standalone shell builds working when those parent targets are not present.
if (TARGET nostr AND TARGET nip19)
  if (NOT TARGET gnostr-secret)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../seahorse
                     ${CMAKE_CURRENT_BINARY_DIR}/gnostr-secret)
  endif()
  add_library(groundhog-identity STATIC
    src/identity/gh-identity.c src/identity/gh-signer.c)
  target_include_directories(groundhog-identity PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/identity)
  target_link_libraries(groundhog-identity PUBLIC gnostr-secret nip19 nostr)
  target_compile_options(groundhog-identity PRIVATE -Wall -Wextra -Werror)

  # Groundhog-only active account: metadata discovery, selection, signer
  # reachability and account generations. GTK-free so it is tested headless.
  add_library(groundhog-account STATIC src/app/gh-account-controller.c)
  target_include_directories(groundhog-account PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
  target_link_libraries(groundhog-account PUBLIC groundhog-identity)
  target_compile_options(groundhog-account PRIVATE -Wall -Wextra -Werror)
  target_sources(groundhog PRIVATE src/app/gh-account-ui.c)
  target_link_libraries(groundhog PRIVATE groundhog-account)
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_ACCOUNTS=1)
  if (BUILD_TESTING)
    add_executable(test-groundhog-identity tests/identity/test_identity.c)
    target_link_libraries(test-groundhog-identity PRIVATE groundhog-identity)
    target_compile_options(test-groundhog-identity PRIVATE -Wall -Wextra -Werror)
    add_test(NAME groundhog-identity COMMAND test-groundhog-identity)
    set_tests_properties(groundhog-identity PROPERTIES TIMEOUT 20
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR};GSETTINGS_BACKEND=memory")
  endif()
else()
  message(STATUS "Groundhog identity adapter unavailable: in-tree nostr and nip19 targets required")
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_ACCOUNTS=0)
endif()

# Groundhog-owned URL-scoped live REQ coordinator. The standalone shell build
# remains usable without the in-tree relay libraries.
if (TARGET nostr_gobject AND TARGET nostr)
  add_library(groundhog-relay STATIC
    src/relay/gh-relay-scope.c src/relay/gh-relay-gnostr.c)
  target_include_directories(groundhog-relay PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/relay)
  target_link_libraries(groundhog-relay PUBLIC nostr_gobject nostr)
  target_compile_options(groundhog-relay PRIVATE -Wall -Wextra -Werror)
  if (BUILD_TESTING)
    add_executable(test-groundhog-relay tests/relay/test_relay_scope.c)
    target_link_libraries(test-groundhog-relay PRIVATE groundhog-relay)
    target_compile_options(test-groundhog-relay PRIVATE -Wall -Wextra -Werror)
    add_test(NAME groundhog-relay COMMAND test-groundhog-relay)
    set_tests_properties(groundhog-relay PROPERTIES TIMEOUT 20)
    pkg_check_modules(GROUNDHOG_RELAY_WIRE_TEST QUIET IMPORTED_TARGET libsoup-3.0)
    if (TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      add_executable(test-groundhog-relay-wire tests/relay/test_relay_wire.c)
      target_link_libraries(test-groundhog-relay-wire PRIVATE
        groundhog-relay PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      target_compile_options(test-groundhog-relay-wire PRIVATE -Wall -Wextra -Werror)
      add_test(NAME groundhog-relay-wire COMMAND test-groundhog-relay-wire)
      set_tests_properties(groundhog-relay-wire PROPERTIES TIMEOUT 20)
    endif()
  endif()
endif()

# The account test compiles Gnostr's schema beside Groundhog's (read-only) to
# prove a Groundhog switch never writes Gnostr's current identity.
set(GROUNDHOG_GNOSTR_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/../../apps/gnostr/data/schemas/org.gnostr.gnostr.gschema.xml)
if (BUILD_TESTING AND TARGET groundhog-account AND EXISTS ${GROUNDHOG_GNOSTR_SCHEMA})
  set(GROUNDHOG_TEST_SCHEMAS ${CMAKE_CURRENT_BINARY_DIR}/test-schemas)
  add_custom_command(
    OUTPUT ${GROUNDHOG_TEST_SCHEMAS}/gschemas.compiled
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GROUNDHOG_TEST_SCHEMAS}
    COMMAND ${CMAKE_COMMAND} -E copy ${GROUNDHOG_DATA}/org.nostr.Groundhog.gschema.xml
            ${GROUNDHOG_GNOSTR_SCHEMA} ${GROUNDHOG_TEST_SCHEMAS}
    COMMAND ${GLIB_COMPILE_SCHEMAS} --strict ${GROUNDHOG_TEST_SCHEMAS}
    DEPENDS ${GROUNDHOG_DATA}/org.nostr.Groundhog.gschema.xml ${GROUNDHOG_GNOSTR_SCHEMA}
    VERBATIM)
  add_custom_target(groundhog-test-schemas DEPENDS ${GROUNDHOG_TEST_SCHEMAS}/gschemas.compiled)
  add_executable(test-groundhog-account tests/app/test_account_controller.c)
  target_link_libraries(test-groundhog-account PRIVATE groundhog-account)
  if (TARGET groundhog-relay)
    target_link_libraries(test-groundhog-account PRIVATE groundhog-relay)
    target_compile_definitions(test-groundhog-account PRIVATE GROUNDHOG_TEST_RELAY=1)
  endif()
  target_compile_options(test-groundhog-account PRIVATE -Wall -Wextra -Werror)
  add_dependencies(test-groundhog-account groundhog-test-schemas)
  add_test(NAME groundhog-account COMMAND test-groundhog-account)
  set_tests_properties(groundhog-account PROPERTIES TIMEOUT 30
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals,fatal-warnings")

  # Headless check that gh-account-ui.c's focus/announcement bookkeeping
  # (added for accessibility) only reacts to a real state transition. Needs
  # a GTK display, unlike the GTK-free tests above, so it self-skips (77);
  # -fatal-warnings is intentionally omitted here (unlike groundhog-account
  # above) because initializing libadwaita's bundled CSS can log
  # non-fatal theme-parser warnings on some GTK builds.
  add_executable(test-groundhog-account-ui tests/app/test_account_ui.c
    src/app/gh-account-ui.c src/app/gh-shell.c)
  target_include_directories(test-groundhog-account-ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
  target_compile_definitions(test-groundhog-account-ui PRIVATE GROUNDHOG_HAVE_ACCOUNTS=1)
  target_link_libraries(test-groundhog-account-ui PRIVATE groundhog-account PkgConfig::GROUNDHOG_DEPS)
  target_compile_options(test-groundhog-account-ui PRIVATE -Wall -Wextra -Werror)
  add_dependencies(test-groundhog-account-ui groundhog-test-schemas)
  add_test(NAME groundhog-account-ui COMMAND test-groundhog-account-ui)
  set_tests_properties(groundhog-account-ui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
endif()

set(GROUNDHOG_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
configure_file(${GROUNDHOG_DATA}/org.nostr.Groundhog.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Groundhog.desktop @ONLY)
configure_file(${GROUNDHOG_DATA}/org.nostr.Groundhog.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Groundhog.service @ONLY)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gschemas.compiled
  COMMAND ${CMAKE_COMMAND} -E copy
          ${GROUNDHOG_DATA}/org.nostr.Groundhog.gschema.xml
          ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Groundhog.gschema.xml
  COMMAND ${GLIB_COMPILE_SCHEMAS} --strict ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${GROUNDHOG_DATA}/org.nostr.Groundhog.gschema.xml
  VERBATIM)
add_custom_target(groundhog-schema ALL
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gschemas.compiled)
if (TARGET test-groundhog-identity)
  add_dependencies(test-groundhog-identity groundhog-schema)
endif()

install(TARGETS groundhog RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Groundhog.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Groundhog.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES ${GROUNDHOG_DATA}/org.nostr.Groundhog.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES ${GROUNDHOG_DATA}/org.nostr.Groundhog.gschema.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)
install(FILES ${GROUNDHOG_DATA}/icons/org.nostr.Groundhog.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

if (BUILD_TESTING)
  add_test(NAME groundhog-version COMMAND groundhog --version)
  set_tests_properties(groundhog-version PROPERTIES PASS_REGULAR_EXPRESSION "Groundhog ${PROJECT_VERSION}")
  add_test(NAME groundhog-launch COMMAND groundhog --smoke)
  set_tests_properties(groundhog-launch PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME groundhog-schema
    COMMAND ${GLIB_COMPILE_SCHEMAS} --strict --dry-run ${CMAKE_CURRENT_BINARY_DIR})
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if (Python3_Interpreter_FOUND)
    add_test(NAME groundhog-data
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_data.py
              ${CMAKE_CURRENT_BINARY_DIR} ${GROUNDHOG_DATA})
  endif()
  find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
  if (DESKTOP_FILE_VALIDATE)
    add_test(NAME groundhog-desktop
      COMMAND ${DESKTOP_FILE_VALIDATE} ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Groundhog.desktop)
  endif()
  find_program(APPSTREAMCLI appstreamcli)
  if (APPSTREAMCLI)
    add_test(NAME groundhog-metainfo
      COMMAND ${APPSTREAMCLI} validate --no-net ${GROUNDHOG_DATA}/org.nostr.Groundhog.metainfo.xml)
  endif()
endif()
