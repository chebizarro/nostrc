cmake_minimum_required(VERSION 3.16)
project(groundhog VERSION 0.7.0 LANGUAGES C)

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
  gh-window gh-sidebar-page gh-content-page gh-onboarding-view gh-onboarding-row gh-account-ui
  gh-conversation-row gh-shortcuts-window
  gh-conversation-view gh-message-row gh-message-list-item gh-day-separator
  gh-delivery-details gh-preferences-dialog gh-composer
  gh-requests-view gh-new-message-dialog gh-new-message-row gh-recipient-chip)
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
          ${GROUNDHOG_DATA}/relay-suggestions.json
          ${GROUNDHOG_DATA}/icons/scalable/actions/send-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/send-symbolic-rtl.svg
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
# A message's status is the honest GhMessageStatus of gh-message-status.h,
# whose header names the relay publish outcomes (enums only, no link).
if (TARGET nostr AND TARGET nip19)
  pkg_check_modules(GROUNDHOG_GIO REQUIRED IMPORTED_TARGET gio-2.0>=2.72)
  add_library(groundhog-conversations STATIC
    src/app/gh-message.c src/app/gh-conversation.c src/app/gh-conversation-store.c
    src/app/gh-message-status.c)
  target_include_directories(groundhog-conversations PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app
                             ${CMAKE_CURRENT_SOURCE_DIR}/src/relay)
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
# G08: GhAuthPolicy, the single NIP-42 identity decision per (connection
# purpose, URL) (charter §4.3, §4.4), with GhAccountAuth, the one account
# signer per generation it hands out (R6). The inbox, the send pipeline, the
# outbox and the lookups all link it (see the G08 block at the end).
if (TARGET groundhog-account AND TARGET groundhog-relay)
  add_library(groundhog-auth-policy STATIC src/app/gh-auth-policy.c src/app/gh-account-auth.c)
  target_include_directories(groundhog-auth-policy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
  target_link_libraries(groundhog-auth-policy PUBLIC groundhog-account groundhog-relay)
  target_compile_options(groundhog-auth-policy PRIVATE -Wall -Wextra -Werror)
endif()

if (TARGET groundhog-account-relays AND TARGET groundhog-conversations)
  add_library(groundhog-dm-inbox STATIC src/app/gh-dm-inbox.c src/app/gh-nip17-inbox.c)
  target_link_libraries(groundhog-dm-inbox PUBLIC groundhog-auth-policy groundhog-account-relays
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
  target_link_libraries(groundhog PRIVATE groundhog-auth-policy)
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

# --- G04 account store and app services ---
# The active account's encrypted store over its lifetime (privacy charter
# §3.4, §3.8, §8.2 G04): key custody without prompting first, open in a
# worker, legacy seen/checkpoint import, the G05 delegate attached to the
# conversation model, the G06 outbox per open store, the inbox granted
# storage only while the store is open (or the in-memory store the user
# chose), and forget/crypto-shred; an old store always closes before the next
# opens (PT-6). src/main.c delegates every process-owned service to
# gh-app-services.c. Without the store (no SQLCipher or libsecret) the
# executable keeps the pre-store receive path.
target_sources(groundhog PRIVATE src/app/gh-app-services.c)
if (TARGET groundhog-store AND TARGET groundhog-store-key AND
    TARGET groundhog-store-conversations AND TARGET groundhog-dm-inbox AND
    TARGET groundhog-conversation-ui)
  message(STATUS "Groundhog account store: enabled")
  add_library(groundhog-account-store STATIC src/app/gh-account-store.c)
  target_include_directories(groundhog-account-store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
  target_link_libraries(groundhog-account-store PUBLIC groundhog-store-conversations
    groundhog-store-key groundhog-dm-inbox)
  target_compile_options(groundhog-account-store PRIVATE -Wall -Wextra -Werror)
  target_sources(groundhog PRIVATE src/ui/gh-store-status.c)
  target_link_libraries(groundhog PRIVATE groundhog-account-store)
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_ACCOUNT_STORE=1)
  if (GROUNDHOG_OUTBOX_SOURCES)
    target_sources(groundhog PRIVATE ${GROUNDHOG_OUTBOX_SOURCES} src/app/gh-app-outbox.c)
    target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_OUTBOX=1)
  else()
    target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_OUTBOX=0)
  endif()
  if (BUILD_TESTING AND TARGET groundhog-test-schemas AND GROUNDHOG_OUTBOX_SOURCES)
    # ST-8, KC-2/3/4 and PT-6 at app level: a real controller on a private
    # bus with the mock signer, the FakeSecret key backend (H5), recording
    # relay transports (H1), the real inbox, delegate and outbox over real
    # SQLCipher stores in private XDG directories. With a display, also the
    # window's history paging and the Start Fresh confirmation on that stack
    # (W13b review B1, non-blocking #3); those cases skip without one.
    add_executable(test-groundhog-account-store tests/app/test_account_store.c
      tests/app/account-store-outbox.c tests/store/fake-secret.c src/ui/gh-store-status.c
      src/app/gh-app-outbox.c ${GROUNDHOG_OUTBOX_SOURCES} ${GROUNDHOG_DM_SEND_SOURCES}
      src/app/gh-nip17-envelope.c)
    target_include_directories(test-groundhog-account-store PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/store ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
    target_link_libraries(test-groundhog-account-store PRIVATE groundhog-account-store
      groundhog-conversation-ui groundhog-ui nostr_nip17_core nostr_nip59_core nostrc-test-bus)
    target_compile_options(test-groundhog-account-store PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-account-store)
    add_dependencies(test-groundhog-account-store groundhog-test-schemas)
    add_test(NAME groundhog-account-store COMMAND test-groundhog-account-store)
    set_tests_properties(groundhog-account-store PROPERTIES TIMEOUT 120
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    # The window half (history paging after a restart, Start Fresh) needs a
    # display and one bus for the whole process; it self-skips (77) without.
    add_test(NAME groundhog-account-store-gui COMMAND test-groundhog-account-store --gui)
    set_tests_properties(groundhog-account-store-gui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog account store: disabled (needs the GhStore, GhStoreKey, the "
                 "store delegate and the NIP-17 inbox)")
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_ACCOUNT_STORE=0
    GROUNDHOG_HAVE_OUTBOX=0)
endif()
# --- end G04 account store and app services ---

# --- G12 Conversation view: bubbles, day separators, delivery, links ---
# The selected conversation's messages (privacy charter §7.4, §7.6):
# GhConversationView, GhMessageRow and GhDeliveryIndicator join the typed
# conversation UI (G11 block), and the GTK-free link policy (PD-3, PT-3) is a
# library of its own, tested without a display.
pkg_check_modules(GROUNDHOG_LINK_POLICY_DEPS REQUIRED IMPORTED_TARGET glib-2.0>=2.66)
add_library(groundhog-link-policy STATIC src/ui/gh-link-policy.c)
target_include_directories(groundhog-link-policy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
target_link_libraries(groundhog-link-policy PUBLIC PkgConfig::GROUNDHOG_LINK_POLICY_DEPS)
target_compile_options(groundhog-link-policy PRIVATE -Wall -Wextra -Werror)
if (BUILD_TESTING)
  add_executable(test-groundhog-link-policy tests/ui/test_link_policy.c)
  target_link_libraries(test-groundhog-link-policy PRIVATE groundhog-link-policy)
  target_compile_options(test-groundhog-link-policy PRIVATE -Wall -Wextra -Werror)
  add_test(NAME groundhog-link-policy COMMAND test-groundhog-link-policy)
  set_tests_properties(groundhog-link-policy PROPERTIES TIMEOUT 20
    ENVIRONMENT "G_DEBUG=fatal-criticals,fatal-warnings")
endif()
if (TARGET groundhog-conversation-ui)
  target_sources(groundhog-conversation-ui PRIVATE src/ui/gh-conversation-view.c
    src/ui/gh-message-row.c src/ui/gh-delivery-indicator.c)
  target_link_libraries(groundhog-conversation-ui PUBLIC groundhog-link-policy)
  # Bubbles, runs, day separators, delivery indicator and details, links,
  # previews behind consent, scrolling, paging, expiry and the §7.15 states
  # 11-13 on a real GhConversationStore with fixture rumors: no relay, no
  # signer, no network (a counting fake stands in for a preview fetcher).
  # Needs a display, so it self-skips (77) like the other GUI tests.
  if (BUILD_TESTING AND TARGET groundhog-test-schemas)
    add_executable(test-groundhog-conversation-view tests/ui/test_conversation_view.c)
    target_link_libraries(test-groundhog-conversation-view PRIVATE groundhog-conversation-ui)
    target_compile_options(test-groundhog-conversation-view PRIVATE -Wall -Wextra -Werror)
    add_dependencies(test-groundhog-conversation-view groundhog-test-schemas)
    add_test(NAME groundhog-conversation-view COMMAND test-groundhog-conversation-view)
    set_tests_properties(groundhog-conversation-view PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
endif()
# --- end G12 ---

# --- G15 background service lifecycle ---
# Background delivery with the window closed (privacy charter §5.3, §8.2
# G15): GhBackground holds the application while run-in-background is on,
# explains it once on the first close, and keeps autostart in line with a
# confirmed choice (the XDG autostart entry on a host install, the Background
# portal in a Flatpak sandbox). The entry's template is configured here and
# compiled in, so the file Groundhog writes is exactly the one validated below.
set(GROUNDHOG_AUTOSTART_DESKTOP ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Groundhog-autostart.desktop)
configure_file(${GROUNDHOG_DATA}/org.nostr.Groundhog-autostart.desktop.in
               ${GROUNDHOG_AUTOSTART_DESKTOP} @ONLY)
file(READ ${GROUNDHOG_AUTOSTART_DESKTOP} GROUNDHOG_AUTOSTART_TEMPLATE)
string(REPLACE "\\" "\\\\" GROUNDHOG_AUTOSTART_TEMPLATE "${GROUNDHOG_AUTOSTART_TEMPLATE}")
string(REPLACE "\"" "\\\"" GROUNDHOG_AUTOSTART_TEMPLATE "${GROUNDHOG_AUTOSTART_TEMPLATE}")
string(REPLACE "\n" "\\n\" \\\n  \"" GROUNDHOG_AUTOSTART_TEMPLATE "${GROUNDHOG_AUTOSTART_TEMPLATE}")
set(GROUNDHOG_BACKGROUND_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/background)
file(CONFIGURE OUTPUT ${GROUNDHOG_BACKGROUND_GENERATED}/gh-background-autostart.h
  CONTENT "/* Generated from data/org.nostr.Groundhog-autostart.desktop.in. */
#define GH_BACKGROUND_AUTOSTART_TEMPLATE \\
  \"@GROUNDHOG_AUTOSTART_TEMPLATE@\"
" @ONLY)
add_library(groundhog-background STATIC src/app/gh-background.c)
target_include_directories(groundhog-background PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app
                           PRIVATE ${GROUNDHOG_BACKGROUND_GENERATED})
target_link_libraries(groundhog-background PUBLIC groundhog-ui)
target_compile_options(groundhog-background PRIVATE -Wall -Wextra -Werror)
if (TARGET groundhog-account-store)
  # The portal status follows the account's store (locked, receiving).
  target_link_libraries(groundhog-background PUBLIC groundhog-account-store)
  target_compile_definitions(groundhog-background PRIVATE GH_BACKGROUND_WITH_STORE=1)
else()
  target_compile_definitions(groundhog-background PRIVATE GH_BACKGROUND_WITH_STORE=0)
endif()
if (TARGET groundhog-account)
  # run-in-background lives in the settings the account build brings up.
  target_link_libraries(groundhog PRIVATE groundhog-background)
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_BACKGROUND=1)
else()
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_BACKGROUND=0)
endif()
if (BUILD_TESTING)
  find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
  if (DESKTOP_FILE_VALIDATE)
    add_test(NAME groundhog-background-desktop
      COMMAND ${DESKTOP_FILE_VALIDATE} ${GROUNDHOG_AUTOSTART_DESKTOP})
  endif()
  if (TARGET groundhog-account-store AND TARGET groundhog-test-schemas)
    # NO-9…NO-12: the real inbox and store on the shared private bus with the
    # mock signer, FakeSecret (H5), recording transports (H1), a fake
    # Background portal, and the real executable for the process checks.
    add_executable(test-groundhog-background tests/app/test_background.c tests/store/fake-secret.c)
    target_include_directories(test-groundhog-background PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/store)
    target_link_libraries(test-groundhog-background PRIVATE groundhog-background
      groundhog-account-store nostr_nip17_core nostr_nip59_core nostrc-test-bus)
    target_compile_definitions(test-groundhog-background PRIVATE
      GROUNDHOG_TEST_EXECUTABLE="$<TARGET_FILE:groundhog>"
      GROUNDHOG_TEST_SCHEMA_DIR="${GROUNDHOG_TEST_SCHEMAS}"
      GROUNDHOG_TEST_AUTOSTART="${GROUNDHOG_AUTOSTART_DESKTOP}")
    target_compile_options(test-groundhog-background PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-background)
    add_dependencies(test-groundhog-background groundhog groundhog-test-schemas)
    add_test(NAME groundhog-background COMMAND test-groundhog-background)
    set_tests_properties(groundhog-background PROPERTIES TIMEOUT 180
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    # The window-close half (the one-time explanation) needs a display and no
    # private bus; it self-skips (77) without one, like groundhog-shell.
    add_test(NAME groundhog-background-gui COMMAND test-groundhog-background --gui)
    set_tests_properties(groundhog-background-gui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
endif()
# --- end G15 background service lifecycle ---

# --- G17 preferences dialog ---
# GhPreferencesDialog (privacy charter §7.11, §8.2 G17): an AdwPreferencesDialog
# whose rows bind every preference key of org.nostr.Groundhog both ways. GTK-only;
# the application opens it with app.preferences (gh-app-services.c), handing it
# the active account and GhAccountStore's forget for "Delete All Messages". A row
# whose feature the build lacks is gated (W13b review B2): src/app/gh-features.h
# is the one list of what the build performs, flipped one line per feature (G09
# Tor, G13 composer, G16 notifications, ...), and tests/check_privacy.py
# requires a consumer for every row it leaves live.
add_library(groundhog-preferences STATIC src/ui/gh-preferences-dialog.c)
target_include_directories(groundhog-preferences PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
target_link_libraries(groundhog-preferences PUBLIC groundhog-ui)
target_compile_options(groundhog-preferences PRIVATE -Wall -Wextra -Werror)
target_link_libraries(groundhog PRIVATE groundhog-preferences)
if (BUILD_TESTING)
  # Every row bound to its key both ways (memory GSettings backend), the rows
  # of this build gated as src/app/gh-features.h says, the Tor row hidden
  # without G09, the URL rules, and Delete All Messages running the forget
  # function after confirmation (a fake stands in for GhAccountStore).
  # Needs a display, so it self-skips (77) like the other GUI tests. With
  # GROUNDHOG_TEST_SCREENSHOTS=<dir> it also renders each page to PNGs.
  add_executable(test-groundhog-preferences tests/ui/test_preferences.c)
  target_link_libraries(test-groundhog-preferences PRIVATE groundhog-preferences)
  target_compile_options(test-groundhog-preferences PRIVATE -Wall -Wextra -Werror)
  add_dependencies(test-groundhog-preferences groundhog-schema)
  add_test(NAME groundhog-preferences COMMAND test-groundhog-preferences)
  set_tests_properties(groundhog-preferences PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
endif()
# --- end G17 preferences dialog ---

# --- G07 disappearing messages ---
# Disappearing messages and retention (privacy charter §3.7, §8.2 G07):
# per-conversation timers in the encrypted store, the exact rumor expiration
# and the jittered, hour-rounded seal and wrap expirations of outgoing
# messages (PT-7), and the GhClock purge scheduler that keeps the store, the
# conversation model and the outbox in step (EX-1, EX-3..EX-5). gh-outbox.c
# computes its messages' expirations with gh-expiry.c, so every target built
# from GROUNDHOG_OUTBOX_SOURCES gets it here (the outbox implies the store
# delegate: both need the GhStore and the in-tree nostr/nip19 targets).
if (GROUNDHOG_OUTBOX_SOURCES AND TARGET groundhog-store-conversations)
  message(STATUS "Groundhog expiry: enabled")
  set(GROUNDHOG_EXPIRY_SOURCES src/app/gh-expiry.c)
  if (TARGET groundhog-account-store)
    target_sources(groundhog PRIVATE ${GROUNDHOG_EXPIRY_SOURCES})
    target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_EXPIRY=1)
  else()
    target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_EXPIRY=0)
  endif()
  if (TARGET test-groundhog-account-store)
    target_sources(test-groundhog-account-store PRIVATE ${GROUNDHOG_EXPIRY_SOURCES})
  endif()
  if (TARGET test-groundhog-outbox)
    # The delegate compiled against the test-hooks store that test links.
    target_sources(test-groundhog-outbox PRIVATE ${GROUNDHOG_EXPIRY_SOURCES}
      src/store/gh-store-conversations.c)
    target_link_libraries(test-groundhog-outbox PRIVATE groundhog-conversations)
  endif()
  if (BUILD_TESTING AND TARGET groundhog-test-schemas)
    # EX-1 through real seals and the durable outbox (mock signer bus,
    # recording transports), PT-7 bounds, rounding and jitter on the fake
    # GhClock (H6), EX-3..EX-5 purges over real SQLCipher stores with the
    # conversation model, and T-purge crash cut points (H8).
    add_executable(test-groundhog-expiry tests/app/test_expiry.c tests/app/expiry-send.c
      tests/store/crash-harness.c ${GROUNDHOG_EXPIRY_SOURCES} ${GROUNDHOG_OUTBOX_SOURCES} ${GROUNDHOG_DM_SEND_SOURCES}
      src/app/gh-nip17-envelope.c src/store/gh-store-conversations.c)
    target_include_directories(test-groundhog-expiry PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/store ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
    target_link_libraries(test-groundhog-expiry PRIVATE groundhog-account-relays
      groundhog-conversations groundhog-store-testhooks nostr_nip17_core nostr_nip59_core
      nostrc-test-bus)
    target_compile_options(test-groundhog-expiry PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-expiry)
    add_dependencies(test-groundhog-expiry groundhog-test-schemas)
    add_test(NAME groundhog-expiry COMMAND test-groundhog-expiry)
    set_tests_properties(groundhog-expiry PROPERTIES TIMEOUT 180
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog expiry: disabled (needs the outbox and the store delegate)")
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_EXPIRY=0)
endif()
# --- end G07 disappearing messages ---

# --- G14 onboarding and inbox setup ---
# The first-run flow and the account's kind-10050 message relays (privacy
# charter §7.8, §8.2 G14, D4): GhInboxSetup (GTK-free: build, sign and
# publish the list to the chosen inbox, own 10002 write and discovery relays
# with account AUTH only on those publication connections, per-relay
# outcomes, and the unauthenticated private-reads probe) and
# GhOnboardingView (data/ui/gh-onboarding-view.blp). The reviewed
# data/relay-suggestions.json is bundled in the GResource. Needs the account,
# relay and NIP-17 inbox layers (GhAccountAuth is in groundhog-dm-inbox) and
# json-glib; without them the window shows no onboarding.
pkg_check_modules(GROUNDHOG_JSON QUIET IMPORTED_TARGET json-glib-1.0>=1.6)
if (TARGET groundhog-dm-inbox AND TARGET groundhog-conversation-ui AND
    TARGET PkgConfig::GROUNDHOG_JSON)
  message(STATUS "Groundhog onboarding: enabled")
  add_library(groundhog-inbox-setup STATIC src/app/gh-inbox-setup.c)
  target_include_directories(groundhog-inbox-setup PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
  target_link_libraries(groundhog-inbox-setup PUBLIC groundhog-dm-inbox PkgConfig::GROUNDHOG_JSON)
  target_compile_options(groundhog-inbox-setup PRIVATE -Wall -Wextra -Werror)
  add_library(groundhog-onboarding STATIC src/ui/gh-onboarding-view.c)
  target_include_directories(groundhog-onboarding PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
  target_link_libraries(groundhog-onboarding PUBLIC groundhog-ui groundhog-inbox-setup)
  target_compile_options(groundhog-onboarding PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(groundhog PRIVATE groundhog-onboarding)
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_ONBOARDING=1)
  if (BUILD_TESTING AND TARGET groundhog-test-schemas)
    # URL rules, the shipped suggestions, the 10050 event, the probe's
    # classification and GhInboxSetup against recording scope/publish
    # transports and the mock signer on a private bus: PT-9 (no connection
    # before the signer approves; none at all on denial), per-relay outcomes,
    # account AUTH only on publication connections, discovery adoption and
    # account switches.
    add_executable(test-groundhog-inbox-setup tests/app/test_inbox_setup.c)
    target_link_libraries(test-groundhog-inbox-setup PRIVATE groundhog-inbox-setup
      nostr_nip17_core nostr_nip59_core nostrc-test-bus)
    target_compile_definitions(test-groundhog-inbox-setup PRIVATE
      "GROUNDHOG_RELAY_SUGGESTIONS=\"${GROUNDHOG_DATA}/relay-suggestions.json\"")
    target_compile_options(test-groundhog-inbox-setup PRIVATE -Wall -Wextra -Werror)
    add_dependencies(test-groundhog-inbox-setup groundhog-test-schemas)
    add_test(NAME groundhog-inbox-setup COMMAND test-groundhog-inbox-setup)
    set_tests_properties(groundhog-inbox-setup PROPERTIES TIMEOUT 60
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals,fatal-warnings")
    # UX-6 and PT-9 through the real window: the onboarding view on a real
    # account controller (mock signer bus), relay lists, inbox and status
    # wiring with recording transports. Needs a display, so it self-skips
    # (77). With GROUNDHOG_TEST_SCREENSHOTS=<dir> it also renders each step.
    add_executable(test-groundhog-onboarding tests/ui/test_onboarding.c src/app/gh-account-ui.c)
    target_link_libraries(test-groundhog-onboarding PRIVATE groundhog-onboarding
      groundhog-conversation-ui nostr_nip17_core nostr_nip59_core nostrc-test-bus)
    target_compile_options(test-groundhog-onboarding PRIVATE -Wall -Wextra -Werror)
    add_dependencies(test-groundhog-onboarding groundhog-test-schemas)
    add_test(NAME groundhog-onboarding COMMAND test-groundhog-onboarding)
    set_tests_properties(groundhog-onboarding PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals;GIO_USE_NETWORK_MONITOR=base")
  endif()
else()
  message(STATUS "Groundhog onboarding: disabled (needs the NIP-17 inbox layers and json-glib-1.0)")
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_ONBOARDING=0)
endif()
# --- end G14 onboarding and inbox setup ---

# --- G16 private notifications ---
# GhNotifier (privacy charter §5.1, §5.2, §8.2 G16): GNotifications for newly
# admitted incoming messages at the chosen content level (hidden by default;
# requests always hidden), coalesced per id on GhClock, withdrawn on read,
# open, account switch, forget and expiry, with app.open-conversation targets
# that name neither account nor room, mutes read from the encrypted store, and
# the locked-store notice (NO-11). gh-app-services.c runs it with the account
# store; its window glue follows the attached GhWindow.
if (TARGET groundhog-store AND TARGET groundhog-conversations)
  message(STATUS "Groundhog notifier: enabled")
  add_library(groundhog-notifier STATIC src/app/gh-notifier.c)
  target_include_directories(groundhog-notifier PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
  target_link_libraries(groundhog-notifier PUBLIC groundhog-ui groundhog-conversations
    groundhog-store)
  target_compile_options(groundhog-notifier PRIVATE -Wall -Wextra -Werror)
  if (TARGET groundhog-account-store)
    target_link_libraries(groundhog PRIVATE groundhog-notifier)
    target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_NOTIFIER=1)
  else()
    target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_NOTIFIER=0)
  endif()
  if (BUILD_TESTING AND TARGET groundhog-store-conversations AND
      TARGET groundhog-conversation-ui AND TARGET groundhog-test-schemas)
    # NO-1…NO-8 (and the NO-11 notice): the fake org.gtk.Notifications (H4)
    # on the shared private bus with GLib's GTK backend, a fake GhClock, a
    # real SQLCipher store for the mute (NO-7). --gui: the window glue
    # (open a conversation, the stale toast); it self-skips (77) without a
    # display.
    add_executable(test-groundhog-notifier tests/app/test_notifier.c
      tests/app/fake-gtk-notifications.c)
    target_link_libraries(test-groundhog-notifier PRIVATE groundhog-notifier
      groundhog-store-conversations groundhog-conversation-ui nostrc-test-bus)
    target_compile_options(test-groundhog-notifier PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-notifier)
    add_dependencies(test-groundhog-notifier groundhog-test-schemas)
    add_test(NAME groundhog-notifier COMMAND test-groundhog-notifier)
    set_tests_properties(groundhog-notifier PROPERTIES TIMEOUT 120
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    add_test(NAME groundhog-notifier-gui COMMAND test-groundhog-notifier --gui)
    set_tests_properties(groundhog-notifier-gui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog notifier: disabled (needs the GhStore's clock and the "
                 "conversation model)")
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_NOTIFIER=0)
endif()
# --- end G16 private notifications ---
# --- G08 GhAuthPolicy ---
# Every executable that compiles the NIP-17 send sources (gh-dm-send.c,
# gh-inbox-lookup.c) or the outbox asks GhAuthPolicy for each connection's
# NIP-42 identity, so each links the policy library defined above.
foreach (groundhog_g08_target test-groundhog-dm-send test-groundhog-outbox
         test-groundhog-account-store test-groundhog-expiry)
  if (TARGET ${groundhog_g08_target})
    target_link_libraries(${groundhog_g08_target} PRIVATE groundhog-auth-policy)
  endif()
endforeach()
if (BUILD_TESTING AND TARGET groundhog-auth-policy AND TARGET groundhog-test-schemas)
  # NT-1..NT-4 and R6 across callers: recording scope/publish transports with
  # their NIP-42 halves and a real account controller with the mock signer on
  # the private test bus.
  add_executable(test-groundhog-auth-policy tests/app/test_auth_policy.c)
  target_link_libraries(test-groundhog-auth-policy PRIVATE groundhog-auth-policy nostrc-test-bus)
  target_compile_options(test-groundhog-auth-policy PRIVATE -Wall -Wextra -Werror)
  add_dependencies(test-groundhog-auth-policy groundhog-test-schemas)
  add_test(NAME groundhog-auth-policy COMMAND test-groundhog-auth-policy)
  set_tests_properties(groundhog-auth-policy PROPERTIES TIMEOUT 60
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
endif()
# --- end G08 GhAuthPolicy ---

# --- G10 contact directory ---
# Cached kind-10050 inbox relays and kind-0 display names of accepted
# contacts (privacy charter §4.3, §4.5 S1/S2, PD-12, PT-8), kept in the
# encrypted store's `directory` table and refreshed off the send path. It is
# the GhInboxResolver of the app's sender and outbox (gh-app-outbox.c), so it
# is compiled wherever that file is.
set(GROUNDHOG_DIRECTORY_SOURCES src/app/gh-contact-directory.c src/store/gh-store-directory.c)
if (TARGET groundhog-account-store AND GROUNDHOG_OUTBOX_SOURCES)
  target_sources(groundhog PRIVATE ${GROUNDHOG_DIRECTORY_SOURCES})
  if (TARGET test-groundhog-account-store)
    target_sources(test-groundhog-account-store PRIVATE ${GROUNDHOG_DIRECTORY_SOURCES})
  endif()
endif()
if (BUILD_TESTING AND TARGET groundhog-store-testhooks AND TARGET groundhog-auth-policy AND
    TARGET groundhog-conversations AND TARGET groundhog-test-schemas)
  # NT-11, NT-12, PT-8, S2, forged and stale events and restore across a
  # restart: recording scope transport (with its NIP-42 half), the fake
  # GhClock (H6), real SQLCipher stores (test hooks) and a real account
  # controller with the mock signer on the private test bus.
  add_executable(test-groundhog-contact-directory tests/app/test_contact_directory.c
    ${GROUNDHOG_DIRECTORY_SOURCES} src/app/gh-inbox-resolver.c)
  target_include_directories(test-groundhog-contact-directory PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
  target_link_libraries(test-groundhog-contact-directory PRIVATE groundhog-auth-policy
    groundhog-conversations groundhog-store-testhooks nostrc-test-bus)
  target_compile_options(test-groundhog-contact-directory PRIVATE -Wall -Wextra -Werror)
  groundhog_store_link_sqlcipher_first(test-groundhog-contact-directory)
  add_dependencies(test-groundhog-contact-directory groundhog-test-schemas)
  add_test(NAME groundhog-contact-directory COMMAND test-groundhog-contact-directory)
  set_tests_properties(groundhog-contact-directory PROPERTIES TIMEOUT 60
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
endif()
# --- end G10 contact directory ---

# --- G13 composer and send wiring ---
# The composer (privacy charter §7.4, §7.7, §7.14, §8.2 G13): GhComposer is a
# template child of the content page, so it joins the GTK-only shell library
# the GUI tests share. gh-send-ui.c connects it and the conversation view to
# the account's durable outbox (T-enqueue before any signer call, local echo,
# honest status, retry, delivery details), drafts, the inbox's messages
# waiting for the signer and the disabled reasons; the executable attaches it
# in gh_app_services_attach_window() whenever it has the outbox.
target_sources(groundhog-ui PRIVATE src/ui/gh-composer.c)
if (TARGET groundhog-account-store AND GROUNDHOG_OUTBOX_SOURCES AND
    TARGET groundhog-conversation-ui)
  target_sources(groundhog PRIVATE src/app/gh-send-ui.c)
  if (BUILD_TESTING AND TARGET groundhog-test-schemas)
    set(GROUNDHOG_SEND_TEST_SOURCES src/app/gh-send-ui.c src/app/gh-account-ui.c
      src/app/gh-app-outbox.c tests/store/fake-secret.c ${GROUNDHOG_OUTBOX_SOURCES}
      ${GROUNDHOG_DM_SEND_SOURCES} ${GROUNDHOG_EXPIRY_SOURCES} ${GROUNDHOG_DIRECTORY_SOURCES}
      src/app/gh-nip17-envelope.c)
    # UX-7, OB-1 (UI side), UX-1/UX-2 for the composer, nostrc-qp24.8.1, and
    # every G12 seam wired: the widget alone (keys, bounds, compact, disabled
    # reasons), then the application's wiring on a real account stack (mock
    # signer bus, FakeSecret, SQLCipher stores, recording relay transports):
    # local echo and status, retry, delivery details, drafts across switches
    # and restarts, note to self, locked messages and Unlock, recipient
    # without an inbox. Needs a display, so it self-skips (77). With
    # GROUNDHOG_TEST_SCREENSHOTS=<dir> it renders <dir>/groundhog-g13-*.png.
    add_executable(test-groundhog-composer tests/ui/test_composer.c
      ${GROUNDHOG_SEND_TEST_SOURCES})
    target_include_directories(test-groundhog-composer PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/store ${CMAKE_CURRENT_SOURCE_DIR}/tests/app)
    target_link_libraries(test-groundhog-composer PRIVATE groundhog-account-store
      groundhog-conversation-ui groundhog-auth-policy nostr_nip17_core nostr_nip59_core
      nostrc-test-bus)
    target_compile_options(test-groundhog-composer PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-composer)
    add_dependencies(test-groundhog-composer groundhog-test-schemas)
    add_test(NAME groundhog-composer COMMAND test-groundhog-composer)
    set_tests_properties(groundhog-composer PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    if (TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      # The whole loop over real sockets: two accounts (one mock signer bus,
      # two keys), local storing relays as their kind-10050 inboxes, A's
      # composer -> outbox -> relays -> B's inbox -> B's conversation view,
      # B's reply, one room each, and both restored from their encrypted
      # stores after a restart. Needs a display (77 without one).
      add_executable(test-groundhog-e2e-dm tests/ui/test_e2e_dm.c ${GROUNDHOG_SEND_TEST_SOURCES})
      target_include_directories(test-groundhog-e2e-dm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/store ${CMAKE_CURRENT_SOURCE_DIR}/tests/app)
      target_link_libraries(test-groundhog-e2e-dm PRIVATE groundhog-account-store
        groundhog-conversation-ui groundhog-auth-policy nostr_nip17_core nostr_nip59_core
        nostrc-test-bus PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      target_compile_options(test-groundhog-e2e-dm PRIVATE -Wall -Wextra -Werror)
      groundhog_store_link_sqlcipher_first(test-groundhog-e2e-dm)
      add_dependencies(test-groundhog-e2e-dm groundhog-test-schemas)
      add_test(NAME groundhog-e2e-dm COMMAND test-groundhog-e2e-dm)
      set_tests_properties(groundhog-e2e-dm PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
        ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    endif()
  endif()
endif()
# --- end G13 ---

# --- G24 privacy acceptance harness ---
# The release gate of privacy charter §8.2 G24: PT-1, PT-2, PT-4, PT-8's "no
# lookup for a request", PT-10, AT-5 and NO-1 end to end (§9.2). Two accounts,
# each composed as gh-app-services.c composes a process, talk through the
# real gnostr transports to local store-and-serve NIP-42 relays (H2), with
# the mock signer, FakeSecret (H5) and the fake org.gtk.Notifications (H4)
# on the private bus; the H7 canary scanner then reads every file under the
# test's XDG directories and TMPDIR, both accounts' GSettings, every relay
# frame, the captured logs and every notification payload.
if (BUILD_TESTING AND TARGET groundhog-account-store AND TARGET groundhog-notifier AND
    TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST AND TARGET groundhog-test-schemas AND
    GROUNDHOG_OUTBOX_SOURCES AND GROUNDHOG_EXPIRY_SOURCES)
  add_executable(test-groundhog-privacy-e2e tests/privacy/test_privacy_e2e.c
    tests/privacy/canary-scan.c tests/app/fake-gtk-notifications.c tests/store/fake-secret.c
    src/app/gh-app-outbox.c src/app/gh-nip17-envelope.c ${GROUNDHOG_OUTBOX_SOURCES}
    ${GROUNDHOG_DM_SEND_SOURCES} ${GROUNDHOG_EXPIRY_SOURCES} ${GROUNDHOG_DIRECTORY_SOURCES})
  target_include_directories(test-groundhog-privacy-e2e PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/app ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/store ${CMAKE_CURRENT_SOURCE_DIR}/tests/privacy
    ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
  target_link_libraries(test-groundhog-privacy-e2e PRIVATE groundhog-account-store
    groundhog-notifier groundhog-auth-policy nostr_nip17_core nostr_nip59_core
    nostrc-test-bus PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
  target_compile_options(test-groundhog-privacy-e2e PRIVATE -Wall -Wextra -Werror)
  groundhog_store_link_sqlcipher_first(test-groundhog-privacy-e2e)
  add_dependencies(test-groundhog-privacy-e2e groundhog-test-schemas)
  add_test(NAME groundhog-privacy-e2e COMMAND test-groundhog-privacy-e2e)
  set_tests_properties(groundhog-privacy-e2e PROPERTIES TIMEOUT 300
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals;G_MESSAGES_DEBUG=all")
endif()
# --- end G24 privacy acceptance harness ---
# --- G18 New message, lookup consent, Message Requests ---
# Starting conversations and answering message requests (privacy charter
# §7.9, PD-2, PD-8, PT-8, §8.2 G18):
#  - groundhog-recipient (GTK-free): what was typed into New Message
#    (npub, nprofile, nostr: URI, name@domain, secrets refused), offline;
#  - groundhog-net: GhNetHttp, the one libsoup HTTP client (src/net/ only,
#    check_privacy libsoup-boundary; the network-mode setting, no redirects,
#    cookies, cache or User-Agent) and the NIP-05 lookup on it, which runs only
#    when the user chooses the consent row that names the host;
#  - GhRequestsView (data/ui/gh-requests-view.blp) joins the conversation UI:
#    a selected request's npub, count and first message, Accept / Delete /
#    Block (the store's gh_store_conversations_block), no profile fetch before
#    Accept;
#  - GhNewMessageDialog (gh-new-message-dialog.blp, -row.blp,
#    gh-recipient-chip.blp) behind win.new-message / Ctrl+N.
pkg_check_modules(GROUNDHOG_NET_DEPS QUIET IMPORTED_TARGET libsoup-3.0 json-glib-1.0>=1.6)
if (TARGET nostr AND TARGET nip19)
  add_library(groundhog-recipient STATIC src/ui/gh-recipient.c)
  target_include_directories(groundhog-recipient PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
  target_link_libraries(groundhog-recipient PUBLIC nostr nip19 PkgConfig::GROUNDHOG_LINK_POLICY_DEPS)
  target_compile_options(groundhog-recipient PRIVATE -Wall -Wextra -Werror)
endif()
if (TARGET groundhog-recipient AND TARGET PkgConfig::GROUNDHOG_NET_DEPS)
  message(STATUS "Groundhog NIP-05 lookup: enabled")
  add_library(groundhog-net STATIC src/net/gh-net-http.c src/net/gh-nip05.c)
  target_include_directories(groundhog-net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/net)
  target_link_libraries(groundhog-net PUBLIC groundhog-recipient PkgConfig::GROUNDHOG_NET_DEPS)
  target_compile_options(groundhog-net PRIVATE -Wall -Wextra -Werror)
else()
  message(STATUS "Groundhog NIP-05 lookup: disabled (needs libsoup-3.0 and json-glib-1.0)")
endif()
if (TARGET groundhog-conversation-ui AND TARGET groundhog-recipient)
  target_sources(groundhog-conversation-ui PRIVATE src/ui/gh-requests-view.c)
  target_link_libraries(groundhog-conversation-ui PUBLIC groundhog-recipient)
endif()
if (TARGET groundhog-conversation-ui AND TARGET groundhog-net AND GROUNDHOG_DM_SEND_SOURCES)
  add_library(groundhog-new-message STATIC src/ui/gh-new-message-dialog.c)
  target_link_libraries(groundhog-new-message PUBLIC groundhog-conversation-ui groundhog-net)
  target_compile_options(groundhog-new-message PRIVATE -Wall -Wextra -Werror)
  # gh-inbox-resolver.c (the directory's interface) is one of the
  # executable's GROUNDHOG_DM_SEND_SOURCES.
  target_link_libraries(groundhog PRIVATE groundhog-new-message)
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_NEW_MESSAGE=1)
else()
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_NEW_MESSAGE=0)
endif()
if (BUILD_TESTING AND TARGET groundhog-net AND TARGET groundhog-test-schemas)
  # Offline parsing of every identifier form, NIP-05 URL and document rules,
  # consent (a recording transport sees nothing until a lookup is asked for;
  # Tor mode and .onion refused), and GhNetHttp against a loopback libsoup
  # server: status, redirect not followed, size cap, https-only, no cookies
  # or User-Agent sent.
  add_executable(test-groundhog-new-message tests/ui/test_new_message.c)
  target_link_libraries(test-groundhog-new-message PRIVATE groundhog-net)
  target_compile_options(test-groundhog-new-message PRIVATE -Wall -Wextra -Werror)
  add_dependencies(test-groundhog-new-message groundhog-test-schemas)
  add_test(NAME groundhog-new-message COMMAND test-groundhog-new-message)
  set_tests_properties(groundhog-new-message PROPERTIES TIMEOUT 60
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals,fatal-warnings")
endif()
if (BUILD_TESTING AND TARGET groundhog-new-message AND TARGET groundhog-store-conversations AND
    TARGET groundhog-auth-policy AND TARGET groundhog-test-schemas)
  # The dialog and the requests view on a real account controller (mock
  # signer, private bus), the real contact directory over a recording scope
  # transport (H1) with the fake GhClock (H6), real SQLCipher stores and a
  # recording HTTP transport: PT-8 (no REQ for a request's sender until
  # Accept, then its kind 0), consent rows (no NIP-05 GET and no 10050 REQ
  # before the click), the 10-recipient limit copy, note to self, opening
  # the room, and Accept / Delete / Block persisted across a reopen. The
  # store half runs without a display; --gui needs one and self-skips (77).
  add_executable(test-groundhog-requests tests/ui/test_requests.c
    ${GROUNDHOG_DIRECTORY_SOURCES} src/app/gh-inbox-resolver.c)
  target_include_directories(test-groundhog-requests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/store ${CMAKE_CURRENT_SOURCE_DIR}/tests/app)
  target_link_libraries(test-groundhog-requests PRIVATE groundhog-new-message
    groundhog-store-conversations groundhog-auth-policy nostrc-test-bus)
  target_compile_options(test-groundhog-requests PRIVATE -Wall -Wextra -Werror)
  groundhog_store_link_sqlcipher_first(test-groundhog-requests)
  add_dependencies(test-groundhog-requests groundhog-test-schemas)
  add_test(NAME groundhog-requests COMMAND test-groundhog-requests)
  set_tests_properties(groundhog-requests PROPERTIES TIMEOUT 120
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  add_test(NAME groundhog-requests-gui COMMAND test-groundhog-requests --gui)
  set_tests_properties(groundhog-requests-gui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
endif()
# --- end G18 New message, lookup consent, Message Requests ---

# --- W13c preference gates (keep last) ---
# test_preferences asserts the Preferences rows of this build from
# src/app/gh-features.h, whose features follow the executable's
# GROUNDHOG_HAVE_* definitions: the test gets the same ones, once every block
# above has set them (W13b review B2).
if (TARGET test-groundhog-preferences)
  get_target_property(GROUNDHOG_EXECUTABLE_DEFINITIONS groundhog COMPILE_DEFINITIONS)
  foreach (definition IN LISTS GROUNDHOG_EXECUTABLE_DEFINITIONS)
    if (definition MATCHES "^GROUNDHOG_HAVE_[A-Z_]+=")
      target_compile_definitions(test-groundhog-preferences PRIVATE ${definition})
    endif()
  endforeach()
endif()
# --- end W13c preference gates ---
