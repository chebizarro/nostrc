cmake_minimum_required(VERSION 3.16)
project(groundhog VERSION 0.12.0 LANGUAGES C)

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
  gh-conversation-section gh-timeline-row
  gh-conversation-view gh-message-row gh-message-list-item gh-day-separator
  gh-delivery-details gh-preferences-dialog gh-composer
  gh-requests-view gh-new-message-dialog gh-new-message-row gh-recipient-chip
  gh-add-contact-dialog
  gh-conversation-info-dialog gh-conversation-info-person
  gh-group-join-dialog gh-group-info-dialog gh-group-member-row gh-new-group-dialog
  gh-mls-invitee-row gh-mls-invitee-picker gh-mls-relay-row gh-mls-new-group-page
  gh-mls-member-row gh-mls-group-info-dialog gh-mls-invite-row gh-mls-invites-dialog
  gh-blocked-page gh-blocked-row gh-conversation-menu
  gh-attachment-card gh-attachment-sheet gh-create-poll-dialog)
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
          ${GROUNDHOG_DATA}/icons/512x512/apps/org.nostr.Groundhog.png
          ${GROUNDHOG_DATA}/icons/256x256/apps/org.nostr.Groundhog.png
          ${GROUNDHOG_DATA}/relay-suggestions.json
          ${GROUNDHOG_DATA}/icons/scalable/actions/send-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/send-symbolic-rtl.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/avatar-default-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/action-unavailable-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/alarm-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/audio-x-generic-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/channel-secure-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/computer-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/content-loading-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/dialog-question-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/dialog-warning-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/document-open-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/emblem-ok-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/emblem-synchronizing-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/go-bottom-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/go-down-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/go-next-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/go-previous-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/go-up-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/help-about-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/image-x-generic-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/mail-attachment-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/mail-read-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/mail-send-receive-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/mail-unread-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/network-offline-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/network-transmit-receive-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/network-workgroup-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/open-menu-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/package-x-generic-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/preferences-system-privacy-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/system-lock-screen-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/system-log-out-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/text-x-generic-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/user-bookmarks-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/video-x-generic-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/view-list-bullet-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/view-more-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/view-pin-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/view-refresh-symbolic.svg
          ${GROUNDHOG_DATA}/icons/scalable/actions/x-office-document-symbolic.svg
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

  # nostrc-test-gdk-frame.h tolerance test (nostrc-ykxf): the forgiven warning
  # does not abort; any other warning still does. No display needed.
  add_executable(test-groundhog-gdk-frame-tolerance tests/test_gdk_frame_tolerance.c)
  target_include_directories(test-groundhog-gdk-frame-tolerance PRIVATE ${CMAKE_SOURCE_DIR}/tests/common)
  target_compile_options(test-groundhog-gdk-frame-tolerance PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(test-groundhog-gdk-frame-tolerance PRIVATE PkgConfig::GROUNDHOG_DEPS)
  add_test(NAME groundhog-gdk-frame-tolerance COMMAND test-groundhog-gdk-frame-tolerance)
  set_tests_properties(groundhog-gdk-frame-tolerance PROPERTIES TIMEOUT 10
    ENVIRONMENT "G_DEBUG=fatal-criticals,fatal-warnings")


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
    src/relay/gh-relay-auth.c src/relay/gh-relay-gnostr-write.c
    src/relay/gh-relay-guard.c)
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

# G08: GhAuthPolicy, the single NIP-42 identity decision per (connection
# purpose, URL) (charter §4.3, §4.4), with GhAccountAuth, the one account
# signer per generation it hands out (R6). The inbox, the send pipeline, the
# outbox, the lookups and own-list discovery and publish all link it (see the
# G08 block at the end).
if (TARGET groundhog-account AND TARGET groundhog-relay)
  add_library(groundhog-auth-policy STATIC src/app/gh-auth-policy.c src/app/gh-account-auth.c)
  target_include_directories(groundhog-auth-policy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
  target_link_libraries(groundhog-auth-policy PUBLIC groundhog-account groundhog-relay)
  target_compile_options(groundhog-auth-policy PRIVATE -Wall -Wextra -Werror)
endif()

# Account-bound relay-list discovery: one URL-scoped live REQ per active
# account generation, cancelled on switch; a discovery relay that demands
# AUTH gets GhAuthPolicy's ephemeral identity. Needs the account and relay
# layers.
if (TARGET groundhog-account AND TARGET groundhog-relay)
  add_library(groundhog-account-relays STATIC src/app/gh-account-relays.c)
  target_link_libraries(groundhog-account-relays PUBLIC groundhog-account groundhog-relay
                        groundhog-auth-policy)
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
    src/app/gh-message-status.c src/app/gh-reaction.c src/app/gh-reaction-store.c)
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

    # Reply-target parsing for NIP-29 and MLS messages (nostrc-zjkv).
    add_executable(test-groundhog-message-reply tests/app/test_message_reply.c)
    target_link_libraries(test-groundhog-message-reply PRIVATE groundhog-conversations)
    target_compile_options(test-groundhog-message-reply PRIVATE -Wall -Wextra -Werror)
    add_test(NAME groundhog-message-reply COMMAND test-groundhog-message-reply)
    set_tests_properties(groundhog-message-reply PROPERTIES TIMEOUT 20
      ENVIRONMENT "G_DEBUG=fatal-criticals,fatal-warnings")

  # Tests that include nostrc-test-gdk-frame.h but do not link nostrc-test-bus
  # (which carries tests/common as a PUBLIC include directory).
  foreach(_gdk_frame_test
    test-groundhog-shell
    test-groundhog-account-ui
    test-groundhog-conversation-list
    test-groundhog-conversation-view
    test-groundhog-conversation-menu
    test-groundhog-preferences
  )
    if(TARGET ${_gdk_frame_test})
      target_include_directories(${_gdk_frame_test} PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/common)
    endif()
  endforeach()
  endif()
    add_executable(test-groundhog-reactions tests/app/test_reactions.c)
    target_link_libraries(test-groundhog-reactions PRIVATE groundhog-conversations)
    target_compile_options(test-groundhog-reactions PRIVATE -Wall -Wextra -Werror)
    add_test(NAME groundhog-reactions COMMAND test-groundhog-reactions)
    set_tests_properties(groundhog-reactions PROPERTIES TIMEOUT 20
      ENVIRONMENT "G_DEBUG=fatal-criticals,fatal-warnings")

  # Tests that include nostrc-test-gdk-frame.h but do not link nostrc-test-bus
  # (which carries tests/common as a PUBLIC include directory).
  foreach(_gdk_frame_test
    test-groundhog-shell
    test-groundhog-account-ui
    test-groundhog-conversation-list
    test-groundhog-conversation-view
    test-groundhog-conversation-menu
    test-groundhog-preferences
  )
    if(TARGET ${_gdk_frame_test})
      target_include_directories(${_gdk_frame_test} PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/common)
    endif()
  endforeach()

    # W26 slice B (nostrc-191r): NIP-17 reaction/deletion rumor builders.
    if (TARGET groundhog-account-relays)
      add_executable(test-groundhog-reaction-send tests/app/test_reaction_send.c
        src/app/gh-nip17-envelope.c)
      target_link_libraries(test-groundhog-reaction-send PRIVATE groundhog-account-relays
        nostr_nip17_core nostr_nip59_core)
      target_compile_options(test-groundhog-reaction-send PRIVATE -Wall -Wextra -Werror)
      add_test(NAME groundhog-reaction-send COMMAND test-groundhog-reaction-send)
      set_tests_properties(groundhog-reaction-send PROPERTIES TIMEOUT 20
        ENVIRONMENT "G_DEBUG=fatal-criticals,fatal-warnings")

  # Tests that include nostrc-test-gdk-frame.h but do not link nostrc-test-bus
  # (which carries tests/common as a PUBLIC include directory).
  foreach(_gdk_frame_test
    test-groundhog-shell
    test-groundhog-account-ui
    test-groundhog-conversation-list
    test-groundhog-conversation-view
    test-groundhog-conversation-menu
    test-groundhog-preferences
  )
    if(TARGET ${_gdk_frame_test})
      target_include_directories(${_gdk_frame_test} PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/common)
    endif()
  endforeach()
    endif()
endif()

# GTK-free NIP-17 receive pipeline: the active account's kind-1059 REQs on its
# own 10050 relays (paged backwards with until, NIP-42 AUTH as the account on
# those relays only, through GhAccountAuth), seen-set pre-filter, bounded
# signer unwraps, store admission. It carries the unwrap (gh-nip17-inbox.c);
# the executable links it through groundhog-conversation-ui (G11 block below).
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
# of relay-signed 39000-39003 snapshots, role checks, unsigned event templates
# and the timeline ring for `previous`. No transport, subscription or UI is
# linked here. The top-level build forces ENABLE_NIP29 with BUILD_GROUNDHOG;
# a standalone Groundhog build pulls nips/nip29 in here instead (as the
# identity block does for gnostr-secret), which also registers test_nip29.
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

  # Tests that include nostrc-test-gdk-frame.h but do not link nostrc-test-bus
  # (which carries tests/common as a PUBLIC include directory).
  foreach(_gdk_frame_test
    test-groundhog-shell
    test-groundhog-account-ui
    test-groundhog-conversation-list
    test-groundhog-conversation-view
    test-groundhog-conversation-menu
    test-groundhog-preferences
  )
    if(TARGET ${_gdk_frame_test})
      target_include_directories(${_gdk_frame_test} PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/common)
    endif()
  endforeach()
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

# Review M5: a GROUNDHOG_ENCRYPTED_GROUPS_PREVIEW build (development only,
# see the W21 section below) is never installed: cmake --install, ninja
# install and CPack run this before any Groundhog file.
if (GROUNDHOG_ENCRYPTED_GROUPS_PREVIEW)
  install(CODE "message(FATAL_ERROR \"Groundhog was built with GROUNDHOG_ENCRYPTED_GROUPS_PREVIEW=ON, a development-only build: it is not installed.\")")
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
install(FILES ${GROUNDHOG_DATA}/icons/512x512/apps/org.nostr.Groundhog.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps)
install(FILES ${GROUNDHOG_DATA}/icons/256x256/apps/org.nostr.Groundhog.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)

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
  set(STORE_KEY_SOURCES src/store/gh-store-key.c)
  if(APPLE)
    list(APPEND STORE_KEY_SOURCES src/store/gh-store-key-keychain.c)
  endif()
  add_library(groundhog-store-key STATIC ${STORE_KEY_SOURCES})
  target_include_directories(groundhog-store-key PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
  target_link_libraries(groundhog-store-key PUBLIC PkgConfig::GROUNDHOG_STORE_KEY_DEPS)
  if(APPLE)
    target_link_libraries(groundhog-store-key PUBLIC "-framework Security" "-framework CoreFoundation")
  endif()
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

  # Tests that include nostrc-test-gdk-frame.h but do not link nostrc-test-bus
  # (which carries tests/common as a PUBLIC include directory).
  foreach(_gdk_frame_test
    test-groundhog-shell
    test-groundhog-account-ui
    test-groundhog-conversation-list
    test-groundhog-conversation-view
    test-groundhog-conversation-menu
    test-groundhog-preferences
  )
    if(TARGET ${_gdk_frame_test})
      target_include_directories(${_gdk_frame_test} PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/common)
    endif()
  endforeach()
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

    # KC-MAC: GhStoreKeyKeychain on a temporary keychain (macOS only).
    # CRITICAL: never touches the user's login keychain.
    if(APPLE)
      add_executable(test-groundhog-store-key-keychain
        tests/store/test_store_key_keychain.c)
      target_link_libraries(test-groundhog-store-key-keychain PRIVATE groundhog-store-key)
      target_compile_options(test-groundhog-store-key-keychain PRIVATE -Wall -Wextra -Werror)
      add_test(NAME groundhog-store-key-keychain COMMAND test-groundhog-store-key-keychain)
      set_tests_properties(groundhog-store-key-keychain PROPERTIES TIMEOUT 30
        ENVIRONMENT "G_DEBUG=fatal-criticals,fatal-warnings")
    endif()
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

  # gh_store_thread_release_crypto() frees the OpenSSL thread state SQLCipher
  # leaves (nostrc-vpha): the libcrypto SQLCipher itself links.
  if (NOT TARGET OpenSSL::Crypto)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  endif()
  function(groundhog_store_library name)
    add_library(${name} STATIC ${GROUNDHOG_STORE_SOURCES})
    target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
    # sqlite3_key_v2 is declared by SQLCipher's sqlite3.h only with this set.
    target_compile_definitions(${name} PUBLIC SQLITE_HAS_CODEC=1)
    target_link_libraries(${name} PUBLIC PkgConfig::GROUNDHOG_SQLCIPHER
      PkgConfig::GROUNDHOG_STORE_DEPS OpenSSL::Crypto ${CMAKE_DL_LIBS})
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
# share one SQLCipher transaction (T-mls), and every libmarmot operation is
# one transaction (its storage hooks). gh-mls-commits is the durable Commit
# lifecycle over it (qp24.7): stage, relay answers, merge/clear, restart.
# Needs the GhStore (G02) and the in-tree libmarmot target; skipped cleanly
# without either. Nothing in the application uses it yet (qp24.13 will link
# it), so the executable's link set is unchanged.
if (TARGET groundhog-store AND TARGET marmot)
  message(STATUS "Groundhog store marmot: enabled")
  add_library(groundhog-store-marmot STATIC src/store/gh-store-marmot.c
    src/store/gh-mls-commits.c)
  target_include_directories(groundhog-store-marmot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
  target_link_libraries(groundhog-store-marmot PUBLIC groundhog-store marmot)
  target_compile_options(groundhog-store-marmot PRIVATE -Wall -Wextra -Werror)
  if (BUILD_TESTING)
    # The adapter compiled against the test-hooks store (H8 crash cut points):
    # libmarmot's storage contract cases, snapshots, T-mls under the crash
    # harness, an end-to-end libmarmot group across store reopens, and the
    # Commit lifecycle (every write of every operation cut, restart,
    # supersession, three members, disk full, canary scan). libmarmot
    # links libsqlite3, so SQLCipher goes first here as in the application
    # (ST-4); every store open re-checks that at runtime.
    add_executable(test-groundhog-store-marmot tests/store/test_store_marmot.c
      tests/store/crash-harness.c tests/privacy/canary-scan.c src/store/gh-store-marmot.c
      src/store/gh-mls-commits.c)
    target_include_directories(test-groundhog-store-marmot PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/store ${CMAKE_CURRENT_SOURCE_DIR}/tests/privacy)
    target_link_libraries(test-groundhog-store-marmot PRIVATE groundhog-store-testhooks marmot)
    target_compile_options(test-groundhog-store-marmot PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-store-marmot)
    add_test(NAME groundhog-store-marmot COMMAND test-groundhog-store-marmot)
    set_tests_properties(groundhog-store-marmot PROPERTIES TIMEOUT 300
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
  add_library(groundhog-store-conversations STATIC src/store/gh-store-conversations.c
    src/store/gh-store-reactions.c)
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

  # Tests that include nostrc-test-gdk-frame.h but do not link nostrc-test-bus
  # (which carries tests/common as a PUBLIC include directory).
  foreach(_gdk_frame_test
    test-groundhog-shell
    test-groundhog-account-ui
    test-groundhog-conversation-list
    test-groundhog-conversation-view
    test-groundhog-conversation-menu
    test-groundhog-preferences
  )
    if(TARGET ${_gdk_frame_test})
      target_include_directories(${_gdk_frame_test} PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/common)
    endif()
  endforeach()
endif()
if (TARGET groundhog-conversation-ui)
  target_sources(groundhog-conversation-ui PRIVATE src/ui/gh-conversation-view.c
    src/ui/gh-message-row.c src/ui/gh-delivery-indicator.c
    src/ui/gh-reaction-bar.c src/ui/gh-reaction-picker.c)
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

# The About dialog (app.about): the app icon from the resource bundle, the
# version, the licence and the project links; no network, no settings.
add_library(groundhog-about STATIC src/ui/gh-about-dialog.c)
target_include_directories(groundhog-about PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
target_link_libraries(groundhog-about PUBLIC PkgConfig::GROUNDHOG_DEPS)
target_compile_definitions(groundhog-about PRIVATE GROUNDHOG_VERSION="${PROJECT_VERSION}")
target_compile_options(groundhog-about PRIVATE -Wall -Wextra -Werror)
target_link_libraries(groundhog PRIVATE groundhog-about)
if (BUILD_TESTING)
  # Default mode (no display): the bundled app icon exists at the hicolor
  # resource layout and decodes; --gui: the dialog shows the icon, version,
  # licence and links, and self-skips (77) without a display.
  add_executable(test-groundhog-about tests/ui/test_about.c ${GROUNDHOG_RESOURCE_C})
  target_link_libraries(test-groundhog-about PRIVATE groundhog-about)
  target_compile_definitions(test-groundhog-about PRIVATE GROUNDHOG_VERSION="${PROJECT_VERSION}")
  target_compile_options(test-groundhog-about PRIVATE -Wall -Wextra -Werror)
  add_test(NAME groundhog-about COMMAND test-groundhog-about)
  add_test(NAME groundhog-about-gui COMMAND test-groundhog-about --gui)
  set_tests_properties(groundhog-about groundhog-about-gui PROPERTIES TIMEOUT 60
    ENVIRONMENT "G_DEBUG=fatal-criticals,fatal-warnings" SKIP_RETURN_CODE 77)
endif()
if (BUILD_TESTING)
  # Icon resolution: every symbolic icon the app references resolves from the
  # bundled GResource, so non-GNOME desktops render icons (nostrc-9juh).
  add_executable(test-groundhog-icons tests/ui/test_icons.c ${GROUNDHOG_RESOURCE_C})
  target_link_libraries(test-groundhog-icons PRIVATE PkgConfig::GROUNDHOG_GIO)
  target_compile_options(test-groundhog-icons PRIVATE -Wall -Wextra -Werror)
  add_test(NAME groundhog-icons COMMAND test-groundhog-icons)
  set_tests_properties(groundhog-icons PROPERTIES TIMEOUT 30
    ENVIRONMENT "G_DEBUG=fatal-criticals,fatal-warnings")
endif()
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
  add_library(groundhog-inbox-setup STATIC src/app/gh-inbox-setup.c src/app/gh-relay-list-setup.c)
  target_include_directories(groundhog-inbox-setup PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
  target_link_libraries(groundhog-inbox-setup PUBLIC groundhog-dm-inbox PkgConfig::GROUNDHOG_JSON)
  target_compile_options(groundhog-inbox-setup PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(groundhog-inbox-setup PRIVATE
    "GROUNDHOG_RELAY_SUGGESTIONS=\"${GROUNDHOG_DATA}/relay-suggestions.json\"")
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
  add_library(groundhog-net STATIC src/net/gh-net-http.c src/net/gh-net-tls.c src/net/gh-nip05.c)
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
  add_library(groundhog-new-message STATIC src/ui/gh-new-message-dialog.c src/ui/gh-add-contact-dialog.c)
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
# --- G19 conversation info ---
# Conversation Info for private conversations (privacy charter §7.4, §2.2,
# §3.7, §3.8, §5.2; §8.2 G19): GhConversationInfoDialog (people, Verify Key
# with the safety code, the privacy summary, mute, the disappearing timer,
# Block and Forget on This Device), opened by the content page's info button
# (win.conversation-info). gh-privacy-summary.c is the GTK-free strings table
# behind it and behind the conversation header's subtitle;
# gh-conversation-actions.c changes the store and withdraws notifications for
# mute, block and forget; gh-store-contacts.c keeps local verification marks
# (schema v3).
add_library(groundhog-privacy-summary STATIC src/ui/gh-privacy-summary.c)
target_include_directories(groundhog-privacy-summary PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
target_link_libraries(groundhog-privacy-summary PUBLIC PkgConfig::GROUNDHOG_LINK_POLICY_DEPS)
target_compile_options(groundhog-privacy-summary PRIVATE -Wall -Wextra -Werror)
if (TARGET groundhog-conversation-ui)
  target_link_libraries(groundhog-conversation-ui PUBLIC groundhog-privacy-summary)
endif()
if (BUILD_TESTING)
  # Snapshot per backend (NIP-17, NIP-29, MLS) against tests/ui/snapshots,
  # the honesty rules of the copy, and the safety code. No display needed.
  add_executable(test-groundhog-privacy-summary tests/ui/test_privacy_summary.c)
  target_link_libraries(test-groundhog-privacy-summary PRIVATE groundhog-privacy-summary)
  target_compile_definitions(test-groundhog-privacy-summary PRIVATE
    GROUNDHOG_TEST_SNAPSHOTS="${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/snapshots")
  target_compile_options(test-groundhog-privacy-summary PRIVATE -Wall -Wextra -Werror)
  add_test(NAME groundhog-privacy-summary COMMAND test-groundhog-privacy-summary)
  set_tests_properties(groundhog-privacy-summary PROPERTIES TIMEOUT 20
    ENVIRONMENT "G_DEBUG=fatal-criticals,fatal-warnings")

  # Tests that include nostrc-test-gdk-frame.h but do not link nostrc-test-bus
  # (which carries tests/common as a PUBLIC include directory).
  foreach(_gdk_frame_test
    test-groundhog-shell
    test-groundhog-account-ui
    test-groundhog-conversation-list
    test-groundhog-conversation-view
    test-groundhog-conversation-menu
    test-groundhog-preferences
  )
    if(TARGET ${_gdk_frame_test})
      target_include_directories(${_gdk_frame_test} PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/common)
    endif()
  endforeach()
endif()
set(GROUNDHOG_CONVERSATION_INFO_SOURCES src/ui/gh-conversation-info-dialog.c
  src/app/gh-conversation-actions.c src/store/gh-store-contacts.c)
if (TARGET groundhog-account-store AND TARGET groundhog-notifier AND GROUNDHOG_EXPIRY_SOURCES AND
    GROUNDHOG_OUTBOX_SOURCES)
  message(STATUS "Groundhog conversation info: enabled")
  target_sources(groundhog PRIVATE ${GROUNDHOG_CONVERSATION_INFO_SOURCES})
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_CONVERSATION_INFO=1)
  if (BUILD_TESTING AND TARGET groundhog-test-schemas)
    # Default mode: mute, block and forget through GhConversationActions with
    # the real notifier on the shared private bus and the fake
    # org.gtk.Notifications (H4), block and verification marks over real
    # SQLCipher stores across restarts. --gui: the dialog itself (ST-9 via
    # the UI, mute in the database surviving a restart (NO-7), block with
    # Undo, Verify Key, the timer, the header button); it self-skips (77)
    # without a display. With GROUNDHOG_TEST_SCREENSHOTS=<dir> the GUI run
    # also renders the dialog to PNGs.
    add_executable(test-groundhog-conversation-info tests/ui/test_conversation_info.c
      tests/app/fake-gtk-notifications.c ${GROUNDHOG_CONVERSATION_INFO_SOURCES}
      ${GROUNDHOG_EXPIRY_SOURCES})
    target_include_directories(test-groundhog-conversation-info PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/app ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
    target_link_libraries(test-groundhog-conversation-info PRIVATE groundhog-notifier
      groundhog-store-conversations groundhog-conversation-ui nostrc-test-bus)
    target_compile_options(test-groundhog-conversation-info PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-conversation-info)
    add_dependencies(test-groundhog-conversation-info groundhog-test-schemas)
    add_test(NAME groundhog-conversation-info COMMAND test-groundhog-conversation-info)
    set_tests_properties(groundhog-conversation-info PROPERTIES TIMEOUT 120
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    add_test(NAME groundhog-conversation-info-gui COMMAND test-groundhog-conversation-info --gui)
    set_tests_properties(groundhog-conversation-info-gui PROPERTIES SKIP_RETURN_CODE 77
      TIMEOUT 120
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog conversation info: disabled (needs the account store, the "
                 "notifier, expiry and the outbox)")
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_CONVERSATION_INFO=0)
endif()
# --- end G19 conversation info ---

# --- G20a NIP-29 service ---
# The account's NIP-29 relay groups (privacy charter §8.2 G20a, plan W5):
# GhNip29Service (per relay one URL-scoped live REQ, EOSE-aware backfill,
# relay-key-verified 39000-39003, honest join states, timeline ring for
# `previous`), the durable NIP-29 engine of the outbox (GhNip29Outbox:
# T-enqueue before the signer, T-seal, publish to exactly the group relay
# with account AUTH through GhAuthPolicy), group rooms in the conversation
# model through the encrypted store's NIP-29 delegate (gh-store-nip29.c,
# nip29_groups), and the NIP-11 relay key fetch (src/net/gh-nip11.c on G18's
# GhNetHttp in groundhog-net: the network-mode setting, https only, no
# redirects, cookies or User-Agent; only for groups the user joined). Needs
# the NIP-29 core, the GhStore, GhAuthPolicy, the conversation model and
# groundhog-net. The executable runs one service per open store
# (gh-app-outbox.c); no UI uses it yet (G20b).
if (TARGET groundhog-nip29 AND TARGET groundhog-store AND TARGET groundhog-auth-policy AND
    TARGET groundhog-conversations AND TARGET groundhog-net)
  message(STATUS "Groundhog NIP-29 service: enabled")
  target_sources(groundhog-net PRIVATE src/net/gh-nip11.c)
  set(GROUNDHOG_NIP29_SERVICE_SOURCES src/nip29/gh-nip29-service.c src/nip29/gh-nip29-outbox.c
    src/store/gh-store-nip29.c)
  add_library(groundhog-nip29-service STATIC ${GROUNDHOG_NIP29_SERVICE_SOURCES})
  target_include_directories(groundhog-nip29-service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nip29 ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
  target_link_libraries(groundhog-nip29-service PUBLIC groundhog-nip29 groundhog-net
    groundhog-store groundhog-auth-policy groundhog-conversations)
  target_compile_options(groundhog-nip29-service PRIVATE -Wall -Wextra -Werror)
  if (TARGET groundhog-account-store AND GROUNDHOG_OUTBOX_SOURCES)
    target_link_libraries(groundhog PRIVATE groundhog-nip29-service)
    target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_NIP29=1)
  endif()
  if (BUILD_TESTING AND TARGET groundhog-test-schemas AND
      TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
    # The acceptance tests of qp24.12 against a local NIP-29 relay
    # (tests/nip29/nip29-relay.h: libsoup, relay-signed 39000-39003, NIP-11,
    # membership-enforced writes, 9021 pending/accept/deny, admin rules,
    # NIP-42): two relays with one group id, admin and non-admin, pending then
    # admission, final denial and closed groups, duplicate, partial member
    # lists, AUTH/CLOSED, restart restore and the outbox resume, over real
    # SQLCipher stores (test hooks), the real gnostr relay transports and a
    # real account controller with the mock signer on the private test bus.
    add_executable(test-groundhog-nip29-service tests/nip29/test_nip29_service.c
      ${GROUNDHOG_NIP29_SERVICE_SOURCES})
    target_include_directories(test-groundhog-nip29-service PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/nip29 ${CMAKE_CURRENT_SOURCE_DIR}/src/store
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/app)
    target_link_libraries(test-groundhog-nip29-service PRIVATE groundhog-nip29 groundhog-net
      groundhog-store-testhooks groundhog-auth-policy groundhog-conversations
      PkgConfig::GROUNDHOG_RELAY_WIRE_TEST nostrc-test-bus)
    target_compile_options(test-groundhog-nip29-service PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-nip29-service)
    add_dependencies(test-groundhog-nip29-service groundhog-test-schemas)
    add_test(NAME groundhog-nip29-service COMMAND test-groundhog-nip29-service)
    set_tests_properties(groundhog-nip29-service PROPERTIES TIMEOUT 180
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog NIP-29 service: disabled (needs the NIP-29 core, the GhStore, "
                 "GhAuthPolicy, the conversation model and groundhog-net)")
endif()
# --- end G20a NIP-29 service ---

# --- W20 MLS service (nostrc-qp24.13 part 1) ---
# GhMlsService: one account's Marmot (MLS) encrypted groups over libmarmot on
# GhStoreMarmot (all MLS state in the encrypted store): the account's
# KeyPackage (kind 30443) published to its own relays and rotated, consented
# KeyPackage lookups (gh-mls-key-packages.c: discovery relays, then the
# person's 10002 write relays; never for a message request), group creation
# with one published-then-merged Add Commit (gh-mls-commits), Welcomes
# gift-wrapped to each invitee's 10050 only and received through the DM
# inbox's Welcome sink, kind-445 REQs per group on its relays (ephemeral AUTH),
# sends with the ratchet step stored before publish, add/remove/leave and
# metadata, and MLS rooms in the conversation model (gh-store-mls.c). The
# executable wires it (gh-app-outbox.c) only when GH_FEATURE_ENCRYPTED_GROUPS
# is 1 (part 2, the UI, flips it); until then it is built and tested here.
if (TARGET groundhog-store-marmot AND TARGET groundhog-dm-inbox AND
    TARGET groundhog-auth-policy AND TARGET groundhog-conversations AND
    TARGET groundhog-account-relays AND TARGET nostr_nip17_core AND TARGET nostr_nip59_core
    AND GROUNDHOG_DM_SEND_SOURCES)
  message(STATUS "Groundhog MLS service: enabled")
  set(GROUNDHOG_MLS_SERVICE_SOURCES src/mls/gh-mls-service.c src/mls/gh-mls-key-packages.c
    src/mls/gh-mls-imeta.c src/mls/gh-mls-poll.c src/store/gh-store-mls.c src/store/gh-store-mls-identity.c)
  add_library(groundhog-mls-service STATIC ${GROUNDHOG_MLS_SERVICE_SOURCES})
  target_include_directories(groundhog-mls-service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mls ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
  target_link_libraries(groundhog-mls-service PUBLIC groundhog-store-marmot groundhog-dm-inbox
    groundhog-auth-policy groundhog-conversations groundhog-account-relays
    nostr_nip17_core nostr_nip59_core)
  target_compile_options(groundhog-mls-service PRIVATE -Wall -Wextra -Werror)
  # The adopted-profile KeyPackage producer (nostrc-0bdg) follows libmarmot's
  # own build gate: one switch, MARMOT_ADOPTED_KEY_PACKAGE_PRODUCER,
  # ON by default since nostrc-lf62, turns both on. The tests that compile
  # the service sources themselves build the same (GROUNDHOG_MLS_SERVICE_DEFS),
  # unless they pin a producer on purpose (groundhog-mls-kp-lifecycle).
  set(GROUNDHOG_MLS_SERVICE_DEFS)
  if (MARMOT_ADOPTED_KEY_PACKAGE_PRODUCER)
    set(GROUNDHOG_MLS_SERVICE_DEFS GH_MLS_ADOPTED_KEY_PACKAGES=1)
    target_compile_definitions(groundhog-mls-service PUBLIC ${GROUNDHOG_MLS_SERVICE_DEFS})
  endif()
  if (TARGET groundhog-account-store AND GROUNDHOG_OUTBOX_SOURCES)
    target_link_libraries(groundhog PRIVATE groundhog-mls-service)
    target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_MLS=1)
    # Preferences shows "Let people using older Marmot apps invite me" only
    # when the older format is a choice (gh-features.h, review N4).
    if (MARMOT_ADOPTED_KEY_PACKAGE_PRODUCER)
      target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_ADOPTED_KEY_PACKAGES=1)
    endif()
  endif()
  if (BUILD_TESTING AND TARGET groundhog-test-schemas AND
      TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
    # The qp24.13 part-1 wire tests: three accounts (real account controllers,
    # the mock signer on the private test bus, real SQLCipher stores with test
    # hooks) against local store-and-serve relays (wire-relay.h) through the
    # real gnostr transports: KeyPackages, create, invite, join via Welcome,
    # messages both ways, add, remove (the removed member reads nothing
    # after), leave, metadata, restart recovery mid-Commit (the pending
    # Commit is republished byte for byte), PT-8 consent, Welcome only as a
    # gift wrap to the invitee's inbox, and a canary scan of the store files.
    add_executable(test-groundhog-mls-service tests/mls/test_mls_service.c
      tests/privacy/canary-scan.c ${GROUNDHOG_MLS_SERVICE_SOURCES}
      src/store/gh-store-marmot.c src/store/gh-mls-commits.c
      src/store/gh-store-conversations.c src/app/gh-nip17-envelope.c
      ${GROUNDHOG_DM_SEND_SOURCES})
    # libmarmot's own src/ too: tests/mls/mls-forge.h builds what only a
    # modified client sends (a forged account proof, nostrc-6ukh).
    target_include_directories(test-groundhog-mls-service PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/mls ${CMAKE_CURRENT_SOURCE_DIR}/src/store
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/app ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/privacy
      ${CMAKE_CURRENT_SOURCE_DIR}/../../libmarmot/src)
    target_link_libraries(test-groundhog-mls-service PRIVATE groundhog-store-testhooks marmot
      groundhog-dm-inbox groundhog-auth-policy groundhog-conversations
      groundhog-account-relays nostr_nip17_core nostr_nip59_core nostrc-test-bus
      PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
    target_compile_options(test-groundhog-mls-service PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(test-groundhog-mls-service PRIVATE GH_MLS_TEST_HOOKS=1
      ${GROUNDHOG_MLS_SERVICE_DEFS})
    groundhog_store_link_sqlcipher_first(test-groundhog-mls-service)
    add_dependencies(test-groundhog-mls-service groundhog-test-schemas)
    add_test(NAME groundhog-mls-service COMMAND test-groundhog-mls-service)
    set_tests_properties(groundhog-mls-service PROPERTIES TIMEOUT 300
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    # NIP-88 poll model unit tests (nostrc-efa8): parse/validate, create,
    # vote, change vote, closed poll, bounds, malformed, privacy.
    add_executable(test-groundhog-mls-poll tests/mls/test_mls_poll.c
      src/mls/gh-mls-poll.c)
    target_include_directories(test-groundhog-mls-poll PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/mls)
    target_link_libraries(test-groundhog-mls-poll PRIVATE nostr PkgConfig::GROUNDHOG_GIO)
    target_compile_options(test-groundhog-mls-poll PRIVATE -Wall -Wextra -Werror)
    add_test(NAME groundhog-mls-poll COMMAND test-groundhog-mls-poll)
    set_tests_properties(groundhog-mls-poll PROPERTIES TIMEOUT 30
      ENVIRONMENT "G_DEBUG=fatal-criticals")
    # KeyPackage transport lifecycle (nostrc-0bdg): kind-10002 write relays
    # only (a read/write split), lookup privacy, the d slot kept, ACK-tied
    # retirement of old init keys, the delayed-Welcome trade-off and failed
    # Welcomes, and the formats' independent lifecycles (nostrc-lf62). Built
    # twice: with the MDK 0.8 producer only (libmarmot's gate off), and as
    # shipped, both formats (libmarmot's ungated internal producer standing
    # in when its gate is off, hence its src/).
    foreach(groundhog_kp_variant IN ITEMS legacy adopted)
      if (groundhog_kp_variant STREQUAL "legacy")
        set(groundhog_kp_name groundhog-mls-kp-lifecycle)
      else()
        set(groundhog_kp_name groundhog-mls-kp-lifecycle-adopted)
      endif()
      add_executable(test-${groundhog_kp_name} tests/mls/test_mls_kp_lifecycle.c
        ${GROUNDHOG_MLS_SERVICE_SOURCES}
        src/store/gh-store-marmot.c src/store/gh-mls-commits.c
        src/store/gh-store-conversations.c src/app/gh-nip17-envelope.c
        ${GROUNDHOG_DM_SEND_SOURCES})
      target_include_directories(test-${groundhog_kp_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mls ${CMAKE_CURRENT_SOURCE_DIR}/src/store
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/app ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mls)
      target_link_libraries(test-${groundhog_kp_name} PRIVATE groundhog-store-testhooks marmot
        groundhog-dm-inbox groundhog-auth-policy groundhog-conversations
        groundhog-account-relays nostr_nip17_core nostr_nip59_core nostrc-test-bus
        PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      target_compile_options(test-${groundhog_kp_name} PRIVATE -Wall -Wextra -Werror)
      target_compile_definitions(test-${groundhog_kp_name} PRIVATE GH_MLS_TEST_HOOKS=1)
      if (groundhog_kp_variant STREQUAL "adopted")
        target_compile_definitions(test-${groundhog_kp_name} PRIVATE
          GH_MLS_ADOPTED_KEY_PACKAGES=1)
        if (MARMOT_ADOPTED_KEY_PACKAGE_PRODUCER)
          # As shipped: libmarmot's public producer, no stand-in.
          target_compile_definitions(test-${groundhog_kp_name} PRIVATE
            GH_TEST_LIBMARMOT_ADOPTED_PRODUCER=1)
        endif()
        target_include_directories(test-${groundhog_kp_name} PRIVATE
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libmarmot/src)
      endif()
      # Two fresh accounts set their lists up as onboarding does (H2).
      if (TARGET groundhog-inbox-setup)
        target_link_libraries(test-${groundhog_kp_name} PRIVATE groundhog-inbox-setup)
        target_compile_definitions(test-${groundhog_kp_name} PRIVATE GH_TEST_HAVE_INBOX_SETUP=1)
      endif()
      groundhog_store_link_sqlcipher_first(test-${groundhog_kp_name})
      add_dependencies(test-${groundhog_kp_name} groundhog-test-schemas)
      add_test(NAME ${groundhog_kp_name} COMMAND test-${groundhog_kp_name})
      set_tests_properties(${groundhog_kp_name} PROPERTIES TIMEOUT 300
        ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    endforeach()
    # G24 privacy harness, encrypted groups (charter §2.2, §4.3, §4.4, PD-8,
    # H7): no KeyPackage lookup for a message request or a stranger; Welcomes
    # only as gift wraps to the invitee's own 10050; kind 445 only under
    # fresh keys and ephemeral AUTH; no canary outside the encrypted store or
    # in the logs (G_MESSAGES_DEBUG=all).
    add_executable(test-groundhog-privacy-mls tests/privacy/test_privacy_mls.c
      tests/privacy/canary-scan.c ${GROUNDHOG_MLS_SERVICE_SOURCES}
      src/store/gh-store-marmot.c src/store/gh-mls-commits.c
      src/store/gh-store-conversations.c src/app/gh-nip17-envelope.c
      ${GROUNDHOG_DM_SEND_SOURCES})
    target_include_directories(test-groundhog-privacy-mls PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/mls ${CMAKE_CURRENT_SOURCE_DIR}/src/store
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/app ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/privacy ${CMAKE_CURRENT_SOURCE_DIR}/tests/mls)
    target_link_libraries(test-groundhog-privacy-mls PRIVATE groundhog-store-testhooks marmot
      groundhog-dm-inbox groundhog-auth-policy groundhog-conversations
      groundhog-account-relays nostr_nip17_core nostr_nip59_core nostrc-test-bus
      PkgConfig::GROUNDHOG_RELAY_WIRE_TEST OpenSSL::Crypto)
    target_compile_options(test-groundhog-privacy-mls PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(test-groundhog-privacy-mls PRIVATE ${GROUNDHOG_MLS_SERVICE_DEFS})
    groundhog_store_link_sqlcipher_first(test-groundhog-privacy-mls)
    add_dependencies(test-groundhog-privacy-mls groundhog-test-schemas)
    add_test(NAME groundhog-privacy-mls COMMAND test-groundhog-privacy-mls)
    set_tests_properties(groundhog-privacy-mls PROPERTIES TIMEOUT 300
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals;G_MESSAGES_DEBUG=all")
  endif()
  # Third-party interop, opt-in (nostrc-7gx7, nostrc-77pa; W23): GhMlsService
  # (the mls-world.h accounts on local relays) with MDK v0.8.0 peers, the
  # JSON-lines driver of tests/interop/mdk/driver, run as a Docker container
  # so no host Rust toolchain is needed. groundhog-mdk-interop-image builds
  # the image (a CTest fixture); MDK_INTEROP_DRIVER replaces it with another
  # command line, e.g. a natively built driver. Default-mode cases admit MDK
  # 0.8 members (nostrc-6ukh); the strict cases set the
  # only-join-verified-mls-groups preference. Evidence:
  # docs/analysis/marmot-mdk-interop-2026-09-30.md.
  option(BUILD_MDK_INTEROP "Build and run the Groundhog/MDK 0.8 interop test (needs Docker)"
         OFF)
  set(MDK_INTEROP_DRIVER "" CACHE STRING
      "MDK interop driver command line; empty: the Docker image the test builds")
  if (BUILD_MDK_INTEROP)
    if (NOT (BUILD_TESTING AND TARGET groundhog-test-schemas AND
             TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST AND TARGET PkgConfig::GROUNDHOG_JSON))
      message(FATAL_ERROR "BUILD_MDK_INTEROP needs BUILD_TESTING, libsoup-3.0 and json-glib-1.0")
    endif()
    add_executable(test-groundhog-mdk-interop tests/mls/test_mdk_interop.c
      ${GROUNDHOG_MLS_SERVICE_SOURCES} src/mls/gh-mls-invitee.c src/ui/gh-mls-copy.c
      src/store/gh-store-marmot.c src/store/gh-mls-commits.c
      src/store/gh-store-conversations.c src/app/gh-nip17-envelope.c
      ${GROUNDHOG_DM_SEND_SOURCES})
    target_include_directories(test-groundhog-mdk-interop PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/mls ${CMAKE_CURRENT_SOURCE_DIR}/src/store
      ${CMAKE_CURRENT_SOURCE_DIR}/src/ui ${CMAKE_CURRENT_SOURCE_DIR}/tests/app
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay ${CMAKE_CURRENT_SOURCE_DIR}/tests/mls)
    target_link_libraries(test-groundhog-mdk-interop PRIVATE groundhog-store-testhooks marmot
      groundhog-dm-inbox groundhog-auth-policy groundhog-conversations
      groundhog-account-relays nostr_nip17_core nostr_nip59_core nostrc-test-bus
      PkgConfig::GROUNDHOG_RELAY_WIRE_TEST PkgConfig::GROUNDHOG_JSON)
    # Case 3d forces a MARMOT_ERR_EVENT_RATE refusal (nostrc-2um6).
    target_compile_definitions(test-groundhog-mdk-interop PRIVATE GH_MLS_TEST_HOOKS=1
      ${GROUNDHOG_MLS_SERVICE_DEFS})
    target_compile_options(test-groundhog-mdk-interop PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-mdk-interop)
    add_dependencies(test-groundhog-mdk-interop groundhog-test-schemas)
    if (MDK_INTEROP_DRIVER)
      set(groundhog_mdk_driver "${MDK_INTEROP_DRIVER}")
      set(groundhog_mdk_fixture)
    else()
      # The docker CLI, never an app bundle: on macOS find_program() prefers
      # /Applications/Docker.app, whose executable is the backend daemon.
      set(groundhog_saved_appbundle "${CMAKE_FIND_APPBUNDLE}")
      set(CMAKE_FIND_APPBUNDLE NEVER)
      find_program(GROUNDHOG_DOCKER docker REQUIRED)
      set(CMAKE_FIND_APPBUNDLE "${groundhog_saved_appbundle}")
      set(groundhog_mdk_image nostrc-mdk-interop:0.8.0)
      add_test(NAME groundhog-mdk-interop-image
        COMMAND ${GROUNDHOG_DOCKER} build -t ${groundhog_mdk_image}
                ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/interop/mdk/driver)
      set_tests_properties(groundhog-mdk-interop-image PROPERTIES TIMEOUT 3600
        FIXTURES_SETUP groundhog-mdk-image)
      set(groundhog_mdk_fixture FIXTURES_REQUIRED groundhog-mdk-image)
      # The driver dials the test's relays on the host's 127.0.0.1: the
      # host's network on Linux, Docker Desktop's host.docker.internal on
      # macOS (the URLs, and the NIP-42 relay tag, stay 127.0.0.1).
      # MDK_DRIVER_LOG (MDK's tracing) and MDK_DRIVER_DEADLINE_S pass
      # through from the environment when set.
      set(groundhog_mdk_run "${GROUNDHOG_DOCKER} run --rm -i -e MDK_DRIVER_LOG -e MDK_DRIVER_DEADLINE_S")
      if (APPLE)
        set(groundhog_mdk_driver "${groundhog_mdk_run} -e MDK_DRIVER_DIAL_HOST=host.docker.internal ${groundhog_mdk_image}")
      else()
        set(groundhog_mdk_driver "${groundhog_mdk_run} --network host ${groundhog_mdk_image}")
      endif()
    endif()
    add_test(NAME groundhog-mdk-interop COMMAND test-groundhog-mdk-interop)
    set_tests_properties(groundhog-mdk-interop PROPERTIES TIMEOUT 1200 ${groundhog_mdk_fixture}
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals;GH_MDK_DRIVER=${groundhog_mdk_driver}")
  endif()
  # The adopted-profile matrix (nostrc-a5u5): MDK v0.11.0 (cgka-engine, the
  # adopted Marmot profile) behind its own driver and image,
  # tests/interop/mdk/driver-0.11, run apart from the 0.8 test so a green 0.8
  # run never implies adopted success. One CTest per case of
  # tests/mls/test_mdk011_interop.c: the MDK<->MDK control and every adopted
  # case must pass (nostrc-lf62); the MDK 0.9.0 probe asserts the expected
  # incompatibility and exits 77 (XFAIL), which SKIP_RETURN_CODE reports as
  # Skipped, never Passed. Groundhog is linked as shipped (the UI library and
  # the service, no test hooks); -gui runs mdk-invites-groundhog through the
  # real invitations dialog (77 without a display). The driver's redacted
  # wire vectors land in ${CMAKE_BINARY_DIR}/mdk011-interop-artifacts.
  option(BUILD_MDK011_INTEROP
         "Build and run the Groundhog/MDK 0.11 (adopted profile) interop matrix (needs Docker)" OFF)
  set(MDK011_INTEROP_DRIVER "" CACHE STRING
      "MDK 0.11 interop driver command line; empty: the Docker image the test builds")
  if (BUILD_MDK011_INTEROP)
    if (NOT (BUILD_TESTING AND TARGET groundhog-test-schemas AND
             TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST AND TARGET PkgConfig::GROUNDHOG_JSON))
      message(FATAL_ERROR "BUILD_MDK011_INTEROP needs BUILD_TESTING, libsoup-3.0 and json-glib-1.0")
    endif()
    # W25: the media case uses the files layer (groundhog-mls-files, through
    # groundhog-mls-ui) with the local Blossom fixture.
    add_executable(test-groundhog-mdk011-interop tests/mls/test_mdk011_interop.c
      src/store/gh-store-conversations.c src/app/gh-nip17-envelope.c
      ${GROUNDHOG_DM_SEND_SOURCES} tests/media/blossom-fixture.c)
    target_include_directories(test-groundhog-mdk011-interop PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/app ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/mls ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/media ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
    target_link_libraries(test-groundhog-mdk011-interop PRIVATE groundhog-mls-ui marmot
      nostr_nip17_core nostr_nip59_core nostrc-test-bus
      PkgConfig::GROUNDHOG_RELAY_WIRE_TEST PkgConfig::GROUNDHOG_JSON)
    target_compile_options(test-groundhog-mdk011-interop PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-mdk011-interop)
    add_dependencies(test-groundhog-mdk011-interop groundhog-test-schemas)
    set(groundhog_mdk011_artifacts "${CMAKE_BINARY_DIR}/mdk011-interop-artifacts")
    file(MAKE_DIRECTORY "${groundhog_mdk011_artifacts}")
    # Every run starts with an empty vectors.jsonl (the drivers append; each
    # line also carries its driver process's run id).
    add_test(NAME groundhog-mdk011-interop-artifacts-reset
      COMMAND ${CMAKE_COMMAND} -E rm -f "${groundhog_mdk011_artifacts}/vectors.jsonl")
    set_tests_properties(groundhog-mdk011-interop-artifacts-reset PROPERTIES
      FIXTURES_SETUP groundhog-mdk011-artifacts)
    if (MDK011_INTEROP_DRIVER)
      set(groundhog_mdk011_driver "${MDK011_INTEROP_DRIVER}")
      set(groundhog_mdk011_fixture)
    else()
      set(groundhog_saved_appbundle "${CMAKE_FIND_APPBUNDLE}")
      set(CMAKE_FIND_APPBUNDLE NEVER)
      find_program(GROUNDHOG_DOCKER docker REQUIRED)
      set(CMAKE_FIND_APPBUNDLE "${groundhog_saved_appbundle}")
      # Image tags are shared by every checkout on the machine: a branch that
      # changes driver-0.11/ builds its own (e.g. -DMDK011_INTEROP_IMAGE=
      # nostrc-mdk-interop:0.11.0-mybranch) so it never races another's.
      set(MDK011_INTEROP_IMAGE "nostrc-mdk-interop:0.11.0" CACHE STRING
          "Docker tag the MDK 0.11 driver image is built and run under")
      set(groundhog_mdk011_image "${MDK011_INTEROP_IMAGE}")
      add_test(NAME groundhog-mdk011-interop-image
        COMMAND ${GROUNDHOG_DOCKER} build -t ${groundhog_mdk011_image}
                ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/interop/mdk/driver-0.11)
      set_tests_properties(groundhog-mdk011-interop-image PROPERTIES TIMEOUT 3600
        FIXTURES_SETUP groundhog-mdk011-image)
      set(groundhog_mdk011_fixture groundhog-mdk011-image)
      # As for 0.8: the host's network on Linux, host.docker.internal on
      # Docker Desktop (the URLs and the NIP-42 relay tag stay 127.0.0.1).
      # The artifact directory is mounted at /artifacts (single-quoted for
      # g_shell_parse_argv: a build path may hold spaces).
      set(groundhog_mdk011_run "${GROUNDHOG_DOCKER} run --rm -i -e MDK_DRIVER_LOG -e MDK_DRIVER_DEADLINE_S -e MDK_DRIVER_ARTIFACT_DIR=/artifacts -v '${groundhog_mdk011_artifacts}:/artifacts'")
      if (APPLE)
        set(groundhog_mdk011_driver "${groundhog_mdk011_run} -e MDK_DRIVER_DIAL_HOST=host.docker.internal ${groundhog_mdk011_image}")
      else()
        # Artifacts owned by the user running the tests, not root.
        execute_process(COMMAND id -u OUTPUT_VARIABLE groundhog_uid OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND id -g OUTPUT_VARIABLE groundhog_gid OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(groundhog_mdk011_driver "${groundhog_mdk011_run} --user ${groundhog_uid}:${groundhog_gid} --network host ${groundhog_mdk011_image}")
      endif()
    endif()
    # The MDK v0.9.0 expected-incompatible probe (KeyPackages only), its own
    # image; MDK09_PROBE_DRIVER replaces it with another command line.
    set(MDK09_PROBE_DRIVER "" CACHE STRING
        "MDK 0.9.0 probe driver command line; empty: the Docker image the test builds")
    if (MDK09_PROBE_DRIVER)
      set(groundhog_mdk09_driver "${MDK09_PROBE_DRIVER}")
      set(groundhog_mdk09_fixture)
    else()
      set(groundhog_saved_appbundle "${CMAKE_FIND_APPBUNDLE}")
      set(CMAKE_FIND_APPBUNDLE NEVER)
      find_program(GROUNDHOG_DOCKER docker REQUIRED)
      set(CMAKE_FIND_APPBUNDLE "${groundhog_saved_appbundle}")
      set(groundhog_mdk09_image nostrc-mdk-interop:0.9.0-probe)
      add_test(NAME groundhog-mdk011-interop-image-mdk09
        COMMAND ${GROUNDHOG_DOCKER} build -t ${groundhog_mdk09_image}
                ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/interop/mdk/driver-0.9-probe)
      set_tests_properties(groundhog-mdk011-interop-image-mdk09 PROPERTIES TIMEOUT 3600
        FIXTURES_SETUP groundhog-mdk09-image)
      set(groundhog_mdk09_fixture groundhog-mdk09-image)
      if (APPLE)
        set(groundhog_mdk09_driver "${GROUNDHOG_DOCKER} run --rm -i -e MDK_DRIVER_LOG -e MDK_DRIVER_DEADLINE_S -e MDK_DRIVER_DIAL_HOST=host.docker.internal ${groundhog_mdk09_image}")
      else()
        set(groundhog_mdk09_driver "${GROUNDHOG_DOCKER} run --rm -i -e MDK_DRIVER_LOG -e MDK_DRIVER_DEADLINE_S --network host ${groundhog_mdk09_image}")
      endif()
    endif()
    foreach(groundhog_mdk011_case control groundhog-invites-mdk mdk-invites-groundhog
            mdk-invites-groundhog-gui adopted-welcome white-noise-welcome white-noise-media
            white-noise-dm adopted-commits routing-rotation concurrent-commits polls
            white-noise-reactions mdk09-probe)
      if (groundhog_mdk011_case STREQUAL "mdk-invites-groundhog-gui")
        set(groundhog_mdk011_command test-groundhog-mdk011-interop --gui
            -p /groundhog/mdk011-interop-gui/mdk-invites-groundhog)
      else()
        set(groundhog_mdk011_command test-groundhog-mdk011-interop
            -p /groundhog/mdk011-interop/${groundhog_mdk011_case})
      endif()
      add_test(NAME groundhog-mdk011-interop-${groundhog_mdk011_case}
        COMMAND ${groundhog_mdk011_command})
      set_tests_properties(groundhog-mdk011-interop-${groundhog_mdk011_case} PROPERTIES
        TIMEOUT 900 SKIP_RETURN_CODE 77 RESOURCE_LOCK groundhog-mdk011
        ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals;GH_MDK_DRIVER=${groundhog_mdk011_driver};GH_MDK09_DRIVER=${groundhog_mdk09_driver};MDK_DRIVER_ARTIFACT_DIR=${groundhog_mdk011_artifacts}")
      set(groundhog_mdk011_fixtures groundhog-mdk011-artifacts ${groundhog_mdk011_fixture})
      if (groundhog_mdk011_case STREQUAL "mdk09-probe")
        list(APPEND groundhog_mdk011_fixtures ${groundhog_mdk09_fixture})
      endif()
      set_tests_properties(groundhog-mdk011-interop-${groundhog_mdk011_case} PROPERTIES
        FIXTURES_REQUIRED "${groundhog_mdk011_fixtures}")
    endforeach()
  endif()
else()
  message(STATUS "Groundhog MLS service: disabled (needs GhStoreMarmot, the DM inbox, "
                 "GhAuthPolicy, the conversation model, the relay lists and NIP-17/59)")
endif()
# --- end W20 MLS service ---

# --- G09 network modes and Tor ---
# Network modes, fail closed (privacy charter §4.1 D3 = A, §4.2, §4.3, §8.2
# G09; NT-5..NT-10, PT-5):
#  - gh-net-session.c (in groundhog-net): the network-mode and
#    tor-socks-address settings, per-purpose SOCKS5 credentials for Tor
#    stream isolation, the relay URL policy (.onion only in Tor mode, ws://
#    only for loopback and .onion) and the Tor reachability check. GhNetHttp,
#    NIP-05 and NIP-11 go through the Tor proxy in Tor mode;
#  - groundhog-net-relay: the libsoup relay transport (gh-relay-soup.c; src/net
#    only, check_privacy libsoup-boundary) and the dispatcher (gh-relay-net.c)
#    that the executable installs as the transport of every relay scope and
#    publish: Tor mode only ever uses libsoup through SOCKS5 with remote DNS;
#    System and No Proxy keep the GNostrRelay transport (libwebsockets has no
#    proxy support). A mode change closes every old connection.
# GROUNDHOG_HAVE_TOR=1 turns on GH_FEATURE_TOR (src/app/gh-features.h): the
# Preferences Tor choice and address, and the "Can't reach Tor" banner.
# Without G09 (GROUNDHOG_HAVE_TOR=0) the executable installs the network-mode
# guard instead (src/relay/gh-relay-guard.h, nostrc-6v0i): network-mode tor,
# or a mode the build does not know, connects to nothing (charter P5) and
# the window says "Tor isn't available in this build". GROUNDHOG_WITHOUT_TOR
# builds that configuration where libsoup is installed, to check it.
option(GROUNDHOG_WITHOUT_TOR "Build Groundhog without G09 network modes and Tor" OFF)
if (TARGET groundhog-net)
  target_sources(groundhog-net PRIVATE src/net/gh-net-session.c)
endif()
if (TARGET groundhog-net AND TARGET groundhog-relay AND NOT GROUNDHOG_WITHOUT_TOR)
  message(STATUS "Groundhog network modes and Tor: enabled")
  add_library(groundhog-net-relay STATIC src/net/gh-relay-soup.c src/net/gh-relay-net.c)
  target_link_libraries(groundhog-net-relay PUBLIC groundhog-net groundhog-relay)
  target_compile_options(groundhog-net-relay PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(groundhog PRIVATE groundhog-net-relay)
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_TOR=1)
  if (BUILD_TESTING AND TARGET groundhog-test-schemas)
    # NT-5..NT-9, PT-5 and the mode switch, on the SOCKS5 fixture (H3,
    # tests/net/socks5-fixture.c: records method, ATYP, host and credentials),
    # the local wire relay (H2), a loopback HTTP server, a tripwire listener
    # that must never be dialled, and a GResolver that records every local DNS
    # lookup (there must be none for a relay or NIP-05 host in Tor mode).
    add_executable(test-groundhog-net tests/net/test_relay_soup.c tests/net/socks5-fixture.c)
    target_include_directories(test-groundhog-net PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/net ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay)
    target_link_libraries(test-groundhog-net PRIVATE groundhog-net-relay)
    target_compile_options(test-groundhog-net PRIVATE -Wall -Wextra -Werror)
    add_dependencies(test-groundhog-net groundhog-test-schemas)
    add_test(NAME groundhog-net COMMAND test-groundhog-net)
    set_tests_properties(groundhog-net PROPERTIES TIMEOUT 120
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    # W16 review B1 (PD-6): no Groundhog TLS connection offers to resume a
    # session (src/net/gh-net-tls.h). A plain program, not a GTest one:
    # glib-networking disables resumption in any process that called
    # g_test_init(). A local TLS server reads each ClientHello (no
    # pre_shared_key, no session ticket) for GhNetHttp (No Proxy, then Tor)
    # and Tor-mode relay scopes through the SOCKS5 fixture, after a control
    # that shows a plain SoupSession does resume there.
    add_executable(test-groundhog-tls-resumption tests/net/test_tls_resumption.c
      tests/net/socks5-fixture.c)
    target_include_directories(test-groundhog-tls-resumption PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/net)
    target_link_libraries(test-groundhog-tls-resumption PRIVATE groundhog-net-relay
      OpenSSL::SSL OpenSSL::Crypto)
    target_compile_options(test-groundhog-tls-resumption PRIVATE -Wall -Wextra -Werror)
    add_dependencies(test-groundhog-tls-resumption groundhog-test-schemas)
    add_test(NAME groundhog-tls-resumption COMMAND test-groundhog-tls-resumption)
    set_tests_properties(groundhog-tls-resumption PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
  # NT-10 transport parity: the existing wire suites run twice, once on the
  # GNostrRelay transport and once in Tor mode (the dispatcher, libsoup and
  # the SOCKS5 fixture in front of the same local relays).
  foreach (wire test-groundhog-relay-wire test-groundhog-relay-publish-wire)
    if (TARGET ${wire})
      target_sources(${wire} PRIVATE tests/net/socks5-fixture.c)
      target_include_directories(${wire} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/net)
      target_link_libraries(${wire} PRIVATE groundhog-net-relay)
      target_compile_definitions(${wire} PRIVATE GROUNDHOG_WIRE_TOR=1)
    endif()
  endforeach()
  if (TEST groundhog-relay-wire)
    set_tests_properties(groundhog-relay-wire PROPERTIES TIMEOUT 60)
  endif()
  if (TEST groundhog-relay-publish-wire)
    set_tests_properties(groundhog-relay-publish-wire PROPERTIES TIMEOUT 90)
  endif()
else()
  message(STATUS "Groundhog network modes and Tor: disabled (needs groundhog-net and the "
                 "relay layer); network-mode tor connects to nothing")
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_TOR=0)
  if (TARGET groundhog-relay)
    target_link_libraries(groundhog PRIVATE groundhog-relay)
    target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_RELAY_GUARD=1)
  endif()
endif()
# The guard is part of the relay layer, so its test runs in every build: a
# recording direct transport (no open at all in Tor mode or for .onion) and
# the real GNostrRelay transport before a tripwire listener that Tor mode
# must never dial.
if (BUILD_TESTING AND TARGET groundhog-relay AND TARGET groundhog-test-schemas)
  add_executable(test-groundhog-relay-guard tests/relay/test_relay_guard.c)
  target_link_libraries(test-groundhog-relay-guard PRIVATE groundhog-relay)
  target_compile_options(test-groundhog-relay-guard PRIVATE -Wall -Wextra -Werror)
  add_dependencies(test-groundhog-relay-guard groundhog-test-schemas)
  add_test(NAME groundhog-relay-guard COMMAND test-groundhog-relay-guard)
  set_tests_properties(groundhog-relay-guard PROPERTIES TIMEOUT 60
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
endif()
# --- end G09 network modes and Tor ---
# --- G20b NIP-29 group UI ---
# The relay-group UI (privacy charter §7.5, §7.7, §7.9, §7.10, §7.15 #18;
# §8.2 G20b) over G20a's GhNip29Service: gh-group-copy.c (GTK-free: group
# references, the honest join/create/read copy, the composer's "closed to
# you" reason, admin gating from the relay-signed 39001/39003),
# GhGroupJoinDialog, GhGroupInfoDialog (+ GhGroupMemberRow) and
# GhNewGroupDialog (the relay part); gh-group-ui.c attaches them to the
# window (win.join-group, win.new-group, the composer delegate for relay
# groups, Group Info as the conversation-info group handler, older history
# from the store). Group rows get their kind glyph in gh-conversation-row.c.
if (TARGET groundhog-nip29-service AND TARGET groundhog-conversation-ui AND
    TARGET groundhog-recipient AND TARGET groundhog-privacy-summary)
  message(STATUS "Groundhog NIP-29 group UI: enabled")
  set(GROUNDHOG_GROUP_UI_SOURCES src/ui/gh-group-copy.c src/ui/gh-group-join-dialog.c
    src/ui/gh-group-info-dialog.c src/ui/gh-new-group-dialog.c)
  add_library(groundhog-group-ui STATIC ${GROUNDHOG_GROUP_UI_SOURCES})
  target_include_directories(groundhog-group-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
  target_link_libraries(groundhog-group-ui PUBLIC groundhog-nip29-service
    groundhog-conversation-ui groundhog-recipient groundhog-privacy-summary)
  target_compile_options(groundhog-group-ui PRIVATE -Wall -Wextra -Werror)
  if (TARGET groundhog-account-store AND GROUNDHOG_OUTBOX_SOURCES)
    # Beside gh-send-ui.c (G13), whose delegate it sets.
    target_sources(groundhog PRIVATE src/ui/gh-group-ui.c)
    target_link_libraries(groundhog PRIVATE groundhog-group-ui)
    target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_GROUP_UI=1)
  endif()
  if (BUILD_TESTING AND TARGET groundhog-test-schemas AND
      TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
    # Default mode (no display): the group UI's decisions against a local
    # NIP-29 relay (tests/nip29/nip29-relay.h) with the real service, outbox,
    # SQLCipher store and mock signer: references, join (pending, admitted,
    # declined, invite only, duplicate), sending into a group and its
    # "closed to you" reasons, retry, leave, admin gating from the relay's
    # 39001/39003 (and the service refusing what is denied), creating a
    # group (accepted, refused and dropped), older history from the store.
    # --gui: the dialogs themselves (Join, Group Info, New Group), the group
    # row's glyph and label, and the window glue (actions, the composer
    # delegate, Group Info) on a real window; it self-skips (77) without a
    # display. With GROUNDHOG_TEST_SCREENSHOTS=<dir> it renders the dialogs.
    add_executable(test-groundhog-group-ui tests/ui/test_group_ui.c
      tests/ui/group-send-stub.c src/ui/gh-group-ui.c)
    target_include_directories(test-groundhog-group-ui PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/app ${CMAKE_CURRENT_SOURCE_DIR}/tests/nip29
      ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
    target_link_libraries(test-groundhog-group-ui PRIVATE groundhog-group-ui
      PkgConfig::GROUNDHOG_RELAY_WIRE_TEST nostrc-test-bus)
    target_compile_options(test-groundhog-group-ui PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-group-ui)
    add_dependencies(test-groundhog-group-ui groundhog-test-schemas)
    add_test(NAME groundhog-group-ui COMMAND test-groundhog-group-ui)
    set_tests_properties(groundhog-group-ui PROPERTIES TIMEOUT 180
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    add_test(NAME groundhog-group-ui-gui COMMAND test-groundhog-group-ui --gui)
    set_tests_properties(groundhog-group-ui-gui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog NIP-29 group UI: disabled (needs the NIP-29 service and the "
                 "conversation UI)")
endif()
# --- end G20b NIP-29 group UI ---
# --- W21 encrypted-group UI (nostrc-9xf5, qp24.13 part 2) ---
# The UI over GhMlsService (privacy charter §7.5, §7.6, §7.7, §7.9, §7.10,
# §7.15 #13): gh-mls-invitee.c and gh-mls-copy.c (GTK-free: accepted
# contacts, the per-person KeyPackage check with libmarmot's account-proof
# test, Owner/Admin roles, the honest words), GhMlsInviteePicker, the
# "Encrypted Group" page of New Group (GhMlsNewGroupPage), GhMlsGroupInfoDialog
# and GhMlsInvitesDialog; gh-mls-ui.c attaches them to the window (the
# composer delegate, win.group-invitations and the sidebar entry, Group Info,
# the header's member count, "Unable to decrypt N messages yet"). The
# executable attaches it only while GH_FEATURE_ENCRYPTED_GROUPS is 1
# (gh-features.h); the tests attach it directly, so it is tested at 0.
if (TARGET groundhog-mls-service AND TARGET groundhog-group-ui)
  message(STATUS "Groundhog encrypted-group UI: enabled")
  set(GROUNDHOG_MLS_UI_SOURCES src/mls/gh-mls-invitee.c src/ui/gh-mls-copy.c
    src/ui/gh-mls-invitee-picker.c src/ui/gh-mls-new-group-page.c
    src/ui/gh-mls-group-info-dialog.c src/ui/gh-mls-invites-dialog.c
    src/ui/gh-create-poll-dialog.c src/ui/gh-poll-card.c)
  add_library(groundhog-mls-ui STATIC ${GROUNDHOG_MLS_UI_SOURCES})
  target_include_directories(groundhog-mls-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mls)
  target_link_libraries(groundhog-mls-ui PUBLIC groundhog-mls-service groundhog-group-ui)
  target_compile_options(groundhog-mls-ui PRIVATE -Wall -Wextra -Werror)
  if (TARGET groundhog-account-store AND GROUNDHOG_OUTBOX_SOURCES)
    # Beside gh-send-ui.c and gh-group-ui.c, whose hooks it uses.
    target_sources(groundhog PRIVATE src/ui/gh-mls-ui.c)
    target_link_libraries(groundhog PRIVATE groundhog-mls-ui)
    target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_MLS_UI=1)
  endif()
  # Trying encrypted groups before their backfill blockers close (see
  # gh-features.h): the executable only, never a release, a package or an
  # install (review M5; the rules are cmake/GroundhogPreviewGuard.cmake,
  # tested by groundhog-mls-preview-guard). A reused build directory keeps the
  # cached option, so a release configure on top of a preview tree fails here
  # instead of shipping it.
  option(GROUNDHOG_ENCRYPTED_GROUPS_PREVIEW
         "Build Groundhog with GH_FEATURE_ENCRYPTED_GROUPS=1 (development only)" OFF)
  if (GROUNDHOG_ENCRYPTED_GROUPS_PREVIEW)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/GroundhogPreviewGuard.cmake)
    set(groundhog_preview_cpack)
    if (DEFINED CPACK_PACKAGE_NAME OR DEFINED CPACK_GENERATOR)
      set(groundhog_preview_cpack CPACK)
    endif()
    groundhog_preview_problem(groundhog_preview_problem
      BUILD_TYPE "${CMAKE_BUILD_TYPE}" CONFIGURATION_TYPES ${CMAKE_CONFIGURATION_TYPES}
      INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" ${groundhog_preview_cpack})
    if (groundhog_preview_problem)
      message(FATAL_ERROR "Groundhog: GROUNDHOG_ENCRYPTED_GROUPS_PREVIEW=ON is for development "
                          "builds only, and ${groundhog_preview_problem}. Encrypted groups "
                          "ship when nostrc-5rfp, nostrc-cpwf and nostrc-kzun close (see "
                          "src/app/gh-features.h). Configure with "
                          "-DGROUNDHOG_ENCRYPTED_GROUPS_PREVIEW=OFF.")
    endif()
    message(WARNING "Groundhog: encrypted groups are ON (GROUNDHOG_ENCRYPTED_GROUPS_PREVIEW); "
                    "their backfill is not complete yet (nostrc-5rfp, nostrc-cpwf, "
                    "nostrc-kzun). Not for release; installing this build is refused.")
    target_compile_definitions(groundhog PRIVATE GH_FEATURE_ENCRYPTED_GROUPS=1)
    # Installing it is refused before Groundhog's first install rule (above).
  endif()
  if (BUILD_TESTING AND TARGET groundhog-test-schemas AND
      TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
    # Default mode (no display): the view model against the three-account
    # local-relay world of the MLS service tests (tests/mls/mls-world.h): the
    # words of every state, accepted contacts only (never a request), the
    # KeyPackage check (ready, not set up, needs an update: a legacy
    # KeyPackage without the account proof, no discovery relay), Owner and
    # Admin from the group's admin order, and the composer's reasons.
    # --gui: on a real window, New Group's encrypted page end to end (checks,
    # the enrollment states with Try Again, create, the invitee accepts in
    # the invitations dialog), sending and receiving through the composer
    # delegate, Group Info (badges, admin add and remove, rename, leave with
    # its copy), the header's member count, "Unable to decrypt N messages
    # yet", the row's lock glyph; it self-skips (77) without a display.
    add_executable(test-groundhog-mls-ui tests/ui/test_mls_ui.c tests/ui/group-send-stub.c
      src/ui/gh-group-ui.c src/ui/gh-mls-ui.c src/store/gh-store-conversations.c
      src/app/gh-nip17-envelope.c ${GROUNDHOG_DM_SEND_SOURCES} tests/media/blossom-fixture.c)
    target_include_directories(test-groundhog-mls-ui PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/app ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/mls ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/media ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
    target_link_libraries(test-groundhog-mls-ui PRIVATE groundhog-mls-ui marmot
      nostr_nip17_core nostr_nip59_core nostrc-test-bus PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
    target_compile_options(test-groundhog-mls-ui PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-mls-ui)
    add_dependencies(test-groundhog-mls-ui groundhog-test-schemas)
    # Review M5: the preview's release/package/install guard, in script mode.
    add_test(NAME groundhog-mls-preview-guard
      COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_preview_guard.cmake)
    set_tests_properties(groundhog-mls-preview-guard PROPERTIES TIMEOUT 30)
    add_test(NAME groundhog-mls-ui COMMAND test-groundhog-mls-ui)
    set_tests_properties(groundhog-mls-ui PROPERTIES TIMEOUT 300
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    add_test(NAME groundhog-mls-ui-gui COMMAND test-groundhog-mls-ui --gui)
    set_tests_properties(groundhog-mls-ui-gui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog encrypted-group UI: disabled (needs the MLS service and the "
                 "group UI)")
endif()
# --- end W21 encrypted-group UI ---
# --- W16 UX polish (nostrc-qp24.66, .70, .71, .72, .74, .80) ---
# Small UX items on top of G11-G19 (privacy charter §7.4, §7.8, §7.9, §7.13):
#  - GhBlockedPage (data/ui/gh-blocked-page.blp, gh-blocked-row.blp): Preferences
#    › Privacy › Blocked Conversations with Unblock (qp24.72), GTK-only in the
#    preferences library; gh-blocked-conversations.c lists and unblocks through
#    the encrypted store (gh_store_conversations_list_blocked(),
#    gh_conversation_actions_block());
#  - gh-conversation-menu.c (data/ui/gh-conversation-menu.blp): the actions
#    behind a conversation row's context menu, Mute…, Conversation Info and
#    Delete… (qp24.74), on GhConversationActions like Conversation Info;
#  - GhContactTitles: accepted conversations titled with the contact
#    directory's cached names, live, never a request's (qp24.66, PT-8).
# Ctrl+I (qp24.71), the header (qp24.70) and onboarding's Start a
# Conversation (qp24.80) are edits of the G11/G14 files.
target_sources(groundhog-preferences PRIVATE src/ui/gh-blocked-page.c)
set(GROUNDHOG_W16_MENU_SOURCES src/ui/gh-conversation-menu.c src/app/gh-blocked-conversations.c)
if (TARGET groundhog-account-store AND GROUNDHOG_OUTBOX_SOURCES)
  target_sources(groundhog PRIVATE src/app/gh-contact-titles.c)
endif()
if (TARGET test-groundhog-contact-directory)
  target_sources(test-groundhog-contact-directory PRIVATE src/app/gh-contact-titles.c)
endif()
if (TARGET groundhog-account-store AND TARGET groundhog-notifier AND GROUNDHOG_EXPIRY_SOURCES AND
    GROUNDHOG_OUTBOX_SOURCES)
  target_sources(groundhog PRIVATE ${GROUNDHOG_W16_MENU_SOURCES})
  if (BUILD_TESTING AND TARGET groundhog-test-schemas)
    # Default mode: the blocked list and unblock over a real (in-memory)
    # SQLCipher store. --gui: the row menu's Mute…, Conversation Info and
    # Delete… through their confirmations, Shift+F10/Menu, no menu for a
    # group, the Blocked Conversations page from Preferences, and a row and
    # header following a contact title; it self-skips (77) without a display.
    add_executable(test-groundhog-conversation-menu tests/ui/test_conversation_menu.c
      ${GROUNDHOG_W16_MENU_SOURCES} ${GROUNDHOG_CONVERSATION_INFO_SOURCES}
      ${GROUNDHOG_EXPIRY_SOURCES})
    target_include_directories(test-groundhog-conversation-menu PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
    target_link_libraries(test-groundhog-conversation-menu PRIVATE groundhog-notifier
      groundhog-store-conversations groundhog-conversation-ui groundhog-preferences)
    target_compile_options(test-groundhog-conversation-menu PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-conversation-menu)
    add_dependencies(test-groundhog-conversation-menu groundhog-test-schemas)
    add_test(NAME groundhog-conversation-menu COMMAND test-groundhog-conversation-menu)
    set_tests_properties(groundhog-conversation-menu PROPERTIES TIMEOUT 60
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals,fatal-warnings")
    add_test(NAME groundhog-conversation-menu-gui COMMAND test-groundhog-conversation-menu --gui)
    set_tests_properties(groundhog-conversation-menu-gui PROPERTIES SKIP_RETURN_CODE 77
      TIMEOUT 120
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
endif()
# --- end W16 UX polish ---

# --- W17 NIP-17 multi-recipient send (nostrc-qp24.78) ---
# One rumor per message to a NIP-17 room of up to 10 people: one seal and
# gift wrap per recipient plus the self-copy (fresh ephemeral key and
# created_at each), per-recipient outbox state, retries and status, nothing
# published for a recipient without a 10050, the disappearing timer, AUTH
# policy and blocks. With libsoup, three accounts over real local
# store-and-serve relays (H2); otherwise the rumor, limit and block cases.
if (TARGET test-groundhog-outbox AND GROUNDHOG_EXPIRY_SOURCES AND TARGET groundhog-auth-policy)
  add_executable(test-groundhog-multi-send tests/app/test_multi_send.c
    ${GROUNDHOG_OUTBOX_SOURCES} ${GROUNDHOG_DM_SEND_SOURCES} ${GROUNDHOG_EXPIRY_SOURCES}
    src/app/gh-nip17-envelope.c src/store/gh-store-conversations.c)
  target_include_directories(test-groundhog-multi-send PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/store ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
  target_link_libraries(test-groundhog-multi-send PRIVATE groundhog-account-relays
    groundhog-auth-policy groundhog-conversations groundhog-store-testhooks nostr_nip17_core
    nostr_nip59_core nostrc-test-bus)
  if (TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
    target_link_libraries(test-groundhog-multi-send PRIVATE PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
    target_compile_definitions(test-groundhog-multi-send PRIVATE GROUNDHOG_TEST_WIRE=1)
  endif()
  target_compile_options(test-groundhog-multi-send PRIVATE -Wall -Wextra -Werror)
  groundhog_store_link_sqlcipher_first(test-groundhog-multi-send)
  add_dependencies(test-groundhog-multi-send groundhog-test-schemas)
  add_test(NAME groundhog-multi-send COMMAND test-groundhog-multi-send)
  set_tests_properties(groundhog-multi-send PROPERTIES TIMEOUT 180
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  message(STATUS "Groundhog multi-recipient send tests: enabled")
else()
  message(STATUS "Groundhog multi-recipient send tests: disabled (needs the outbox and expiry)")
endif()
# --- end W17 NIP-17 multi-recipient send ---

# --- G21 encrypted attachments core ---
# NIP-17 kind-15 file messages (privacy charter §6, §0.1 item 9, D6, §8.2
# G21; AT-1..AT-9; the UI is G22):
#  - gh-nip17-file.c (in groundhog-conversations): the kind-15 tags, parsed
#    strictly and built canonically; the unwrap (gh-nip17-inbox.c), GhMessage
#    and the envelope admit kind 15 with valid tags; the envelope builds and
#    seals it like a text, to one person or a W17 room
#    (gh_nip17_rumor_new_file_room), and the outbox sends it
#    (gh_outbox_send_file, gh_outbox_send_file_room);
#  - groundhog-media: src/media/gh-metadata-strip.c (JPEG/PNG metadata out,
#    no re-encoding), gh-attachment.c (AES-256-GCM through OpenSSL EVP, x
#    before decrypting, the decode guard, the upload and the user's
#    Download) and gh-blossom-client.c (BUD-01/02 over GhNetHttp only, so
#    network mode, Tor and no TLS resumption hold; kind-24242 upload auth
#    with a throwaway key unless the user consented per server), and
#    src/store/gh-store-media.c (the decrypted cache inside the encrypted
#    store, a 200 MiB LRU; never a file).
# Nothing in the application calls groundhog-media yet (G22 adds the attach
# button and the card), so the executable's link set is unchanged and
# GH_FEATURE_ATTACHMENTS stays 0.
if (TARGET groundhog-conversations)
  target_sources(groundhog-conversations PRIVATE src/app/gh-nip17-file.c)
endif()
# Executables that compile the unwrap, envelope or outbox sources themselves
# (instead of linking the libraries that carry them) get the kind-15 codec
# beside them; linked through groundhog-conversations too, its object is then
# simply not pulled from the archive.
get_property(GROUNDHOG_G21_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach (target IN LISTS GROUNDHOG_G21_TARGETS)
  get_target_property(type ${target} TYPE)
  get_target_property(sources ${target} SOURCES)
  if (type STREQUAL "EXECUTABLE" AND sources MATCHES "gh-nip17-(inbox|envelope)\\.c|gh-outbox\\.c")
    target_sources(${target} PRIVATE src/app/gh-nip17-file.c)
  endif()
endforeach()
if (NOT TARGET OpenSSL::Crypto)
  find_package(OpenSSL QUIET)
endif()
if (TARGET groundhog-net AND TARGET groundhog-store AND TARGET groundhog-conversations AND
    TARGET OpenSSL::Crypto)
  message(STATUS "Groundhog encrypted attachments core: enabled")
  add_library(groundhog-media STATIC src/media/gh-attachment.c src/media/gh-blossom-client.c
    src/media/gh-metadata-strip.c src/store/gh-store-media.c)
  target_include_directories(groundhog-media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/media
    ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
  target_link_libraries(groundhog-media PUBLIC groundhog-net groundhog-store
    groundhog-conversations OpenSSL::Crypto)
  target_compile_options(groundhog-media PRIVATE -Wall -Wextra -Werror)
  if (BUILD_TESTING AND TARGET groundhog-test-schemas)
    # Offline: metadata stripping (AT-1's APP1, PNG chunks, damaged files),
    # the decode guard (AT-4), AES-GCM with x before decryption and the GCM
    # tag (AT-2), an independent Amethyst/0xchat vector, the kind-15 tags,
    # and the encrypted media cache.
    add_executable(test-groundhog-media tests/media/test_media.c)
    target_link_libraries(test-groundhog-media PRIVATE groundhog-media)
    target_compile_options(test-groundhog-media PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-media)
    add_test(NAME groundhog-media COMMAND test-groundhog-media)
    set_tests_properties(groundhog-media PROPERTIES TIMEOUT 60
      ENVIRONMENT "G_DEBUG=fatal-criticals")
    # A local Blossom server on SoupServer (tests/media/blossom-fixture.c:
    # BUD-02 upload with kind-24242 checks, BUD-01 GET, scripted refusals,
    # stalls and oversize answers) and the SOCKS5 fixture (H3): AT-1 round
    # trip, AT-2, AT-3, AT-6, AT-7, AT-8, AT-9, no server, and no file.
    add_executable(test-groundhog-blossom tests/media/test_blossom.c
      tests/media/blossom-fixture.c tests/net/socks5-fixture.c)
    target_include_directories(test-groundhog-blossom PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/media ${CMAKE_CURRENT_SOURCE_DIR}/tests/net
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/privacy)
    target_sources(test-groundhog-blossom PRIVATE tests/privacy/canary-scan.c)
    target_link_libraries(test-groundhog-blossom PRIVATE groundhog-media)
    target_compile_options(test-groundhog-blossom PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-blossom)
    add_dependencies(test-groundhog-blossom groundhog-test-schemas)
    add_test(NAME groundhog-blossom COMMAND test-groundhog-blossom)
    set_tests_properties(groundhog-blossom PROPERTIES TIMEOUT 120
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
  # Encrypted group attachments (nostrc-u7cb): MIP-04 encrypted-media-v2
  # through libmarmot, Blossom through GhBlossomClient (GhNetHttp). Nothing
  # calls it yet: the group conversation UI is nostrc-u7cb's follow-up.
  if (TARGET marmot)
    add_library(groundhog-mls-media STATIC src/mls/gh-mls-media.c)
    target_include_directories(groundhog-mls-media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/mls)
    target_link_libraries(groundhog-mls-media PUBLIC groundhog-media marmot)
    target_compile_options(groundhog-mls-media PRIVATE -Wall -Wextra -Werror)
    if (BUILD_TESTING AND TARGET groundhog-test-schemas)
      # Seal (metadata out, current epoch), upload to and fetch from the
      # local Blossom fixture, imeta round trip, retained-epoch open after a
      # Commit, the epoch check before send, damaged and unavailable
      # attachments, locator fallback, and files on this device only.
      add_executable(test-groundhog-mls-media tests/mls/test_mls_media.c
        tests/media/blossom-fixture.c)
      target_include_directories(test-groundhog-mls-media PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/media)
      target_link_libraries(test-groundhog-mls-media PRIVATE groundhog-mls-media)
      target_compile_options(test-groundhog-mls-media PRIVATE -Wall -Wextra -Werror)
      groundhog_store_link_sqlcipher_first(test-groundhog-mls-media)
      add_dependencies(test-groundhog-mls-media groundhog-test-schemas)
      add_test(NAME groundhog-mls-media COMMAND test-groundhog-mls-media)
      set_tests_properties(groundhog-mls-media PROPERTIES TIMEOUT 90
        ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    endif()
  endif()
  # W25 (nostrc-q3a6, nostrc-m6tp): GhMlsAttachments, an encrypted group's
  # files and picture for the window (gh-mls-attachments.h) over
  # GhMlsService, gh-mls-media and the account's GhAttachments: downloads
  # only on request (cache, own locators, then the group's verified media
  # servers), sends resealed on an epoch change, the 0x8002 picture fetched
  # on request and set by admins. gh-mls-attachment-ui.c wires it into the
  # attach flow and the cards; gh-mls-ui.c into Group Info.
  if (TARGET groundhog-mls-media AND TARGET groundhog-mls-service)
    add_library(groundhog-mls-files STATIC src/mls/gh-mls-attachments.c)
    target_include_directories(groundhog-mls-files PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/mls)
    target_link_libraries(groundhog-mls-files PUBLIC groundhog-mls-service groundhog-mls-media)
    target_compile_options(groundhog-mls-files PRIVATE -Wall -Wextra -Werror)
    if (TARGET groundhog-mls-ui)
      target_link_libraries(groundhog-mls-ui PUBLIC groundhog-mls-files)
    endif()
    if (BUILD_TESTING AND TARGET groundhog-test-schemas AND
        TARGET PkgConfig::GROUNDHOG_RELAY_WIRE_TEST AND GROUNDHOG_MLS_SERVICE_SOURCES)
      # Real accounts, stores and services (mls-world.h) with the local
      # Blossom and SOCKS5 fixtures: no fetch before Download, the epoch
      # from the message (restored after a restart), the cache bound to the
      # message, the neutral name, resealing on an epoch change, a stale
      # epoch never sent, Tor for every transfer, no picture in a legacy
      # group, the picture policy and Save As names. The test-hooks store,
      # so the media sources are compiled in (as test-groundhog-multi-send).
      add_executable(test-groundhog-mls-files tests/mls/test_mls_files.c
        ${GROUNDHOG_MLS_SERVICE_SOURCES} src/store/gh-store-marmot.c src/store/gh-mls-commits.c
        src/store/gh-store-conversations.c src/app/gh-nip17-envelope.c
        ${GROUNDHOG_DM_SEND_SOURCES} src/mls/gh-mls-media.c src/mls/gh-mls-attachments.c
        src/media/gh-attachment.c src/media/gh-attachments.c src/media/gh-blossom-client.c
        src/media/gh-metadata-strip.c src/store/gh-store-media.c src/store/gh-store-blossom.c
        tests/media/blossom-fixture.c tests/net/socks5-fixture.c)
      target_include_directories(test-groundhog-mls-files PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mls ${CMAKE_CURRENT_SOURCE_DIR}/src/store
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media ${CMAKE_CURRENT_SOURCE_DIR}/tests/app
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay ${CMAKE_CURRENT_SOURCE_DIR}/tests/media
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/net)
      target_link_libraries(test-groundhog-mls-files PRIVATE groundhog-store-testhooks marmot
        groundhog-dm-inbox groundhog-auth-policy groundhog-conversations
        groundhog-account-relays groundhog-net nostr_nip17_core nostr_nip59_core
        nostrc-test-bus OpenSSL::Crypto PkgConfig::GROUNDHOG_RELAY_WIRE_TEST)
      target_compile_options(test-groundhog-mls-files PRIVATE -Wall -Wextra -Werror)
      target_compile_definitions(test-groundhog-mls-files PRIVATE GH_MLS_TEST_HOOKS=1
        ${GROUNDHOG_MLS_SERVICE_DEFS})
      groundhog_store_link_sqlcipher_first(test-groundhog-mls-files)
      add_dependencies(test-groundhog-mls-files groundhog-test-schemas)
      add_test(NAME groundhog-mls-files COMMAND test-groundhog-mls-files)
      set_tests_properties(groundhog-mls-files PROPERTIES TIMEOUT 300
        ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    endif()
  endif()
  # W17 review B1: the cache is bound to the file (x, key and nonce), not
  # to x: a message naming another file's x with its own key misses, goes to
  # the network and fails as damaged, and cannot keep that file's plaintext
  # past its message (the reviewer's repro; Tor mode through the SOCKS5
  # fixture to a .onion on the local Blossom fixture).
  if (BUILD_TESTING AND TARGET groundhog-test-schemas)
    add_executable(test-groundhog-media-cache-binding tests/media/test_cache_binding.c
      tests/media/blossom-fixture.c tests/net/socks5-fixture.c)
    target_include_directories(test-groundhog-media-cache-binding PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/media ${CMAKE_CURRENT_SOURCE_DIR}/tests/net)
    target_link_libraries(test-groundhog-media-cache-binding PRIVATE groundhog-media)
    target_compile_options(test-groundhog-media-cache-binding PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-media-cache-binding)
    add_dependencies(test-groundhog-media-cache-binding groundhog-test-schemas)
    add_test(NAME groundhog-media-cache-binding COMMAND test-groundhog-media-cache-binding)
    set_tests_properties(groundhog-media-cache-binding PROPERTIES TIMEOUT 60
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
  # W17 rooms (nostrc-qp24.78) with files: a kind-15 message to Bob and
  # Carol through gh_outbox_send_file_room(), each downloading and
  # decrypting it. That test links the test-hooks store, so the media
  # sources are compiled in rather than linking groundhog-media, which
  # brings the production store.
  if (TARGET test-groundhog-multi-send)
    target_sources(test-groundhog-multi-send PRIVATE src/media/gh-attachment.c
      src/media/gh-blossom-client.c src/media/gh-metadata-strip.c src/store/gh-store-media.c
      tests/media/blossom-fixture.c)
    target_include_directories(test-groundhog-multi-send PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/media ${CMAKE_CURRENT_SOURCE_DIR}/tests/media)
    target_link_libraries(test-groundhog-multi-send PRIVATE groundhog-net OpenSSL::Crypto)
    target_compile_definitions(test-groundhog-multi-send PRIVATE GROUNDHOG_TEST_ATTACHMENTS=1)
  endif()
  # nostrc-5x5b: a cached file's plaintext goes with its message (expiry and
  # retention purge, forget, block and forget, outbox delete), checked in
  # the rows, the WAL, the raw store files and every decrypted page (H7).
  if (BUILD_TESTING AND TARGET groundhog-store-conversations)
    add_executable(test-groundhog-media-purge tests/media/test_media_purge.c
      tests/privacy/canary-scan.c)
    target_include_directories(test-groundhog-media-purge PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/privacy)
    target_link_libraries(test-groundhog-media-purge PRIVATE groundhog-media
      groundhog-store-conversations)
    target_compile_options(test-groundhog-media-purge PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-media-purge)
    add_test(NAME groundhog-media-purge COMMAND test-groundhog-media-purge)
    set_tests_properties(groundhog-media-purge PROPERTIES TIMEOUT 60
      ENVIRONMENT "G_DEBUG=fatal-criticals")
  endif()
  # G24's AT-5 with attachments and PT-2 with an admitted kind 15: the
  # privacy harness sends, receives and downloads a file end to end.
  if (TARGET test-groundhog-privacy-e2e)
    target_sources(test-groundhog-privacy-e2e PRIVATE tests/media/blossom-fixture.c)
    target_include_directories(test-groundhog-privacy-e2e PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/media)
    target_link_libraries(test-groundhog-privacy-e2e PRIVATE groundhog-media)
    target_compile_definitions(test-groundhog-privacy-e2e PRIVATE GROUNDHOG_TEST_ATTACHMENTS=1)
  endif()
else()
  message(STATUS "Groundhog encrypted attachments core: disabled (needs groundhog-net, the "
                 "GhStore, the conversation model and OpenSSL)")
endif()
# --- end G21 encrypted attachments core ---
# --- W18 messaging polish (nostrc-qp24.75, .83, .84, .86, yp69, 9cho, lff5) ---
#  - GhDmSender spaces a room's wraps on a GhClock (charter §4.5 S4 on the
#    direct path, nostrc-yp69): every executable that compiles gh-dm-send.c
#    gets the clock, GTK-free and without the store (groundhog-clock; linked
#    beside groundhog-store too, its object is then simply not pulled twice).
if (TARGET PkgConfig::GROUNDHOG_STORE_DEPS)
  add_library(groundhog-clock STATIC src/store/gh-clock.c)
  target_include_directories(groundhog-clock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/store)
  target_link_libraries(groundhog-clock PUBLIC PkgConfig::GROUNDHOG_STORE_DEPS)
  target_compile_options(groundhog-clock PRIVATE -Wall -Wextra -Werror)
  get_property(GROUNDHOG_W18_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach (target IN LISTS GROUNDHOG_W18_TARGETS)
    get_target_property(type ${target} TYPE)
    get_target_property(sources ${target} SOURCES)
    if (type STREQUAL "EXECUTABLE" AND sources MATCHES "gh-dm-send\\.c")
      target_link_libraries(${target} PRIVATE groundhog-clock)
    endif()
  endforeach()
endif()
#  - The conversation timeline's rows (nostrc-qp24.83): a message or a local
#    event such as the disappearing timer's change (GhTimelineRow,
#    data/ui/gh-timeline-row.blp; the list item binds it).
if (TARGET groundhog-conversation-ui)
  target_sources(groundhog-conversation-ui PRIVATE src/ui/gh-timeline-row.c)
endif()
# --- end W18 messaging polish ---
# --- G22 attachment UI ---
# Encrypted attachments in the app (privacy charter §6, D6, §7.4, §7.14,
# §8.2 G22; AT-7 in the UI, a cancel test, Save through the Save dialog;
# nostrc-dnsc folded in):
#  - GhAttachmentTransfer (src/app/gh-attachment-transfer.c, GTK-free, in
#    groundhog-conversations): one file message's download state, shared by
#    every recycled row that shows it;
#  - GhAttachmentCard and GhAttachmentSheet (src/ui/, data/ui/*.blp, in the
#    conversation UI): the card in a file message's bubble (Download, Cancel,
#    the photo after the decode guard, Save As…) and the sheet between
#    choosing a file and sending it (server choice, preview, consent);
#  - GhAttachments (src/media/gh-attachments.c, in groundhog-media): the
#    open store's Blossom client, downloads only on request, cancel, honest
#    errors, uploads, the cache's size and Clear, and the per-server upload
#    consent kept in the encrypted store (src/store/gh-store-blossom.c,
#    nostrc-dnsc);
#  - gh-attachment-ui.c (the executable, beside gh-send-ui.c): the
#    composer's attach button, drop and paste, the sheet, the outbox
#    (gh_outbox_send_file_room), the cards' provider and Save As.
# GROUNDHOG_HAVE_ATTACHMENTS=1 turns on GH_FEATURE_ATTACHMENTS
# (src/app/gh-features.h): Preferences › Attachments becomes editable.
if (TARGET groundhog-conversations)
  target_sources(groundhog-conversations PRIVATE src/app/gh-attachment-transfer.c)
endif()
if (TARGET groundhog-conversation-ui)
  target_sources(groundhog-conversation-ui PRIVATE src/ui/gh-attachment-card.c
    src/ui/gh-attachment-sheet.c)
endif()
if (TARGET groundhog-media)
  target_sources(groundhog-media PRIVATE src/media/gh-attachments.c src/store/gh-store-blossom.c)
endif()
if (TARGET groundhog-media AND TARGET groundhog-account-store AND GROUNDHOG_OUTBOX_SOURCES AND
    TARGET groundhog-conversation-ui)
  message(STATUS "Groundhog attachment UI: enabled")
  target_sources(groundhog PRIVATE src/app/gh-attachment-ui.c)
  target_link_libraries(groundhog PRIVATE groundhog-media)
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_ATTACHMENTS=1)
  if (BUILD_TESTING AND TARGET groundhog-test-schemas)
    # Display-free: the server list rules (add, remove, order), the upload
    # consent kept per account in the encrypted store and loaded into the
    # client (a restart keeps it for that account only; revoked means no
    # signer call), downloads only on request, from the cache or the local
    # Blossom fixture, cancel, the held-bytes bound, Clear, the sender's
    # own file, the upload's server, and the error words.
    add_executable(test-groundhog-attachments tests/media/test_attachments.c
      tests/media/blossom-fixture.c)
    target_include_directories(test-groundhog-attachments PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/media)
    target_link_libraries(test-groundhog-attachments PRIVATE groundhog-media
      groundhog-preferences groundhog-conversation-ui)
    target_compile_options(test-groundhog-attachments PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-attachments)
    add_dependencies(test-groundhog-attachments groundhog-test-schemas)
    add_test(NAME groundhog-attachments COMMAND test-groundhog-attachments)
    set_tests_properties(groundhog-attachments PROPERTIES TIMEOUT 90
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
    # The window (needs a display, self-skips with 77): the account stack of
    # the composer tests (mock signer bus, FakeSecret, SQLCipher, recording
    # relays) with the local Blossom fixture. Attach -> sheet -> upload ->
    # outbox -> wrap published, the first-use server choice, consent,
    # drop and paste, a received file's card fetching nothing until
    # Download, Cancel, Save As writing only the chosen file, the damaged
    # and unreachable errors, and Preferences › Attachments.
    add_executable(test-groundhog-attachment-ui tests/ui/test_attachment_ui.c
      ${GROUNDHOG_SEND_TEST_SOURCES} src/app/gh-attachment-ui.c tests/media/blossom-fixture.c)
    target_include_directories(test-groundhog-attachment-ui PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/store ${CMAKE_CURRENT_SOURCE_DIR}/tests/app
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/media)
    target_link_libraries(test-groundhog-attachment-ui PRIVATE groundhog-account-store
      groundhog-conversation-ui groundhog-auth-policy groundhog-media groundhog-preferences
      nostr_nip17_core nostr_nip59_core nostrc-test-bus)
    target_compile_options(test-groundhog-attachment-ui PRIVATE -Wall -Wextra -Werror)
    groundhog_store_link_sqlcipher_first(test-groundhog-attachment-ui)
    add_dependencies(test-groundhog-attachment-ui groundhog-test-schemas)
    # W25 re-review R2: the application's encrypted-group delegate
    # (gh-mls-attachment-ui.c) on a real window, under fatal-criticals.
    if (TARGET groundhog-mls-files)
      target_sources(test-groundhog-attachment-ui PRIVATE src/app/gh-mls-attachment-ui.c
        src/ui/gh-mls-copy.c)
      target_link_libraries(test-groundhog-attachment-ui PRIVATE groundhog-mls-files)
      target_compile_definitions(test-groundhog-attachment-ui PRIVATE
        GROUNDHOG_TEST_MLS_FILES=1)
    endif()
    add_test(NAME groundhog-attachment-ui COMMAND test-groundhog-attachment-ui)
    set_tests_properties(groundhog-attachment-ui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${GROUNDHOG_TEST_SCHEMAS};GSETTINGS_BACKEND=memory;G_DEBUG=fatal-criticals")
  endif()
else()
  message(STATUS "Groundhog attachment UI: disabled (needs the attachments core, the account "
                 "store and the outbox)")
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_ATTACHMENTS=0)
endif()
# W25 (nostrc-q3a6): files in encrypted groups in the window, the attachment
# UI's group delegate over GhMlsAttachments (gh-mls-attachment-ui.c), with
# both the attachment UI and the encrypted-group UI.
if (TARGET groundhog-mls-files AND TARGET groundhog-mls-ui AND TARGET groundhog-media AND
    TARGET groundhog-account-store AND GROUNDHOG_OUTBOX_SOURCES AND
    TARGET groundhog-conversation-ui)
  message(STATUS "Groundhog encrypted-group files: enabled")
  target_sources(groundhog PRIVATE src/app/gh-mls-attachment-ui.c)
  target_link_libraries(groundhog PRIVATE groundhog-mls-files)
  target_compile_definitions(groundhog PRIVATE GROUNDHOG_HAVE_MLS_FILES=1)
endif()
# --- end G22 attachment UI ---
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

# Tests that include nostrc-test-gdk-frame.h (nostrc-ykxf) but do not link
# nostrc-test-bus (which carries tests/common as a PUBLIC include directory).
foreach(_gdk_frame_test
  test-groundhog-shell
  test-groundhog-account-ui
  test-groundhog-conversation-list
  test-groundhog-conversation-view
  test-groundhog-conversation-menu
  test-groundhog-preferences
)
  if(TARGET ${_gdk_frame_test})
    target_include_directories(${_gdk_frame_test} PRIVATE
      ${CMAKE_SOURCE_DIR}/tests/common)
  endif()
endforeach()
