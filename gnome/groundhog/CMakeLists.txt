cmake_minimum_required(VERSION 3.16)
project(groundhog VERSION 0.5.3 LANGUAGES C)

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

# All Groundhog UI is Blueprint, following the repository convention
# (apps/gnostr/CMakeLists.txt, nostr-gtk): data/ui/*.blp are the source of
# truth and the compiled data/ui/*.ui are committed beside them, so a build
# without blueprint-compiler still bundles a complete UI. When a compiler of at
# least GROUNDHOG_BLUEPRINT_MIN_VERSION is found, the .blp files are compiled
# into the build tree and glib-compile-resources prefers those over the
# committed copies (first --sourcedir wins). Unlike gnostr, a normal build never
# rewrites tracked files: CI (0.12, Ubuntu 24.04) and developer hosts (e.g.
# 0.20) emit trivially different XML. The groundhog-blueprint test instead
# fails when the committed .ui no longer matches the .blp, and the
# groundhog-update-ui target regenerates them on request.
set(GROUNDHOG_BLUEPRINTS
  gh-window gh-sidebar-page gh-content-page gh-onboarding-page gh-account-ui)
set(GROUNDHOG_BLUEPRINT_MIN_VERSION 0.12.0)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/Blueprint.cmake)
set(GROUNDHOG_BLUEPRINT_COMPILER "")
if (BLUEPRINT_COMPILER)
  execute_process(COMMAND ${BLUEPRINT_COMPILER} --version
    OUTPUT_VARIABLE GROUNDHOG_BLUEPRINT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GROUNDHOG_BLUEPRINT_VERSION_RESULT ERROR_QUIET)
  if (GROUNDHOG_BLUEPRINT_VERSION_RESULT EQUAL 0 AND
      GROUNDHOG_BLUEPRINT_VERSION VERSION_GREATER_EQUAL GROUNDHOG_BLUEPRINT_MIN_VERSION)
    set(GROUNDHOG_BLUEPRINT_COMPILER ${BLUEPRINT_COMPILER})
    message(STATUS "Groundhog UI: compiling Blueprint with blueprint-compiler ${GROUNDHOG_BLUEPRINT_VERSION}")
  else()
    message(WARNING "Groundhog UI: blueprint-compiler '${GROUNDHOG_BLUEPRINT_VERSION}' is older than "
                    "${GROUNDHOG_BLUEPRINT_MIN_VERSION}; using the committed data/ui/*.ui")
  endif()
else()
  message(STATUS "Groundhog UI: blueprint-compiler not found; using the committed data/ui/*.ui")
endif()

set(GROUNDHOG_BLUEPRINT_SOURCES)
set(GROUNDHOG_COMMITTED_UI)
foreach (name IN LISTS GROUNDHOG_BLUEPRINTS)
  list(APPEND GROUNDHOG_BLUEPRINT_SOURCES ${GROUNDHOG_DATA}/ui/${name}.blp)
  list(APPEND GROUNDHOG_COMMITTED_UI ${GROUNDHOG_DATA}/ui/${name}.ui)
endforeach()
if (GROUNDHOG_BLUEPRINT_COMPILER)
  # Writes ${CMAKE_CURRENT_BINARY_DIR}/blueprint/data/ui/<name>.ui.
  blueprint_compile(
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/blueprint
    SOURCES ${GROUNDHOG_BLUEPRINT_SOURCES}
    OUTPUT_FILES_VAR GROUNDHOG_UI_FILES)
  set(GROUNDHOG_UI_SOURCEDIR --sourcedir ${CMAKE_CURRENT_BINARY_DIR}/blueprint/data)
  set(GROUNDHOG_UPDATE_UI_COMMANDS)
  foreach (name IN LISTS GROUNDHOG_BLUEPRINTS)
    list(APPEND GROUNDHOG_UPDATE_UI_COMMANDS
      COMMAND ${GROUNDHOG_BLUEPRINT_COMPILER} compile
              --output ${GROUNDHOG_DATA}/ui/${name}.ui ${GROUNDHOG_DATA}/ui/${name}.blp)
  endforeach()
  # Explicit, never part of ALL: refreshes the committed fallback .ui files.
  add_custom_target(groundhog-update-ui ${GROUNDHOG_UPDATE_UI_COMMANDS}
    COMMENT "Regenerating committed Groundhog data/ui/*.ui from Blueprint" VERBATIM)
else()
  set(GROUNDHOG_UI_FILES ${GROUNDHOG_COMMITTED_UI})
  set(GROUNDHOG_UI_SOURCEDIR)
endif()

set(GROUNDHOG_RESOURCE_C ${CMAKE_CURRENT_BINARY_DIR}/groundhog-resources.c)
add_custom_command(
  OUTPUT ${GROUNDHOG_RESOURCE_C}
  COMMAND ${GLIB_COMPILE_RESOURCES} --generate-source --manual-register --c-name groundhog
          ${GROUNDHOG_UI_SOURCEDIR} --sourcedir ${GROUNDHOG_DATA}
          --target ${GROUNDHOG_RESOURCE_C}
          ${GROUNDHOG_DATA}/groundhog.gresource.xml
  DEPENDS ${GROUNDHOG_DATA}/groundhog.gresource.xml
          ${GROUNDHOG_DATA}/style.css
          ${GROUNDHOG_DATA}/icons/org.nostr.Groundhog.svg
          ${GROUNDHOG_UI_FILES}
  VERBATIM)

# The window and page templates plus the resource bundle that holds their
# compiled UI, shared by the application and the GUI tests so every test
# instantiates exactly the tree the application ships.
add_library(groundhog-ui STATIC src/app/gh-shell.c src/app/gh-window.c ${GROUNDHOG_RESOURCE_C})
target_include_directories(groundhog-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
target_compile_definitions(groundhog-ui PUBLIC
  ADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_5
  ADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_5
  GDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14
  GDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14)
target_compile_options(groundhog-ui PRIVATE -Wall -Wextra -Werror)
target_link_libraries(groundhog-ui PUBLIC PkgConfig::GROUNDHOG_DEPS)

add_executable(groundhog src/main.c)
target_compile_definitions(groundhog PRIVATE GROUNDHOG_VERSION="${PROJECT_VERSION}")
target_compile_options(groundhog PRIVATE -Wall -Wextra -Werror)
target_link_libraries(groundhog PRIVATE groundhog-ui)

if (BUILD_TESTING)
  add_executable(test-groundhog-resource tests/app/test_resource.c)
  target_link_libraries(test-groundhog-resource PRIVATE groundhog-ui)
  target_compile_options(test-groundhog-resource PRIVATE -Wall -Wextra -Werror)
  add_test(NAME groundhog-resource COMMAND test-groundhog-resource)
  set_tests_properties(groundhog-resource PROPERTIES TIMEOUT 10
    ENVIRONMENT "G_DEBUG=fatal-criticals")

  # Layout/accessibility check for the shell templates: instantiates the
  # compiled GhWindow/GhSidebarPage/GhContentPage from the GResource (no
  # GSettings or signer). It needs a GTK display connection, so it self-skips
  # (77) when one is unavailable, matching groundhog-launch below.
  add_executable(test-groundhog-shell tests/app/test_shell_layout.c)
  target_compile_options(test-groundhog-shell PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(test-groundhog-shell PRIVATE groundhog-ui)
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
  # The outbound codec and the inbound unwrap/seen-set are part of the
  # production executable, not test-only helpers. No relay lookup, inbox REQ
  # or publish path is linked here.
  if (TARGET nostr_nip17_core AND TARGET nostr_nip59_core)
    target_sources(groundhog PRIVATE src/app/gh-nip17-envelope.c src/app/gh-nip17-inbox.c)
    target_link_libraries(groundhog PRIVATE nostr_nip17_core nostr_nip59_core)
  endif()
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

# Groundhog-owned URL-scoped live REQ coordinator and per-relay signed-event
# publisher. The standalone shell build remains usable without the in-tree
# relay libraries.
if (TARGET nostr_gobject AND TARGET nostr)
  add_library(groundhog-relay STATIC
    src/relay/gh-relay-scope.c src/relay/gh-relay-gnostr.c
    src/relay/gh-relay-publish.c src/relay/gh-relay-publish-gnostr.c)
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
    add_executable(test-groundhog-relay-publish tests/relay/test_relay_publish.c)
    target_link_libraries(test-groundhog-relay-publish PRIVATE groundhog-relay)
    target_compile_options(test-groundhog-relay-publish PRIVATE -Wall -Wextra -Werror)
    add_test(NAME groundhog-relay-publish COMMAND test-groundhog-relay-publish)
    set_tests_properties(groundhog-relay-publish PROPERTIES TIMEOUT 20)
    pkg_check_modules(GROUNDHOG_RELAY_WIRE_TEST QUIET IMPORTED_TARGET libsoup-3.0)
    if (TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      add_executable(test-groundhog-relay-wire tests/relay/test_relay_wire.c)
      target_link_libraries(test-groundhog-relay-wire PRIVATE
        groundhog-relay PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      target_compile_options(test-groundhog-relay-wire PRIVATE -Wall -Wextra -Werror)
      add_test(NAME groundhog-relay-wire COMMAND test-groundhog-relay-wire)
      set_tests_properties(groundhog-relay-wire PROPERTIES TIMEOUT 20)
      add_executable(test-groundhog-relay-publish-wire
        tests/relay/test_relay_publish_wire.c)
      target_link_libraries(test-groundhog-relay-publish-wire PRIVATE
        groundhog-relay PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      target_compile_options(test-groundhog-relay-publish-wire PRIVATE
        -Wall -Wextra -Werror)
      add_test(NAME groundhog-relay-publish-wire
        COMMAND test-groundhog-relay-publish-wire)
      set_tests_properties(groundhog-relay-publish-wire PROPERTIES TIMEOUT 40)
    endif()
  endif()
endif()

# Account-bound relay-list discovery: one URL-scoped live REQ per active
# account generation, cancelled on switch. Needs the account and relay layers.
if (TARGET groundhog-account AND TARGET groundhog-relay)
  add_library(groundhog-account-relays STATIC src/app/gh-account-relays.c)
  target_link_libraries(groundhog-account-relays PUBLIC groundhog-account groundhog-relay)
  target_compile_options(groundhog-account-relays PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(groundhog PRIVATE groundhog-account-relays)
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_RELAYS=1)
else()
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_RELAYS=0)
endif()

# GTK-free NIP-17 conversation model: messages, rooms keyed by participant set
# and the per-account store (in-memory backend; see gh-conversation-store.h).
if (TARGET nostr AND TARGET nip19)
  pkg_check_modules(GROUNDHOG_GIO REQUIRED IMPORTED_TARGET gio-2.0>=2.72)
  add_library(groundhog-conversations STATIC
    src/app/gh-message.c src/app/gh-conversation.c src/app/gh-conversation-store.c)
  target_include_directories(groundhog-conversations PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
  target_link_libraries(groundhog-conversations PUBLIC nostr nip19 PkgConfig::GROUNDHOG_GIO)
  target_compile_options(groundhog-conversations PRIVATE -Wall -Wextra -Werror)
  if (BUILD_TESTING)
    add_executable(test-groundhog-conversations tests/app/test_conversation_store.c)
    target_link_libraries(test-groundhog-conversations PRIVATE groundhog-conversations)
    target_compile_options(test-groundhog-conversations PRIVATE -Wall -Wextra -Werror)
    add_test(NAME groundhog-conversations COMMAND test-groundhog-conversations)
    set_tests_properties(groundhog-conversations PROPERTIES TIMEOUT 20
      ENVIRONMENT "G_DEBUG=fatal-criticals,fatal-warnings")
  endif()
endif()

# GTK-free NIP-17 receive pipeline: the active account's kind-1059 REQ on its
# own 10050 relays, seen-set pre-filter, bounded signer unwraps, store
# admission. It carries its own copy of the unwrap (gh-nip17-inbox.c) so it
# links standalone; it is not yet linked into the executable, which compiles
# that file directly. Replace that target_sources entry with this library when
# the inbox is wired into the application.
if (TARGET groundhog-account-relays AND TARGET groundhog-conversations)
  add_library(groundhog-dm-inbox STATIC src/app/gh-dm-inbox.c src/app/gh-nip17-inbox.c)
  target_link_libraries(groundhog-dm-inbox PUBLIC groundhog-account-relays
                        groundhog-conversations)
  target_compile_options(groundhog-dm-inbox PRIVATE -Wall -Wextra -Werror)
endif()

# GTK-free NIP-29 relay-group core: (relay URL, group id) identity, admission
# of relay-signed 39000-39003 snapshots, role checks and unsigned event
# templates. No transport, subscription or UI is linked here. nips/nip29 is
# only added by ENABLE_NIP29=ON, so pull it in here otherwise (as the identity
# block does for gnostr-secret); that also registers its test_nip29 CTest.
if (TARGET nostr AND TARGET nip19)
  if (NOT TARGET nip29)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../nips/nip29
                     ${CMAKE_CURRENT_BINARY_DIR}/nip29)
  endif()
  get_target_property(GROUNDHOG_NIP29_SOURCE_DIR nip29 SOURCE_DIR)
  pkg_check_modules(GROUNDHOG_NIP29_GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.66)
  add_library(groundhog-nip29 STATIC
    src/nip29/gh-nip29-group.c src/nip29/gh-nip29-template.c)
  target_include_directories(groundhog-nip29 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nip29 ${GROUNDHOG_NIP29_SOURCE_DIR}/include)
  target_link_libraries(groundhog-nip29 PUBLIC nip29 nostr PkgConfig::GROUNDHOG_NIP29_GLIB)
  target_compile_options(groundhog-nip29 PRIVATE -Wall -Wextra -Werror)
  if (BUILD_TESTING)
    add_executable(test-groundhog-nip29 tests/nip29/test_nip29_group.c)
    target_link_libraries(test-groundhog-nip29 PRIVATE groundhog-nip29)
    target_compile_options(test-groundhog-nip29 PRIVATE -Wall -Wextra -Werror)
    add_test(NAME groundhog-nip29 COMMAND test-groundhog-nip29)
    set_tests_properties(groundhog-nip29 PROPERTIES TIMEOUT 20
      ENVIRONMENT "G_DEBUG=fatal-criticals,fatal-warnings")
  endif()
else()
  message(STATUS "Groundhog NIP-29 core unavailable: in-tree nostr and nip19 targets required")
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
  if (TARGET nostr_nip17_core AND TARGET nostr_nip59_core)
    target_sources(test-groundhog-account PRIVATE src/app/gh-nip17-envelope.c
                   src/app/gh-nip17-inbox.c)
    target_link_libraries(test-groundhog-account PRIVATE nostr_nip17_core nostr_nip59_core)
    target_compile_definitions(test-groundhog-account PRIVATE GROUNDHOG_TEST_NIP17=1)
  endif()
  if (TARGET groundhog-relay)
    target_link_libraries(test-groundhog-account PRIVATE groundhog-relay)
    target_compile_definitions(test-groundhog-account PRIVATE GROUNDHOG_TEST_RELAY=1)
  endif()
  target_compile_options(test-groundhog-account PRIVATE -Wall -Wextra -Werror)
  add_dependencies(test-groundhog-account groundhog-test-schemas)
  add_test(NAME groundhog-account COMMAND test-groundhog-account)
  set_tests_properties(groundhog-account PROPERTIES TIMEOUT 30
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals,fatal-warnings")

  # Account switch/teardown and relay-list admission against a recording
  # transport and, when libsoup is present, a real local WebSocket relay.
  if (TARGET groundhog-account-relays)
    add_executable(test-groundhog-account-relays tests/app/test_account_relays.c)
    target_link_libraries(test-groundhog-account-relays PRIVATE groundhog-account-relays)
    if (TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      target_link_libraries(test-groundhog-account-relays PRIVATE
        PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      target_compile_definitions(test-groundhog-account-relays PRIVATE GROUNDHOG_TEST_WIRE=1)
    endif()
    target_compile_options(test-groundhog-account-relays PRIVATE -Wall -Wextra -Werror)
    add_dependencies(test-groundhog-account-relays groundhog-test-schemas)
    add_test(NAME groundhog-account-relays COMMAND test-groundhog-account-relays)
    set_tests_properties(groundhog-account-relays PROPERTIES TIMEOUT 40
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()

  # The NIP-17 receive pipeline against recording transports and a mock
  # signer on a private bus (crafted peer wraps need the NIP-59/44 cores).
  if (TARGET groundhog-dm-inbox AND TARGET nostr_nip59_core)
    add_executable(test-groundhog-dm-inbox tests/app/test_dm_inbox.c)
    target_link_libraries(test-groundhog-dm-inbox PRIVATE groundhog-dm-inbox
                          nostr_nip17_core nostr_nip59_core)
    target_compile_options(test-groundhog-dm-inbox PRIVATE -Wall -Wextra -Werror)
    add_dependencies(test-groundhog-dm-inbox groundhog-test-schemas)
    add_test(NAME groundhog-dm-inbox COMMAND test-groundhog-dm-inbox)
    set_tests_properties(groundhog-dm-inbox PROPERTIES TIMEOUT 60
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals,fatal-warnings")
  endif()

  # Check that gh-account-ui.c attaches the gh-account-ui.blp pages/menu to a
  # real GhWindow and that its focus/announcement bookkeeping (added for
  # accessibility) only reacts to a real state transition. Needs
  # a GTK display, unlike the GTK-free tests above, so it self-skips (77);
  # -fatal-warnings is intentionally omitted here (unlike groundhog-account
  # above) because initializing libadwaita's bundled CSS can log
  # non-fatal theme-parser warnings on some GTK builds.
  add_executable(test-groundhog-account-ui tests/app/test_account_ui.c
    src/app/gh-account-ui.c)
  target_link_libraries(test-groundhog-account-ui PRIVATE groundhog-account groundhog-ui)
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
    # The committed data/ui/*.ui fallback must match fresh Blueprint output
    # (compared as canonical XML, so 0.12 and newer compilers agree). Fix a
    # failure by building the groundhog-update-ui target and committing.
    if (GROUNDHOG_BLUEPRINT_COMPILER)
      add_test(NAME groundhog-blueprint
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_blueprint.py
                ${GROUNDHOG_BLUEPRINT_COMPILER} ${GROUNDHOG_DATA}/ui)
      set_tests_properties(groundhog-blueprint PROPERTIES TIMEOUT 60)
    endif()
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
