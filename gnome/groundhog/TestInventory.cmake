include_guard(DIRECTORY)
# Exact registrations from this directory, not test-name ownership inference.
# Unlisted registrations remain visible blocking gaps in the realization audit.
# Include LAST: adopted interop receives MLS-files and voice link edges late in
# CMakeLists.txt; the File API supplies that final closure without copying it here.
if(NOT COMMAND nostrc_describe_test)
  return()
endif()

function(_gh_describe name target owner)
  if(TEST "${name}")
    cmake_parse_arguments(PARSE_ARGV 3 ARG "" "EXPECTED" "INPUTS;REQUIRES;TARGETS")
    if(NOT ARG_EXPECTED)
      set(ARG_EXPECTED pass)
    endif()
    set(_targets "${target}" ${ARG_TARGETS})
    get_property(_environment TEST "${name}" PROPERTY ENVIRONMENT)
    if(_environment MATCHES "GSETTINGS_SCHEMA_DIR=" AND TARGET groundhog-test-schemas)
      list(APPEND _targets groundhog-test-schemas)
      list(APPEND ARG_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/data/org.nostr.Groundhog.gschema.xml")
    endif()
    nostrc_describe_test("${name}" OWNER "${owner}" TARGETS ${_targets}
      INPUTS ${ARG_INPUTS} REQUIRES ${ARG_REQUIRES} EXPECTED "${ARG_EXPECTED}")
  endif()
endfunction()

# Additional runtime paths are taken from registration commands and compile
# definitions in CMakeLists.txt (not inferred from execution names).
_gh_describe(groundhog-about test-groundhog-about groundhog.shell_ui)
_gh_describe(groundhog-about-gui test-groundhog-about groundhog.shell_ui REQUIRES display)
_gh_describe(groundhog-conversation-view test-groundhog-conversation-view groundhog.unclassified
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/fixtures")
_gh_describe(groundhog-privacy-summary test-groundhog-privacy-summary groundhog.unclassified
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/snapshots")
_gh_describe(groundhog-icons test-groundhog-icons groundhog.unclassified
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/data/ui" "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
_gh_describe(groundhog-inbox-setup test-groundhog-inbox-setup groundhog.unclassified
  INPUTS "${GROUNDHOG_DATA}/relay-suggestions.json")
_gh_describe(groundhog-background test-groundhog-background groundhog.unclassified
  TARGETS groundhog INPUTS "${GROUNDHOG_AUTOSTART_DESKTOP}" REQUIRES private_bus)
_gh_describe(groundhog-background-gui test-groundhog-background groundhog.unclassified
  TARGETS groundhog INPUTS "${GROUNDHOG_AUTOSTART_DESKTOP}" REQUIRES display)
_gh_describe(groundhog-bus-probe test-groundhog-bus-probe groundhog.unclassified
  TARGETS groundhog REQUIRES private_bus)
_gh_describe(groundhog-two-instance test-groundhog-two-instance groundhog.unclassified
  TARGETS groundhog REQUIRES private_bus)
_gh_describe(groundhog-i18n test-groundhog-i18n groundhog.resources TARGETS groundhog-i18n
  INPUTS "${groundhog_LOCALE_BUILD_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/po"
    "${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Groundhog.desktop"
    "${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Groundhog.metainfo.xml")

if(TEST groundhog-schema)
  nostrc_describe_test(groundhog-schema OWNER groundhog.resources TARGETS groundhog-schema
    INPUTS "${GROUNDHOG_DATA}/org.nostr.Groundhog.gschema.xml"
      "${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Groundhog.gschema.xml")
endif()
if(TEST groundhog-data)
  nostrc_describe_test(groundhog-data OWNER groundhog.resources TARGETS groundhog
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_data.py" "${GROUNDHOG_DATA}")
endif()
if(TEST groundhog-blueprint)
  nostrc_describe_test(groundhog-blueprint OWNER groundhog.resources
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_blueprint.py" "${GROUNDHOG_DATA}/ui")
endif()
if(TEST groundhog-desktop)
  nostrc_describe_test(groundhog-desktop OWNER groundhog.resources TARGETS groundhog-i18n
    INPUTS "${GROUNDHOG_DATA}/org.nostr.Groundhog.desktop.in"
      "${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Groundhog.desktop")
endif()
if(TEST groundhog-metainfo)
  nostrc_describe_test(groundhog-metainfo OWNER groundhog.resources
    INPUTS "${GROUNDHOG_DATA}/org.nostr.Groundhog.metainfo.xml")
endif()
if(TEST groundhog-metainfo-merged)
  nostrc_describe_test(groundhog-metainfo-merged OWNER groundhog.resources TARGETS groundhog-i18n
    INPUTS "${GROUNDHOG_DATA}/org.nostr.Groundhog.metainfo.xml"
      "${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Groundhog.metainfo.xml")
endif()
if(TEST groundhog-background-desktop)
  nostrc_describe_test(groundhog-background-desktop OWNER groundhog.resources TARGETS groundhog-i18n
    INPUTS "${GROUNDHOG_AUTOSTART_DESKTOP}")
endif()
if(TEST groundhog-potfiles)
  nostrc_describe_test(groundhog-potfiles OWNER groundhog.unclassified
    INPUTS "${CMAKE_SOURCE_DIR}/cmake/nostrc-check-potfiles.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/po/POTFILES" "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${GROUNDHOG_DATA}/ui")
endif()
if(TEST groundhog-privacy-static)
  nostrc_describe_test(groundhog-privacy-static OWNER groundhog.unclassified
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_privacy.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/src" "${GROUNDHOG_DATA}")
endif()

_gh_describe(groundhog-store-marmot test-groundhog-store-marmot groundhog.mls_store)
_gh_describe(groundhog-mls-service test-groundhog-mls-service groundhog.mls_service)
_gh_describe(groundhog-mls-remote test-groundhog-mls-remote groundhog.mls_service)
_gh_describe(groundhog-mls-poll test-groundhog-mls-poll groundhog.mls_service)
_gh_describe(groundhog-privacy-mls test-groundhog-privacy-mls groundhog.mls_service)
_gh_describe(groundhog-mls-ui test-groundhog-mls-ui groundhog.mls_ui)
_gh_describe(groundhog-mls-ui-gui test-groundhog-mls-ui groundhog.mls_ui REQUIRES display)
_gh_describe(groundhog-poll-ui test-groundhog-poll-ui groundhog.poll_ui)
_gh_describe(groundhog-group-ui test-groundhog-group-ui groundhog.group_ui)
_gh_describe(groundhog-group-ui-gui test-groundhog-group-ui groundhog.group_ui REQUIRES display)
_gh_describe(groundhog-mls-media test-groundhog-mls-media groundhog.mls_media)
_gh_describe(groundhog-mls-files test-groundhog-mls-files groundhog.mls_files)
_gh_describe(groundhog-voice test-groundhog-voice groundhog.voice
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/media/fixtures/voice")
_gh_describe(groundhog-voice-ui test-groundhog-voice-ui groundhog.voice REQUIRES display
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/media/fixtures/voice")
_gh_describe(groundhog-version groundhog groundhog.shell_ui)
_gh_describe(groundhog-launch groundhog groundhog.shell_ui REQUIRES display private_bus)
_gh_describe(groundhog-two-instance-acceptance test-groundhog-two-instance-acceptance
  groundhog.mls_service REQUIRES display private_bus)
_gh_describe(groundhog-two-instance-remote-acceptance test-groundhog-two-instance-remote-acceptance
  groundhog.mls_service REQUIRES display private_bus)

if(TEST groundhog-packaging-version)
  nostrc_describe_test(groundhog-packaging-version OWNER packaging.groundhog version.contract
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckPackagingVersion.cmake"
      "${CMAKE_SOURCE_DIR}/flake.nix"
      "${CMAKE_SOURCE_DIR}/packaging/archlinux/groundhog/PKGBUILD"
      "${CMAKE_SOURCE_DIR}/packaging/rpm/groundhog.spec"
      "${CMAKE_SOURCE_DIR}/packaging/debian-desktop/changelog")
endif()
if(TEST groundhog-test-gst-registry)
  nostrc_describe_test(groundhog-test-gst-registry OWNER testing.framework EXPECTED fixture)
endif()

# A configured single native peer file is an execution input, not an image-build
# receipt. prepared_peer is admitted only by the host-owned contained runtime;
# file presence here never establishes provenance or lifecycle success.
function(_gh_mdk_override out_requires out_inputs command_line)
  set(_requires docker private_bus)
  set(_inputs "")
  separate_arguments(_argv UNIX_COMMAND "${command_line}")
  list(LENGTH _argv _argc)
  if(_argc EQUAL 1)
    list(GET _argv 0 _driver)
    if(IS_ABSOLUTE "${_driver}" AND EXISTS "${_driver}" AND NOT IS_DIRECTORY "${_driver}")
      set(_requires prepared_peer private_bus)
      set(_inputs "${_driver}")
    endif()
  endif()
  set(${out_requires} "${_requires}" PARENT_SCOPE)
  set(${out_inputs} "${_inputs}" PARENT_SCOPE)
endfunction()

_gh_mdk_override(_mdk08_requires _mdk08_inputs "${MDK_INTEROP_DRIVER}")
_gh_mdk_override(_mdk011_requires _mdk011_inputs "${MDK011_INTEROP_DRIVER}")
_gh_mdk_override(_mdk09_requires _mdk09_inputs "${MDK09_PROBE_DRIVER}")

# Legacy directly compiles gh-mls-copy.c; adopted links the shipped MLS UI.
# Do not refine away those actual inputs (nor its late voice linkage).
_gh_describe(groundhog-mdk-interop test-groundhog-mdk-interop groundhog.mls_service
  INPUTS "${CMAKE_SOURCE_DIR}/tests/interop/mdk/driver" ${_mdk08_inputs}
  REQUIRES ${_mdk08_requires})
foreach(_case control groundhog-invites-mdk mdk-invites-groundhog
    mdk-invites-groundhog-gui adopted-welcome white-noise-welcome white-noise-media
    media-policy white-noise-voice white-noise-dm adopted-commits routing-rotation
    concurrent-commits polls white-noise-reactions)
  set(_requires ${_mdk011_requires})
  if(_case STREQUAL "mdk-invites-groundhog-gui")
    list(APPEND _requires display)
  endif()
  _gh_describe("groundhog-mdk011-interop-${_case}" test-groundhog-mdk011-interop
    groundhog.mls_service INPUTS "${CMAKE_SOURCE_DIR}/tests/interop/mdk/driver-0.11" ${_mdk011_inputs}
    REQUIRES ${_requires})
endforeach()
_gh_describe(groundhog-mdk011-interop-mdk09-probe test-groundhog-mdk011-interop
  groundhog.mls_service INPUTS "${CMAKE_SOURCE_DIR}/tests/interop/mdk/driver-0.11"
    "${CMAKE_SOURCE_DIR}/tests/interop/mdk/driver-0.9-probe" ${_mdk011_inputs} ${_mdk09_inputs}
  REQUIRES ${_mdk011_requires} ${_mdk09_requires} EXPECTED mdk09_incompatibility)
foreach(_pair
    "groundhog-mdk-interop-image|driver"
    "groundhog-mdk011-interop-image|driver-0.11"
    "groundhog-mdk011-interop-image-mdk09|driver-0.9-probe")
  string(REPLACE "|" ";" _pair "${_pair}")
  list(GET _pair 0 _name)
  list(GET _pair 1 _driver)
  if(TEST "${_name}")
    nostrc_describe_test("${_name}" OWNER groundhog.mls_service EXPECTED fixture
      INPUTS "${CMAKE_SOURCE_DIR}/tests/interop/mdk/${_driver}" REQUIRES docker)
  endif()
endforeach()
if(TEST groundhog-mdk011-interop-artifacts-reset)
  nostrc_describe_test(groundhog-mdk011-interop-artifacts-reset
    OWNER testing.framework EXPECTED fixture)
endif()

# All remaining registrations retain conservative component ownership, but still
# declare their actual GSETTINGS runtime/producer inputs. Target identity comes
# from CTest's command and File API, never from a synthesized test-name prefix.
# Literal command/target pairs observed in the real 412-registration Linux
# export. GUI aliases share the actual executable; no name-to-target synthesis.
set(_gh_remaining_targets
    "groundhog-account|test-groundhog-account"
    "groundhog-account-backends|test-groundhog-account-backends"
    "groundhog-account-relays|test-groundhog-account-relays"
    "groundhog-account-remote-switch|test-groundhog-account-remote-switch"
    "groundhog-account-store|test-groundhog-account-store"
    "groundhog-account-store-gui|test-groundhog-account-store"
    "groundhog-account-ui|test-groundhog-account-ui"
    "groundhog-attachment-ui|test-groundhog-attachment-ui"
    "groundhog-attachments|test-groundhog-attachments"
    "groundhog-auth-policy|test-groundhog-auth-policy"
    "groundhog-blossom|test-groundhog-blossom"
    "groundhog-composer|test-groundhog-composer"
    "groundhog-contact-directory|test-groundhog-contact-directory"
    "groundhog-conversation-info|test-groundhog-conversation-info"
    "groundhog-conversation-info-gui|test-groundhog-conversation-info"
    "groundhog-conversation-list|test-groundhog-conversation-list"
    "groundhog-conversation-menu|test-groundhog-conversation-menu"
    "groundhog-conversation-menu-gui|test-groundhog-conversation-menu"
    "groundhog-diagnostics|test-groundhog-diagnostics"
    "groundhog-dm-inbox|test-groundhog-dm-inbox"
    "groundhog-dm-send|test-groundhog-dm-send"
    "groundhog-e2e-dm|test-groundhog-e2e-dm"
    "groundhog-expiry|test-groundhog-expiry"
    "groundhog-identity|test-groundhog-identity"
    "groundhog-issue-uploader|test-groundhog-issue-uploader"
    "groundhog-media-cache-binding|test-groundhog-media-cache-binding"
    "groundhog-mls-kp-lifecycle|test-groundhog-mls-kp-lifecycle"
    "groundhog-mls-kp-lifecycle-adopted|test-groundhog-mls-kp-lifecycle-adopted"
    "groundhog-multi-send|test-groundhog-multi-send"
    "groundhog-net|test-groundhog-net"
    "groundhog-new-message|test-groundhog-new-message"
    "groundhog-nip29-group-list|test-groundhog-nip29-group-list"
    "groundhog-nip29-service|test-groundhog-nip29-service"
    "groundhog-nip46-inbox-nak|test-groundhog-nip46-inbox-nak"
    "groundhog-nip46-pair-dialog|test-groundhog-nip46-pair-dialog"
    "groundhog-nip46-pair-nak|test-groundhog-nip46-pair-nak"
    "groundhog-notifier|test-groundhog-notifier"
    "groundhog-notifier-gui|test-groundhog-notifier"
    "groundhog-onboarding|test-groundhog-onboarding"
    "groundhog-outbox|test-groundhog-outbox"
    "groundhog-preferences|test-groundhog-preferences"
    "groundhog-privacy-e2e|test-groundhog-privacy-e2e"
    "groundhog-relay-guard|test-groundhog-relay-guard"
    "groundhog-remote-feature-matrix|test-groundhog-remote-feature-matrix"
    "groundhog-requests|test-groundhog-requests"
    "groundhog-requests-gui|test-groundhog-requests"
    "groundhog-tls-resumption|test-groundhog-tls-resumption")
get_property(_gh_tests DIRECTORY PROPERTY TESTS)
foreach(_test IN LISTS _gh_tests)
  get_property(_described TEST "${_test}" PROPERTY NOSTRC_SELECTION_DESCRIBED)
  if(NOT _described)
    set(_target "")
    foreach(_pair IN LISTS _gh_remaining_targets)
      string(REPLACE "|" ";" _parts "${_pair}")
      list(GET _parts 0 _name)
      if(_test STREQUAL _name)
        list(GET _parts 1 _target)
        break()
      endif()
    endforeach()
    _gh_describe("${_test}" "${_target}" groundhog.unclassified)
  endif()
endforeach()

# Exact registration/input relationships retained from the ab5 production audit.
if(TEST "groundhog-about")
  nostrc_add_test_owners("groundhog-about" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.unclassified nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-about-gui")
  nostrc_add_test_owners("groundhog-about-gui" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.unclassified nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-account-backends")
  nostrc_add_test_owners("groundhog-account-backends" OWNER gnome.seahorse gnostr groundhog.identity_auth groundhog.relay groundhog.resources nip06 nip11 nip46 nip49 nip55l relayd)
endif()
if(TEST "groundhog-account-remote-switch")
  nostrc_add_test_owners("groundhog-account-remote-switch" OWNER gnome.seahorse gnostr groundhog.identity_auth groundhog.relay groundhog.resources nip06 nip11 nip46 nip49 nip55l relayd)
endif()
if(TEST "groundhog-account-store-gui")
  nostrc_add_test_owners("groundhog-account-store-gui" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l nip59 relayd shared.tests)
endif()
if(TEST "groundhog-account-ui")
  nostrc_add_test_owners("groundhog-account-ui" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-agent-events")
  nostrc_add_test_owners("groundhog-agent-events" OWNER groundhog.relay nip06 nip11 nip34 nip46 nip49 nip55l nostr.gtk relayd)
endif()
if(TEST "groundhog-agents-page")
  nostrc_add_test_owners("groundhog-agents-page" OWNER groundhog.relay groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l nostr.gtk relayd)
endif()
if(TEST "groundhog-attachment-ui")
  nostrc_add_test_owners("groundhog-attachment-ui" OWNER gnome.seahorse gnostr groundhog.legacy_harness_inputs groundhog.mls_ui groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-background-desktop")
  nostrc_add_test_owners("groundhog-background-desktop" OWNER groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.shell_ui groundhog.unclassified)
endif()
if(TEST "groundhog-background-gui")
  nostrc_add_test_owners("groundhog-background-gui" OWNER gnome.common gnome.seahorse gnostr groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.shell_ui nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-blueprint")
  nostrc_add_test_owners("groundhog-blueprint" OWNER groundhog.unclassified)
endif()
if(TEST "groundhog-bus-probe")
  nostrc_add_test_owners("groundhog-bus-probe" OWNER gnome.common gnome.seahorse gnostr groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.shell_ui nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-composer")
  nostrc_add_test_owners("groundhog-composer" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l nip59 relayd shared.tests)
endif()
if(TEST "groundhog-conversation-info-gui")
  nostrc_add_test_owners("groundhog-conversation-info-gui" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-conversation-list")
  nostrc_add_test_owners("groundhog-conversation-list" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-conversation-menu-gui")
  nostrc_add_test_owners("groundhog-conversation-menu-gui" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-conversation-view")
  nostrc_add_test_owners("groundhog-conversation-view" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-data")
  nostrc_add_test_owners("groundhog-data" OWNER gnome.common gnome.seahorse groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.shell_ui groundhog.unclassified nip06 nip11 nip34 nip46 nip49 nip55l nostr.gtk relayd)
endif()
if(TEST "groundhog-desktop")
  nostrc_add_test_owners("groundhog-desktop" OWNER groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.shell_ui groundhog.unclassified)
endif()
if(TEST "groundhog-diagnostics")
  nostrc_add_test_owners("groundhog-diagnostics" OWNER gnostr groundhog.resources)
endif()
if(TEST "groundhog-e2e-dm")
  nostrc_add_test_owners("groundhog-e2e-dm" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l nip59 relayd shared.tests)
endif()
if(TEST "groundhog-gdk-frame-tolerance")
  nostrc_add_test_owners("groundhog-gdk-frame-tolerance" OWNER shared.tests)
endif()
if(TEST "groundhog-group-ui-gui")
  nostrc_add_test_owners("groundhog-group-ui-gui" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources groundhog.unclassified nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-i18n")
  nostrc_add_test_owners("groundhog-i18n" OWNER groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.shell_ui groundhog.unclassified)
endif()
if(TEST "groundhog-issue-uploader")
  nostrc_add_test_owners("groundhog-issue-uploader" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd)
endif()
if(TEST "groundhog-launch")
  nostrc_add_test_owners("groundhog-launch" OWNER gnome.common gnome.seahorse gnostr groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.unclassified nip06 nip11 nip34 nip46 nip49 nip55l relayd)
endif()
if(TEST "groundhog-metainfo-merged")
  nostrc_add_test_owners("groundhog-metainfo-merged" OWNER groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.shell_ui groundhog.unclassified)
endif()
if(TEST "groundhog-mls-remote")
  nostrc_add_test_owners("groundhog-mls-remote" OWNER gnome.seahorse gnostr groundhog.resources groundhog.unclassified nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-mls-ui-gui")
  nostrc_add_test_owners("groundhog-mls-ui-gui" OWNER gnome.seahorse gnostr groundhog.resources groundhog.unclassified nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-nip29-group-list")
  nostrc_add_test_owners("groundhog-nip29-group-list" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.group_ui groundhog.net groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-nip46-credentials-list")
  nostrc_add_test_owners("groundhog-nip46-credentials-list" OWNER gnome.seahorse groundhog.identity_auth groundhog.relay nip06 nip11 nip46 nip49 nip55l relayd)
endif()
if(TEST "groundhog-nip46-inbox-nak")
  nostrc_add_test_owners("groundhog-nip46-inbox-nak" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.net groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l nip59 relayd)
endif()
if(TEST "groundhog-nip46-nak")
  nostrc_add_test_owners("groundhog-nip46-nak" OWNER groundhog.identity_auth groundhog.net groundhog.relay nip06 nip11 nip46 nip49 nip55l relayd)
endif()
if(TEST "groundhog-nip46-pair-dialog")
  nostrc_add_test_owners("groundhog-nip46-pair-dialog" OWNER gnome.seahorse gnostr groundhog.identity_auth groundhog.net groundhog.relay groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-nip46-pair-nak")
  nostrc_add_test_owners("groundhog-nip46-pair-nak" OWNER gnome.seahorse gnostr groundhog.identity_auth groundhog.net groundhog.relay groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-nip46-session")
  nostrc_add_test_owners("groundhog-nip46-session" OWNER groundhog.identity_auth groundhog.relay nip06 nip11 nip46 nip49 nip55l relayd)
endif()
if(TEST "groundhog-nip46-wire")
  nostrc_add_test_owners("groundhog-nip46-wire" OWNER groundhog.identity_auth groundhog.net groundhog.relay nip06 nip11 nip46 nip49 nip55l relayd)
endif()
if(TEST "groundhog-notifier-gui")
  nostrc_add_test_owners("groundhog-notifier-gui" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-onboarding")
  nostrc_add_test_owners("groundhog-onboarding" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l nip59 relayd shared.tests)
endif()
if(TEST "groundhog-packaging-version")
  nostrc_add_test_owners("groundhog-packaging-version" OWNER groundhog.unclassified)
endif()
if(TEST "groundhog-poll-ui")
  nostrc_add_test_owners("groundhog-poll-ui" OWNER gnome.seahorse gnostr groundhog.resources groundhog.unclassified nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-portable-singleton")
  nostrc_add_test_owners("groundhog-portable-singleton" OWNER nip06 nip11 nip34 nip46 nip49 nip55l nostr.gtk relayd)
endif()
if(TEST "groundhog-potfiles")
  nostrc_add_test_owners("groundhog-potfiles" OWNER groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.shell_ui)
endif()
if(TEST "groundhog-preferences")
  nostrc_add_test_owners("groundhog-preferences" OWNER gnostr groundhog.relay groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-privacy-static")
  nostrc_add_test_owners("groundhog-privacy-static" OWNER groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.shell_ui)
endif()
if(TEST "groundhog-profile-guard")
  nostrc_add_test_owners("groundhog-profile-guard" OWNER gnome.seahorse groundhog.identity_auth groundhog.relay groundhog.store nip06 nip11 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-public-note-post")
  nostrc_add_test_owners("groundhog-public-note-post" OWNER groundhog.relay groundhog.store nip06 nip11 nip34 nip46 nip49 nip55l nostr.gtk relayd)
endif()
if(TEST "groundhog-remote-feature-matrix")
  nostrc_add_test_owners("groundhog-remote-feature-matrix" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.group_ui groundhog.net groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l nip59 relayd)
endif()
if(TEST "groundhog-requests-gui")
  nostrc_add_test_owners("groundhog-requests-gui" OWNER gnome.seahorse gnostr groundhog.account_relays groundhog.media groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-resource")
  nostrc_add_test_owners("groundhog-resource" OWNER groundhog.relay groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l nostr.gtk relayd)
endif()
if(TEST "groundhog-shell")
  nostrc_add_test_owners("groundhog-shell" OWNER groundhog.relay groundhog.resources nip06 nip11 nip34 nip46 nip49 nip55l nostr.gtk relayd shared.tests)
endif()
if(TEST "groundhog-signer-nip46")
  nostrc_add_test_owners("groundhog-signer-nip46" OWNER gnome.seahorse groundhog.identity_auth groundhog.relay nip06 nip11 nip46 nip49 nip55l relayd)
endif()
if(TEST "groundhog-status-notifier")
  nostrc_add_test_owners("groundhog-status-notifier" OWNER gnome.common shared.tests)
endif()
if(TEST "groundhog-two-instance")
  nostrc_add_test_owners("groundhog-two-instance" OWNER gnome.common gnome.seahorse gnostr groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.shell_ui nip06 nip11 nip34 nip46 nip49 nip55l relayd)
endif()
if(TEST "groundhog-two-instance-remote-acceptance")
  nostrc_add_test_owners("groundhog-two-instance-remote-acceptance" OWNER gnome.common gnome.seahorse gnostr groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.shell_ui groundhog.unclassified nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "groundhog-version")
  nostrc_add_test_owners("groundhog-version" OWNER gnome.common gnome.seahorse groundhog.legacy_harness_inputs groundhog.poll_ui groundhog.unclassified nip06 nip11 nip34 nip46 nip49 nip55l nostr.gtk relayd)
endif()
if(TEST "groundhog-voice-ui")
  nostrc_add_test_owners("groundhog-voice-ui" OWNER gnome.seahorse groundhog.resources groundhog.unclassified nip06 nip11 nip34 nip46 nip49 nip55l nostr.gtk relayd shared.tests)
endif()
