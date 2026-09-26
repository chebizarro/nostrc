# nostr-signer-webext-host — NIP-07 / WebLN native-messaging host (nostrc-jjyp).
#
# Bridges the browser extension in browser-extension/nip07/ to the desktop
# signer daemon org.nostr.Signer (window.nostr) and the wallet agent
# org.nostr.Wallet1 (window.webln) over the session bus. Headless and small
# on purpose: GLib/GIO + json-glib only, no libnostr, no key material.
#
# Reached three ways:
#   * apps/gnostr-signer (BUILD_APPS + BUILD_NATIVE_HOST) for dev builds
#   * top level -DENABLE_NOSTR_SIGNER_WEBEXT_HOST=ON (Debian/RPM packaging,
#     BUILD_APPS=OFF)
#   * standalone: cmake -S apps/gnostr-signer/native-host -B build-nmh
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(nostr_signer_webext_host C)
  include(CTest)
endif()

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NMH_DEPS REQUIRED IMPORTED_TARGET
  glib-2.0>=2.66 gio-2.0 gio-unix-2.0 json-glib-1.0)

set(NMH_VERSION "0.2.0")
set(NMH_BIN "nostr-signer-webext-host")
set(NMH_HOST_NAME "org.nostr.signer_bridge")

# Extension identities the host manifests are pinned to. Firefox: the
# gecko id from browser-extension/nip07/manifest.firefox.json. Chromium: the
# id derived from the "key" in manifest.chromium.json (README "Extension
# ids" explains how to re-derive it).
set(NMH_FIREFOX_EXTENSION_ID "signer-bridge@gnostr.org" CACHE STRING
    "Firefox extension id allowed to launch the NIP-07 host")
set(NMH_CHROMIUM_EXTENSION_ID "ljigikpdhlameofnnalkmjnbeagdhbin" CACHE STRING
    "Chromium extension id allowed to launch the NIP-07 host")

option(NOSTR_SIGNER_WEBEXT_INSTALL_BROWSER_MANIFESTS
  "Install the host manifest into the system-wide Firefox/Chromium/Chrome lookup dirs (packaging)" OFF)
set(NMH_MOZILLA_HOSTS_DIR "/usr/lib/mozilla/native-messaging-hosts" CACHE PATH
    "System Firefox native-messaging-hosts directory")
set(NMH_CHROMIUM_HOSTS_DIR "/etc/chromium/native-messaging-hosts" CACHE PATH
    "System Chromium native-messaging-hosts directory")
set(NMH_CHROME_HOSTS_DIR "/etc/opt/chrome/native-messaging-hosts" CACHE PATH
    "System Google Chrome native-messaging-hosts directory")

set(NMH_WARN -Wall -Wextra -Wformat=2 -Wno-unused-parameter)

add_library(nmh_core STATIC
  native_messaging.c
  nm_errors.c
  nm_policy.c
  nm_router.c
  nm_provider_nip07.c
  nm_provider_webln.c
  nm_webln.c)
target_include_directories(nmh_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(nmh_core PUBLIC c_std_11)
target_compile_options(nmh_core PRIVATE ${NMH_WARN})
target_compile_definitions(nmh_core PUBLIC NM_HOST_VERSION="${NMH_VERSION}")
target_link_libraries(nmh_core PUBLIC PkgConfig::NMH_DEPS)

add_executable(${NMH_BIN} main_native_host.c)
target_compile_options(${NMH_BIN} PRIVATE ${NMH_WARN})
target_link_libraries(${NMH_BIN} PRIVATE nmh_core)

# ---- install ----------------------------------------------------------------
# The browser execs the absolute path from the manifest, so the binary lives
# in libexec: it is not a user command.
install(TARGETS ${NMH_BIN} RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

set(NMH_HOST_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/${NMH_BIN}")
configure_file(manifests/firefox/org.nostr.signer_bridge.json.in
  ${CMAKE_CURRENT_BINARY_DIR}/firefox/${NMH_HOST_NAME}.json @ONLY)
configure_file(manifests/chromium/org.nostr.signer_bridge.json.in
  ${CMAKE_CURRENT_BINARY_DIR}/chromium/${NMH_HOST_NAME}.json @ONLY)

set(NMH_DATADIR ${CMAKE_INSTALL_DATADIR}/nostr-signer-webext-host)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/firefox/${NMH_HOST_NAME}.json
        DESTINATION ${NMH_DATADIR}/firefox)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/chromium/${NMH_HOST_NAME}.json
        DESTINATION ${NMH_DATADIR}/chromium)
install(PROGRAMS install.sh DESTINATION ${NMH_DATADIR} RENAME install-user-manifests.sh)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nostr-signer-webext-host)

if(NOSTR_SIGNER_WEBEXT_INSTALL_BROWSER_MANIFESTS)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/firefox/${NMH_HOST_NAME}.json
          DESTINATION ${NMH_MOZILLA_HOSTS_DIR})
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/chromium/${NMH_HOST_NAME}.json
          DESTINATION ${NMH_CHROMIUM_HOSTS_DIR})
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/chromium/${NMH_HOST_NAME}.json
          DESTINATION ${NMH_CHROME_HOSTS_DIR})
endif()

# ---- tests ------------------------------------------------------------------
if(BUILD_TESTING)
  foreach(t framing policy errors router webln)
    add_executable(test_nmh_${t} tests/test_nm_${t}.c)
    target_compile_options(test_nmh_${t} PRIVATE ${NMH_WARN})
    target_link_libraries(test_nmh_${t} PRIVATE nmh_core)
    add_test(NAME nostr_signer_webext_host_${t} COMMAND test_nmh_${t})
    set_tests_properties(nostr_signer_webext_host_${t} PROPERTIES
      LABELS "nostr-signer-webext-host" TIMEOUT 60)
  endforeach()

  # End-to-end: the real host binary against the real nostr-signer-daemon
  # on a private GTestDBus bus (mirrors nips/nip55l/tests/
  # test_signer_dbus_contract.c). Needs the nip55l GLib daemon target.
  if(TARGET nostr-signer-daemon)
    add_executable(test_nmh_e2e tests/test_nm_e2e.c)
    target_compile_options(test_nmh_e2e PRIVATE ${NMH_WARN})
    target_link_libraries(test_nmh_e2e PRIVATE nmh_core)
    target_compile_definitions(test_nmh_e2e PRIVATE
      "NMH_DAEMON_PATH=\"$<TARGET_FILE:nostr-signer-daemon>\""
      "NMH_HOST_PATH=\"$<TARGET_FILE:${NMH_BIN}>\"")
    add_dependencies(test_nmh_e2e nostr-signer-daemon ${NMH_BIN})
    add_test(NAME nostr_signer_webext_host_e2e COMMAND test_nmh_e2e)
    set_tests_properties(nostr_signer_webext_host_e2e PROPERTIES
      LABELS "nostr-signer-webext-host;dbus" TIMEOUT 120)
  endif()

  # WebLN end-to-end: the real host against the real nostr-wallet-agent on a
  # private GTestDBus bus (headless, ephemeral pairing store). The agent is
  # told to trust the build-tree host via its test-build-only
  # NOSTR_WALLET_AGENT_ORIGIN_BRIDGES override (defaults to BUILD_TESTING,
  # and is defined later: the agent directory is added after this one).
  if(ENABLE_NOSTR_WALLET_AGENT AND
     (NOT DEFINED NOSTR_WALLET_AGENT_ORIGIN_BRIDGE_ENV OR NOSTR_WALLET_AGENT_ORIGIN_BRIDGE_ENV))
    add_executable(test_nmh_webln_e2e tests/test_nm_webln_e2e.c)
    target_compile_options(test_nmh_webln_e2e PRIVATE ${NMH_WARN})
    target_link_libraries(test_nmh_webln_e2e PRIVATE nmh_core)
    target_compile_definitions(test_nmh_webln_e2e PRIVATE
      "NMH_AGENT_PATH=\"$<TARGET_FILE:nostr-wallet-agent>\""
      "NMH_HOST_PATH=\"$<TARGET_FILE:${NMH_BIN}>\"")
    add_dependencies(test_nmh_webln_e2e nostr-wallet-agent ${NMH_BIN})
    add_test(NAME nostr_signer_webext_host_webln_e2e COMMAND test_nmh_webln_e2e)
    set_tests_properties(nostr_signer_webext_host_webln_e2e PROPERTIES
      LABELS "nostr-signer-webext-host;dbus" TIMEOUT 120)
  endif()

  # Extension-side policy (origin derivation, event validation, permission
  # categories) — plain Node, no browser.
  find_program(NMH_NODE node)
  set(NMH_EXT_TEST ${CMAKE_CURRENT_SOURCE_DIR}/../../../browser-extension/nip07/tests/policy.test.js)
  if(NMH_NODE AND EXISTS ${NMH_EXT_TEST})
    add_test(NAME nostr_signer_webext_extension_policy COMMAND ${NMH_NODE} ${NMH_EXT_TEST})
    set_tests_properties(nostr_signer_webext_extension_policy PROPERTIES
      LABELS "nostr-signer-webext-host" TIMEOUT 60)
    add_test(NAME nostr_signer_webext_extension_webln
             COMMAND ${NMH_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/../../../browser-extension/nip07/tests/webln.test.js)
    set_tests_properties(nostr_signer_webext_extension_webln PROPERTIES
      LABELS "nostr-signer-webext-host" TIMEOUT 60)
  endif()
endif()
