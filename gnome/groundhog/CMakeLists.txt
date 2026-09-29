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
  gh-window gh-sidebar-page gh-content-page gh-onboarding-page gh-account-ui
  gh-conversation-row gh-message-item gh-shortcuts-window)
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
# instantiates exactly the tree the application ships. GTK-only: the typed
# conversation UI (groundhog-conversation-ui below) binds its models.
add_library(groundhog-ui STATIC src/app/gh-shell.c src/app/gh-window.c src/ui/gh-status.c
            ${GROUNDHOG_RESOURCE_C})
target_include_directories(groundhog-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
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
  # Private D-Bus buses for the signer, account, DM and keyring tests
  # (tests/common/nostrc-test-bus.h; never GTestDBus).
  include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcTestBus.cmake)
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
  # The outbound codec is part of the production executable, not a test-only
  # helper. The inbound unwrap/seen-set (gh-nip17-inbox.c) comes with the
  # groundhog-dm-inbox library the G11 block links; no publish path is
  # linked here.
  if (TARGET nostr_nip17_core AND TARGET nostr_nip59_core)
    target_sources(groundhog PRIVATE src/app/gh-nip17-envelope.c)
    target_link_libraries(groundhog PRIVATE nostr_nip17_core nostr_nip59_core)
  endif()
  if (BUILD_TESTING)
    add_executable(test-groundhog-identity tests/identity/test_identity.c)
    target_link_libraries(test-groundhog-identity PRIVATE groundhog-identity nostrc-test-bus)
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
    src/relay/gh-relay-publish.c src/relay/gh-relay-publish-gnostr.c
    src/relay/gh-relay-auth.c src/relay/gh-relay-gnostr-write.c)
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

# GTK-free NIP-17 receive pipeline: the active account's kind-1059 REQs on its
# own 10050 relays (paged backwards with until, NIP-42 AUTH as the account on
# those relays only, through GhAccountAuth), seen-set pre-filter, bounded
# signer unwraps, store admission. It carries the unwrap (gh-nip17-inbox.c);
# the executable links it through groundhog-conversation-ui (G11 block below).
if (TARGET groundhog-account-relays AND TARGET groundhog-conversations)
  add_library(groundhog-dm-inbox STATIC src/app/gh-dm-inbox.c src/app/gh-nip17-inbox.c
    src/app/gh-account-auth.c)
  target_link_libraries(groundhog-dm-inbox PUBLIC groundhog-account-relays
                        groundhog-conversations)
  target_compile_options(groundhog-dm-inbox PRIVATE -Wall -Wextra -Werror)
endif()

# NIP-17 send pipeline: recipient kind-10050 lookup (URL-scoped REQ, no
# kind-10002 fallback) and per-relay publication of the recipient wrap and the
# sender self-copy. Part of the executable like the envelope codec it drives;
# no UI or durable outbox uses it yet.
if (TARGET groundhog-account-relays AND TARGET nostr_nip17_core AND TARGET nostr_nip59_core)
  set(GROUNDHOG_DM_SEND_SOURCES src/app/gh-inbox-resolver.c src/app/gh-inbox-lookup.c
    src/app/gh-dm-send.c)
  target_sources(groundhog PRIVATE ${GROUNDHOG_DM_SEND_SOURCES})
endif()

# --- G11 Shell IA: conversation list, inbox status, NIP-17 inbox wiring ---
# The typed half of the shell: GhConversationRow, the conversation-list binder
# (accepted/request filters, search, selection -> read-only messages) and the
# inbox banner adapter. Built with the NIP-17 receive pipeline, which the
# executable then runs: one GhConversationStore and GhDmInbox per process
# (src/main.c, nostrc-qp24.10.9).
if (TARGET groundhog-dm-inbox)
  add_library(groundhog-conversation-ui STATIC
    src/ui/gh-conversation-row.c src/ui/gh-conversation-list.c src/ui/gh-inbox-status.c)
  target_include_directories(groundhog-conversation-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
  target_link_libraries(groundhog-conversation-ui PUBLIC groundhog-ui groundhog-dm-inbox)
  target_compile_options(groundhog-conversation-ui PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(groundhog PRIVATE groundhog-conversation-ui)
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_INBOX=1)
else()
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_INBOX=0)
endif()
# --- end G11 ---

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
  target_link_libraries(test-groundhog-account PRIVATE groundhog-account nostrc-test-bus)
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
                          nostr_nip17_core nostr_nip59_core nostrc-test-bus)
    target_compile_options(test-groundhog-dm-inbox PRIVATE -Wall -Wextra -Werror)
    add_dependencies(test-groundhog-dm-inbox groundhog-test-schemas)
    add_test(NAME groundhog-dm-inbox COMMAND test-groundhog-dm-inbox)
    set_tests_properties(groundhog-dm-inbox PROPERTIES TIMEOUT 60
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals,fatal-warnings")
  endif()

  # Send pipeline against recording scope/publish transports and the mock
  # signer bus and, when libsoup is present, real local WebSocket relays.
  if (GROUNDHOG_DM_SEND_SOURCES)
    add_executable(test-groundhog-dm-send tests/app/test_dm_send.c
      ${GROUNDHOG_DM_SEND_SOURCES} src/app/gh-nip17-envelope.c)
    target_link_libraries(test-groundhog-dm-send PRIVATE groundhog-account-relays
      nostr_nip17_core nostr_nip59_core nostrc-test-bus)
    if (TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      target_link_libraries(test-groundhog-dm-send PRIVATE PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      target_compile_definitions(test-groundhog-dm-send PRIVATE GROUNDHOG_TEST_WIRE=1)
    endif()
    target_compile_options(test-groundhog-dm-send PRIVATE -Wall -Wextra -Werror)
    add_dependencies(test-groundhog-dm-send groundhog-test-schemas)
    add_test(NAME groundhog-dm-send COMMAND test-groundhog-dm-send)
    set_tests_properties(groundhog-dm-send PROPERTIES TIMEOUT 90
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
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

  # --- G11 Shell IA: conversation list test ---
  # A real GhConversationStore fed through its admission API with fixture
  # rumors (no relay, no signer): order, badges, requests, search, selection,
  # banners, accessibility and the 360x294 collapsed layout, plus the inbox
  # status wiring on a real account/relay/inbox stack with no relay set up.
  # Needs a display, so it self-skips (77) like the other GUI tests.
  if (TARGET groundhog-conversation-ui)
    add_executable(test-groundhog-conversation-list tests/ui/test_conversation_list.c)
    target_link_libraries(test-groundhog-conversation-list PRIVATE groundhog-conversation-ui)
    target_compile_options(test-groundhog-conversation-list PRIVATE -Wall -Wextra -Werror)
    add_dependencies(test-groundhog-conversation-list groundhog-test-schemas)
    add_test(NAME groundhog-conversation-list COMMAND test-groundhog-conversation-list)
    set_tests_properties(groundhog-conversation-list PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
  # --- end G11 ---
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
  # Memory settings: the smoke never reads a real account, so it contacts no
  # relay and writes no inbox state.
  set_tests_properties(groundhog-launch PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR};GSETTINGS_BACKEND=memory")
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

# --- G01 privacy static ---
# Static privacy guards from the privacy charter (§8.2 G01): no URL literals,
# the GSettings allowlist pinned to name/type/default, the Blueprint class
# denylist, libsoup/gdk-pixbuf/tmp boundaries, NIP-17 publication relay
# sources, GhMessageStatus values and the app ID. --self-test also proves each
# rule fails on a synthetic violating tree. Needs only python3.
if (BUILD_TESTING)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if (Python3_Interpreter_FOUND)
    add_test(NAME groundhog-privacy-static
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_privacy.py
              --self-test ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(groundhog-privacy-static PROPERTIES TIMEOUT 60
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
  endif()
endif()
# --- end G01 privacy static ---

# --- G03 store key ---
# Custody of the per-account store key (charter §3.4, D2): 32 random bytes in
# a Groundhog-owned Secret Service item (org.nostr.Groundhog.StoreKey), held
# only in libsodium guarded memory, with explicit LOCKED/NOT_FOUND/UNAVAILABLE
# states and no plaintext fallback. GTK-free; G04 links it into the app.
pkg_check_modules(GROUNDHOG_STORE_KEY_DEPS QUIET IMPORTED_TARGET
  glib-2.0>=2.66 gio-2.0 libsecret-1 libsodium)
if (TARGET PkgConfig::GROUNDHOG_STORE_KEY_DEPS)
  add_library(groundhog-store-key STATIC src/store/gh-store-key.c)
  target_include_directories(groundhog-store-key PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
  target_link_libraries(groundhog-store-key PUBLIC PkgConfig::GROUNDHOG_STORE_KEY_DEPS)
  target_compile_options(groundhog-store-key PRIVATE -Wall -Wextra -Werror)
  if (BUILD_TESTING)
    # KC-1..KC-5 against the in-process FakeSecret backend, plus the libsecret
    # backend on a private bus with no Secret Service (KC-4).
    add_executable(test-groundhog-store-key
      tests/store/test_store_key.c tests/store/fake-secret.c)
    target_link_libraries(test-groundhog-store-key PRIVATE groundhog-store-key)
    target_compile_options(test-groundhog-store-key PRIVATE -Wall -Wextra -Werror)
    add_test(NAME groundhog-store-key COMMAND test-groundhog-store-key)
    set_tests_properties(groundhog-store-key PROPERTIES TIMEOUT 30
      ENVIRONMENT "G_DEBUG=fatal-criticals,fatal-warnings")
    # KC-6: a real gnome-keyring on a private bus (or, with
    # GH_STORE_KEY_TEST_SESSION_KEYRING=1, the session's). Skips (77) only
    # when gnome-keyring-daemon or dbus-daemon is not installed.
    add_executable(test-groundhog-store-key-keyring
      tests/store/test_store_key_keyring.c tests/store/fake-secret.c)
    target_link_libraries(test-groundhog-store-key-keyring PRIVATE groundhog-store-key nostrc-test-bus)
    target_compile_options(test-groundhog-store-key-keyring PRIVATE -Wall -Wextra -Werror)
    add_test(NAME groundhog-store-key-keyring COMMAND test-groundhog-store-key-keyring)
    set_tests_properties(groundhog-store-key-keyring PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90
      ENVIRONMENT "G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog store key unavailable: libsecret-1 and libsodium are required")
endif()
# --- end G03 store key ---

# --- G02 store ---
# Encrypted per-account store (privacy charter §3, decision D1: SQLCipher 4
# keyed with a raw 256-bit key). Optional at configure time: without SQLCipher
# or libsodium the store is disabled; there is never a plaintext fallback.
pkg_check_modules(GROUNDHOG_SQLCIPHER QUIET IMPORTED_TARGET sqlcipher)
pkg_check_modules(GROUNDHOG_STORE_DEPS QUIET IMPORTED_TARGET gio-2.0>=2.66 libsodium>=1.0.18)
if (TARGET PkgConfig::GROUNDHOG_SQLCIPHER AND TARGET PkgConfig::GROUNDHOG_STORE_DEPS)
  # Homebrew's sqlcipher.pc reports the SQLite base version, Debian's the
  # SQLCipher one; the cipher itself is verified at runtime (ST-4 guard).
  message(STATUS "Groundhog store: enabled (pkg-config sqlcipher "
                 "${GROUNDHOG_SQLCIPHER_VERSION})")
  set(GROUNDHOG_STORE_SOURCES
    src/store/gh-store.c src/store/gh-store-schema.c src/store/gh-clock.c)

  # ST-4: one SQLite implementation, SQLCipher, per process. The linked
  # target records libsqlcipher as its first DT_NEEDED entry (kept even under
  # --as-needed), so it precedes any libsqlite3 that libsoup, libmarmot or a
  # later dependency pulls in, and ELF global lookup binds every sqlite3_* to
  # SQLCipher. Mach-O binds each image to the library it linked (two-level
  # namespace); the order is kept there only for symmetry. The runtime guard
  # (gh_store_check_sqlite) refuses to open a store if this ever breaks.
  function(groundhog_store_link_sqlcipher_first target)
    foreach (lib IN LISTS GROUNDHOG_SQLCIPHER_LINK_LIBRARIES)
      if (APPLE)
        target_link_options(${target} BEFORE PRIVATE "${lib}")
      else()
        target_link_options(${target} BEFORE PRIVATE
          "SHELL:-Wl,--push-state,--no-as-needed ${lib} -Wl,--pop-state")
      endif()
    endforeach()
  endfunction()

  function(groundhog_store_library name)
    add_library(${name} STATIC ${GROUNDHOG_STORE_SOURCES})
    target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
    # sqlite3_key_v2 is declared by SQLCipher's sqlite3.h only with this set.
    target_compile_definitions(${name} PUBLIC SQLITE_HAS_CODEC=1)
    target_link_libraries(${name} PUBLIC PkgConfig::GROUNDHOG_SQLCIPHER
      PkgConfig::GROUNDHOG_STORE_DEPS ${CMAKE_DL_LIBS})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
  endfunction()

  groundhog_store_library(groundhog-store)
  groundhog_store_link_sqlcipher_first(groundhog)
  target_link_libraries(groundhog PRIVATE groundhog-store)
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_STORE=1)

  if (BUILD_TESTING)
    # The same sources with the H8 crash cut points and the ownership hook;
    # never linked into the application.
    groundhog_store_library(groundhog-store-testhooks)
    target_compile_definitions(groundhog-store-testhooks PUBLIC GH_STORE_TEST_HOOKS=1)
    add_executable(test-groundhog-store tests/store/test_store.c tests/store/crash-harness.c)
    target_include_directories(test-groundhog-store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/store)
    target_link_libraries(test-groundhog-store PRIVATE groundhog-store-testhooks)
    target_compile_options(test-groundhog-store PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-store)
    add_test(NAME groundhog-store COMMAND test-groundhog-store)
    set_tests_properties(groundhog-store PROPERTIES TIMEOUT 120
      ENVIRONMENT "G_DEBUG=fatal-criticals")

    # ST-4 against what ships: the production library linked the way the
    # groundhog executable is, plus the real groundhog binary's loader scope
    # and bindings (glibc) and a preload mutation that the guard must refuse.
    add_executable(test-groundhog-store-sqlite tests/store/test_store.c)
    target_compile_definitions(test-groundhog-store-sqlite PRIVATE GH_STORE_TEST_LINK_SET=1
      "GROUNDHOG_EXECUTABLE=\"$<TARGET_FILE:groundhog>\"")
    target_link_libraries(test-groundhog-store-sqlite PRIVATE groundhog-store
      $<TARGET_PROPERTY:groundhog,LINK_LIBRARIES>)
    target_compile_options(test-groundhog-store-sqlite PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-store-sqlite)
    add_dependencies(test-groundhog-store-sqlite groundhog)
    add_test(NAME groundhog-store-sqlite COMMAND test-groundhog-store-sqlite)
    set_tests_properties(groundhog-store-sqlite PROPERTIES TIMEOUT 60
      ENVIRONMENT "G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog store: disabled. It needs the pkg-config modules "
                 "'sqlcipher' and 'libsodium' (Debian/Ubuntu: libsqlcipher-dev "
                 "libsodium-dev; Homebrew: sqlcipher libsodium). There is no "
                 "plaintext fallback, so nothing is stored without them.")
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_STORE=0)
endif()
# --- end G02 store ---

# --- G23 store marmot ---
# libmarmot's MarmotStorage over the encrypted per-account GhStore (privacy
# charter §3.9, decision D5 = vtable): MLS state, its snapshots and the outbox
# share one SQLCipher transaction (T-mls). Needs the GhStore (G02) and the
# in-tree libmarmot target; skipped cleanly without either. Nothing in the
# application uses it yet (qp24.7 and qp24.13 will link it), so the
# executable's link set is unchanged.
if (TARGET groundhog-store AND TARGET marmot)
  message(STATUS "Groundhog store marmot: enabled")
  add_library(groundhog-store-marmot STATIC src/store/gh-store-marmot.c)
  target_include_directories(groundhog-store-marmot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
  target_link_libraries(groundhog-store-marmot PUBLIC groundhog-store marmot)
  target_compile_options(groundhog-store-marmot PRIVATE -Wall -Wextra -Werror)
  if (BUILD_TESTING)
    # The adapter compiled against the test-hooks store (H8 crash cut points):
    # libmarmot's storage contract cases, snapshots, T-mls under the crash
    # harness and an end-to-end libmarmot group across store reopens. libmarmot
    # links libsqlite3, so SQLCipher goes first here as in the application
    # (ST-4); every store open re-checks that at runtime.
    add_executable(test-groundhog-store-marmot tests/store/test_store_marmot.c
      tests/store/crash-harness.c src/store/gh-store-marmot.c)
    target_include_directories(test-groundhog-store-marmot PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/store)
    target_link_libraries(test-groundhog-store-marmot PRIVATE groundhog-store-testhooks marmot)
    target_compile_options(test-groundhog-store-marmot PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-store-marmot)
    add_test(NAME groundhog-store-marmot COMMAND test-groundhog-store-marmot)
    set_tests_properties(groundhog-store-marmot PROPERTIES TIMEOUT 180
      ENVIRONMENT "G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog store marmot: disabled (needs the GhStore and the in-tree "
                 "libmarmot target)")
endif()
# --- end G23 store marmot ---

# --- G05 store conversations ---
# The encrypted store's persistence delegate for GhConversationStore (privacy
# charter §3.3, §3.5, §8.2 G05): T-admit (message, room, seen keys and read
# state in one transaction), restore of rooms, read markers, requests and
# drafts on attach, paged older history, forget, and the one-time import of the
# legacy GhNip17Seen file. Needs the GhStore (G02) and the conversation model.
# Nothing in the application uses it yet (G04 wires it), so the executable's
# link set is unchanged.
if (TARGET groundhog-store AND TARGET groundhog-conversations)
  message(STATUS "Groundhog store conversations: enabled")
  add_library(groundhog-store-conversations STATIC src/store/gh-store-conversations.c)
  target_include_directories(groundhog-store-conversations PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
  target_link_libraries(groundhog-store-conversations PUBLIC groundhog-store
    groundhog-conversations)
  target_compile_options(groundhog-store-conversations PRIVATE -Wall -Wextra -Werror)
  if (BUILD_TESTING)
    # The delegate compiled against the test-hooks store (H8 crash cut points):
    # ST-6 admit cut points under the crash harness, ST-7, ST-9, ST-12, EX-4/6,
    # restart restore (order, unread, requests, drafts, paging), the rejected
    # namespace, per-account isolation and a canary scan of the store files.
    add_executable(test-groundhog-store-conversations
      tests/store/test_store_conversations.c tests/store/crash-harness.c
      src/store/gh-store-conversations.c)
    target_include_directories(test-groundhog-store-conversations PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/store ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
    target_link_libraries(test-groundhog-store-conversations PRIVATE
      groundhog-store-testhooks groundhog-conversations)
    target_compile_options(test-groundhog-store-conversations PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-store-conversations)
    add_test(NAME groundhog-store-conversations COMMAND test-groundhog-store-conversations)
    set_tests_properties(groundhog-store-conversations PROPERTIES TIMEOUT 120
      ENVIRONMENT "G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog store conversations: disabled (needs the GhStore and the "
                 "conversation model)")
endif()
# --- end G05 store conversations ---

# --- G06 outbox ---
# Durable NIP-17 outbox over the encrypted GhStore (privacy charter §3.5,
# §3.6, item G06): T-enqueue before any signer call, T-seal before any
# publish, per-relay T-outcome, the retry scheduler on GhClock, and the
# GTK-free GhMessageStatus derivation. Nothing in the application uses it
# yet (G04/G13 wire it by adding GROUNDHOG_OUTBOX_SOURCES to the executable),
# so the executable's link set is unchanged.
if (TARGET groundhog-store AND GROUNDHOG_DM_SEND_SOURCES)
  message(STATUS "Groundhog outbox: enabled")
  set(GROUNDHOG_OUTBOX_SOURCES src/app/gh-outbox.c src/app/gh-message-status.c)
  if (BUILD_TESTING AND TARGET groundhog-test-schemas)
    # OB-1..OB-10 against the test-hooks store (H8 crash cut points), the
    # fake GhClock, recording transports and the mock signer bus; with
    # libsoup also real local WebSocket relays (H2).
    add_executable(test-groundhog-outbox tests/app/test_outbox.c tests/store/crash-harness.c
      ${GROUNDHOG_OUTBOX_SOURCES} ${GROUNDHOG_DM_SEND_SOURCES} src/app/gh-nip17-envelope.c)
    target_include_directories(test-groundhog-outbox PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/store)
    target_link_libraries(test-groundhog-outbox PRIVATE groundhog-account-relays
      groundhog-store-testhooks nostr_nip17_core nostr_nip59_core nostrc-test-bus)
    if (TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      target_link_libraries(test-groundhog-outbox PRIVATE PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      target_compile_definitions(test-groundhog-outbox PRIVATE GROUNDHOG_TEST_WIRE=1)
    endif()
    target_compile_options(test-groundhog-outbox PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-outbox)
    add_dependencies(test-groundhog-outbox groundhog-test-schemas)
    add_test(NAME groundhog-outbox COMMAND test-groundhog-outbox)
    set_tests_properties(groundhog-outbox PROPERTIES TIMEOUT 180
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog outbox: disabled (needs the GhStore and the NIP-17 send sources)")
endif()
# --- end G06 outbox ---
