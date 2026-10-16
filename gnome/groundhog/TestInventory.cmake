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

# Legacy directly compiles gh-mls-copy.c; adopted links the shipped MLS UI.
# Do not refine away those actual inputs (nor its late voice linkage).
_gh_describe(groundhog-mdk-interop test-groundhog-mdk-interop groundhog.mls_service
  INPUTS "${CMAKE_SOURCE_DIR}/tests/interop/mdk/driver" REQUIRES docker private_bus)
foreach(_case control groundhog-invites-mdk mdk-invites-groundhog
    mdk-invites-groundhog-gui adopted-welcome white-noise-welcome white-noise-media
    media-policy white-noise-voice white-noise-dm adopted-commits routing-rotation
    concurrent-commits polls white-noise-reactions)
  set(_requires docker private_bus)
  if(_case STREQUAL "mdk-invites-groundhog-gui")
    list(APPEND _requires display)
  endif()
  _gh_describe("groundhog-mdk011-interop-${_case}" test-groundhog-mdk011-interop
    groundhog.mls_service INPUTS "${CMAKE_SOURCE_DIR}/tests/interop/mdk/driver-0.11"
    REQUIRES ${_requires})
endforeach()
_gh_describe(groundhog-mdk011-interop-mdk09-probe test-groundhog-mdk011-interop
  groundhog.mls_service INPUTS "${CMAKE_SOURCE_DIR}/tests/interop/mdk/driver-0.11"
    "${CMAKE_SOURCE_DIR}/tests/interop/mdk/driver-0.9-probe"
  REQUIRES docker private_bus EXPECTED mdk09_incompatibility)
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
