# nostr-seal — seal files for Nostr public keys or a passphrase (nostrc-da9c).
#
# Separate user-facing binary: it never links into nostr-authd / pam_nostr.so
# (scripts/check-authd-dep-purity.sh stays green), and it does not need the
# nostr-homed tree to be configured.
#
# Primitive reuse, not re-implementation:
#   * porthome D4 container: nostrc::porthome owns the portable crypto.
#     No copied sibling source, libhanami, sandbox or provisioner is linked.
#   * NIP-44: the nostr_nip44_core static target (convkey, v2 cipher, HKDF).
#   * NIP-49: its TUs are compiled into a private static lib (the in-tree
#     nip49 shared library is unversioned and not shipped). NIP-19 links the
#     shared libnip19.so.0 from libnostr-nips0 (nostrc-9rvm).
#   * --publish (nostrc-hby8) reuses nostr-share's upload path
#     (nostr-share-core: NIP-65/BUD-03 discovery, libnostr-publish) with
#     GnBlossomClient + BUD-02 when ENABLE_NOSTR_SHARE is on; otherwise the
#     command reports that it was built without publishing.
cmake_minimum_required(VERSION 3.20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(NSEAL_GIO REQUIRED IMPORTED_TARGET glib-2.0>=2.74 gio-2.0>=2.74)
find_package(OpenSSL REQUIRED)
find_library(NSEAL_SODIUM_LIB sodium REQUIRED)
find_path(NSEAL_SODIUM_INC sodium.h)
foreach(_t nostr nostr_nip44_core)
  if(NOT TARGET ${_t})
    message(FATAL_ERROR "nostr-seal needs the in-tree target ${_t}")
  endif()
endforeach()

set(_ns_root ${CMAKE_CURRENT_SOURCE_DIR}/../..)
if(NOT TARGET nostrc::porthome)
  message(FATAL_ERROR "nostr-seal requires BUILD_LIBPORTHOME=ON or ENABLE_NOSTR_SEAL=OFF.")
endif()

if(NOT TARGET nip19)
  message(FATAL_ERROR "nostr-seal requires ENABLE_NIP19=ON")
endif()
add_library(nostr_seal_nips STATIC
  ${_ns_root}/nips/nip49/src/core/nip49.c
  ${_ns_root}/nips/nip49/src/core/nip49_kdf.c
  ${_ns_root}/nips/nip49/src/core/nip49_aead.c
  ${_ns_root}/nips/nip49/src/core/nip49_bech.c)
target_include_directories(nostr_seal_nips PUBLIC
  ${_ns_root}/nips/nip49/include)
if(NSEAL_SODIUM_INC)
  target_include_directories(nostr_seal_nips PUBLIC ${NSEAL_SODIUM_INC})
endif()
target_link_libraries(nostr_seal_nips PUBLIC nip19 nostr ${NSEAL_SODIUM_LIB})
set_property(TARGET nostr_seal_nips PROPERTY POSITION_INDEPENDENT_CODE ON)

add_library(nostr_seal_core STATIC
  src/nseal.c
  src/nseal-output.c
  src/nseal-signer.c
  src/nseal-config.c
  src/nseal-publish.c)
target_include_directories(nostr_seal_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(nostr_seal_core PRIVATE -Wall -Wextra -Werror)
target_link_libraries(nostr_seal_core PUBLIC
  nostr_seal_nips nostr_nip44_core nostrc::porthome nostr OpenSSL::Crypto PkgConfig::NSEAL_GIO)
set_property(TARGET nostr_seal_core PROPERTY POSITION_INDEPENDENT_CODE ON)
# Target names resolve at generate time, so nostr-share (added after this
# directory) can still provide nostr-share-core here.
if(ENABLE_NOSTR_SHARE)
  target_compile_definitions(nostr_seal_core PUBLIC NSEAL_HAVE_PUBLISH=1)
  target_link_libraries(nostr_seal_core PUBLIC nostr-share-core nostr_gobject nostrc::bud02)
  message(STATUS "nostr-seal: --publish enabled (nostr-share-core)")
else()
  message(STATUS "nostr-seal: --publish disabled (needs ENABLE_NOSTR_SHARE)")
endif()

add_executable(nostr-seal src/main.c)
target_compile_options(nostr-seal PRIVATE -Wall -Wextra -Werror)
target_link_libraries(nostr-seal PRIVATE nostr_seal_core)

include(GNUInstallDirs)
install(TARGETS nostr-seal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/nostr-seal.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
install(FILES README.md data/seal.conf.example DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nostr-seal)

# Optional libadwaita front-end: the double-click handler for
# application/vnd.nostr.sealed. The .desktop file only ships with it.
option(NOSTR_SEAL_ENABLE_GTK "Build nostr-seal-gtk (libadwaita) when available" ON)
if(NOSTR_SEAL_ENABLE_GTK)
  pkg_check_modules(NSEAL_ADW QUIET IMPORTED_TARGET libadwaita-1>=1.5 gtk4>=4.12)
endif()
if(NOSTR_SEAL_ENABLE_GTK AND NSEAL_ADW_FOUND)
  add_executable(nostr-seal-gtk src/gtk-main.c)
  target_compile_options(nostr-seal-gtk PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(nostr-seal-gtk PRIVATE nostr_seal_core PkgConfig::NSEAL_ADW)
  install(TARGETS nostr-seal-gtk RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES data/org.nostr.Seal.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  message(STATUS "nostr-seal: libadwaita found — building nostr-seal-gtk")
else()
  message(STATUS "nostr-seal: CLI only (no libadwaita >= 1.5)")
endif()

if(BUILD_TESTING)
  add_executable(test-nostr-seal tests/test-nseal.c)
  target_compile_options(test-nostr-seal PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(test-nostr-seal PRIVATE nostr_seal_core)
  add_test(NAME nostr_seal_unit COMMAND test-nostr-seal)

  add_executable(test-nostr-seal-publish tests/test-nseal-publish.c)
  target_compile_options(test-nostr-seal-publish PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(test-nostr-seal-publish PRIVATE nostr_seal_core)
  target_compile_definitions(test-nostr-seal-publish PRIVATE
    "NSEAL_CLI_PATH=\"$<TARGET_FILE:nostr-seal>\"")
  add_dependencies(test-nostr-seal-publish nostr-seal)
  if(ENABLE_NOSTR_SHARE)
    target_sources(test-nostr-seal-publish PRIVATE tests/test-nseal-upload.c)
  endif()
  add_test(NAME nostr_seal_publish COMMAND test-nostr-seal-publish)
  set_tests_properties(nostr_seal_publish PROPERTIES LABELS "nostr-seal" TIMEOUT 60)

  # ~/.config/nostr/seal.conf defaults (written by org.nostr.Settings).
  add_executable(test-nostr-seal-config tests/test-nseal-config.c)
  target_compile_options(test-nostr-seal-config PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(test-nostr-seal-config PRIVATE nostr_seal_core)
  target_compile_definitions(test-nostr-seal-config PRIVATE
    "NSEAL_CLI_PATH=\"$<TARGET_FILE:nostr-seal>\"")
  add_test(NAME nostr_seal_config COMMAND test-nostr-seal-config)
  set_tests_properties(nostr_seal_config PROPERTIES LABELS "nostr-seal" TIMEOUT 60)
  set_tests_properties(nostr_seal_unit PROPERTIES LABELS "nostr-seal" TIMEOUT 120)

  # Multi-GB streaming on a sparse file (bytes: NSEAL_TEST_BIG_BYTES).
  add_test(NAME nostr_seal_big COMMAND test-nostr-seal --big)
  set_tests_properties(nostr_seal_big PROPERTIES LABELS "nostr-seal;slow" TIMEOUT 1200)

  # CLI ↔ real nostr-signer-daemon, binary-safe nip44_decrypt only.
  if(TARGET nostr-signer-daemon)
    add_executable(test-nostr-seal-signer tests/test-nseal-signer.c)
    target_compile_options(test-nostr-seal-signer PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test-nostr-seal-signer PRIVATE nostr_seal_core nostr_nip55l_glib nostrc-test-bus)
    target_include_directories(test-nostr-seal-signer PRIVATE ${CMAKE_SOURCE_DIR}/nips/nip55l/src/glib)
    target_compile_definitions(test-nostr-seal-signer PRIVATE
      "NSEAL_DAEMON_PATH=\"$<TARGET_FILE:nostr-signer-daemon>\""
      "NSEAL_CLI_PATH=\"$<TARGET_FILE:nostr-seal>\"")
    add_dependencies(test-nostr-seal-signer nostr-signer-daemon nostr-seal)
    add_test(NAME nostr_seal_signer COMMAND test-nostr-seal-signer)
    set_tests_properties(nostr_seal_signer PROPERTIES LABELS "nostr-seal;dbus;integration" TIMEOUT 180)
  endif()
endif()
