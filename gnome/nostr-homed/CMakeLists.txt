cmake_minimum_required(VERSION 3.18)
project(nostr_homed VERSION 0.2.0 LANGUAGES C)

include(GNUInstallDirs)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# -std=c11 is strict ISO; glibc then hides POSIX/BSD functions (strdup, mkstemp, ...)
# unless a feature-test macro is set. Without it they become implicit int, truncating
# returned 64-bit pointers (segfaults on Linux; masked on macOS). _DEFAULT_SOURCE
# enables the POSIX.1-2008 + BSD set (covers both strdup and mkstemp).
add_compile_definitions(_DEFAULT_SOURCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(NOSTR_HOMED_ENABLE_AUTH_CORE
  "Build non-installed authentication protocol/provider core and tests" OFF)
option(NOSTR_HOMED_ENABLE_IDENTITY_CORE
  "Build non-installed identity authority/reader core and tests" OFF)
option(NOSTR_HOMED_ENABLE_NSS
  "Build and install the read-only libnss_nostr NSS module (requires identity core)" OFF)
option(NOSTR_HOMED_ENABLE_AUTH_RUNTIME
  "Build the auth broker daemon/client transport nostr-authd (requires auth+identity cores)" OFF)
option(NOSTR_HOMED_ENABLE_PAM
  "Build the broker-backed pam_nostr.so PAM module (requires auth runtime + libpam)" OFF)
option(NOSTR_HOMED_ENABLE_DOMAIN_CONFIG
  "Install inert winbind/domain examples and validator (never activates PAM)" OFF)
option(NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING
  "Build the legacy FUSE/Blossom roaming-home experiment" OFF)
option(NOSTR_HOMED_ENABLE_SMB
  "Build the standalone SMB credential authority core (D5)" OFF)
option(NOSTR_HOMED_ENABLE_AUTH_INSTALL
  "Install the system broker/PAM authentication product" OFF)
# D-11 / B5: headless identity-authority admin CLI (nostr-homectl) — a
# GLib-free / FUSE-free build decoupled from the experimental FUSE/D-Bus
# roaming stack. Independent of NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING;
# both flags produce a `nostr-homectl` binary and are mutually exclusive.
option(NOSTR_HOMED_ENABLE_CTL
  "Build the headless identity-authority nostr-homectl (requires identity core)" OFF)
# Greeter-side gnome-shell extension that renders the pending NIP-46 QR pairing
# in the GDM login dialog.  Purely display code (JS + CSS + metadata); no C
# artefacts.  Independent of every other option here so packagers can ship the
# extension without pulling in the auth runtime.
option(NOSTR_HOMED_ENABLE_GREETER_EXTENSION
  "Install the gnome-shell greeter extension nostr-login-qr@nostrc" OFF)
# B5-profile: Nostr kind-0 → AccountsService (avatar + real name at GDM).
# The seeders and broker post-login hook link the profile library; the
# unprivileged image-downloader helper is a separate installable binary.
option(NOSTR_HOMED_ENABLE_PROFILE
  "Build the Nostr kind-0 → AccountsService avatar/name bridge (requires auth runtime + gdk-pixbuf + libcurl + gio-2.0)" OFF)
option(NOSTR_HOMED_BUILD_TESTS "Build tests for enabled nostr-homed features" ON)
# Phase-1 primitives for the portable-home epic (nostrc-h10m). Ships a
# STANDALONE library `libnostr_porthome.a` (crypto + manifest + Blossom
# wrapper) NOT linked into the base nostr-login stack. Requires libhanami
# and OpenSSL. Every public header is loudly marked EXPERIMENTAL — see
# docs/designs/porthome-crypto-spec.md.
option(NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL
  "Build the experimental portable-home primitives library (requires libhanami)" OFF)
# Phase-3 (bead nostrc-p6qp). Ships the user-scoped sync daemon
# nostr-home-syncd + a small libnostr_syncd_core the unit + integration
# tests link against. Gated behind its own option ON TOP OF the
# porthome-experimental gate so a base build never links a daemon that
# depends on libnostr / libhanami into pam_nostr / nostr-authd. Nothing
# built here installs — packaging + systemd unit are I3.
option(NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL
  "Build the experimental user-scoped nostr-home-syncd daemon (requires porthome experimental + libnostr)" OFF)
# Phase 4 P4-I (bead nostrc-1u55). Ships the read-only FUSE 3 overlay
# nostr-home-fuse. Gated on top of SYNCD_EXPERIMENTAL (which is gated
# on top of PORTHOME_EXPERIMENTAL) and requires FUSE3. Nothing is
# built when OFF — the base pam_nostr / nostr-authd closure never
# gains a libfuse3 or nh_fuse_* dependency.
option(NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL
  "Build the experimental read-only portable-home FUSE overlay nostr-home-fuse (requires SYNCD_EXPERIMENTAL + libfuse3)" OFF)
# Phase 5 I1 (bead nostrc-h10m.1). Panel indicator that renders the
# portable-home status file in the user session. Purely data-driven
# (JS + CSS + metadata); ships nothing that runs C. Independent of
# every runtime option — packagers can install the extension without
# pulling any auth/porthome C code.
option(NOSTR_HOMED_ENABLE_PORTHOME_STATUS_EXTENSION
  "Install the user-session gnome-shell extension nostr-home-status@nostrc" OFF)

if(NOSTR_HOMED_ENABLE_CTL AND NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING)
  message(FATAL_ERROR
    "NOSTR_HOMED_ENABLE_CTL and NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING both "
    "define a `nostr-homectl` target. Enable one and only one — CTL for the "
    "headless identity-authority admin CLI, EXPERIMENTAL_ROAMING for the "
    "FUSE/D-Bus roaming experiment.")
endif()
if(NOSTR_HOMED_ENABLE_CTL AND NOT NOSTR_HOMED_ENABLE_IDENTITY_CORE)
  message(FATAL_ERROR
    "NOSTR_HOMED_ENABLE_CTL requires NOSTR_HOMED_ENABLE_IDENTITY_CORE")
endif()

# Installing the login runtime requires the broker/PAM/NSS pieces to be built.
# Fail loudly rather than silently omitting authentication.
if(NOSTR_HOMED_ENABLE_AUTH_INSTALL AND NOT (NOSTR_HOMED_ENABLE_AUTH_RUNTIME
    AND NOSTR_HOMED_ENABLE_PAM AND NOSTR_HOMED_ENABLE_NSS))
  message(FATAL_ERROR
    "NOSTR_HOMED_ENABLE_AUTH_INSTALL installs the broker/PAM/NSS login runtime "
    "and requires NOSTR_HOMED_ENABLE_AUTH_RUNTIME, NOSTR_HOMED_ENABLE_PAM and "
    "NOSTR_HOMED_ENABLE_NSS to be ON.")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)

if(NOSTR_HOMED_ENABLE_AUTH_CORE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)
  find_package(OpenSSL REQUIRED)
  if(TARGET libnostr)
    set(_homed_nostr libnostr)
  else()
    pkg_check_modules(NOSTR REQUIRED IMPORTED_TARGET nostr)
    pkg_check_modules(LIBGO REQUIRED IMPORTED_TARGET libgo)
    set(_homed_nostr PkgConfig::NOSTR PkgConfig::LIBGO)
  endif()

  # auth_ratelimit's optional persistent backing uses SQLite3. Ensure the
  # imported target exists in this scope (find_package creates a
  # directory-scoped imported target — another subdirectory may have found
  # SQLite3 first, leaving SQLite3::SQLite3 invisible here; synthesize it).
  find_package(SQLite3 REQUIRED)
  if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 INTERFACE IMPORTED)
    target_include_directories(SQLite3::SQLite3 INTERFACE ${SQLite3_INCLUDE_DIRS})
    target_link_libraries(SQLite3::SQLite3 INTERFACE ${SQLite3_LIBRARIES})
  endif()

  add_library(nostr_auth_core STATIC
    src/auth/auth_protocol.c
    src/auth/auth_transaction.c
    src/auth/auth_challenge.c
    src/auth/auth_ratelimit.c
    src/auth/auth_vault.c
    src/auth/auth_worker.c
    src/auth/provider_local.c
    src/auth/provider_nip46.c
    src/auth/provider_nip46_verify.c
    src/auth/provider_nip46_qr.c)
  target_compile_options(nostr_auth_core PRIVATE -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr_auth_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auth)
  target_link_libraries(nostr_auth_core PUBLIC
    ${_homed_nostr} PkgConfig::JANSSON OpenSSL::Crypto SQLite3::SQLite3)
  # The NIP-46 external-signer provider consumes the in-tree nip46 client. In
  # an in-tree build (with ENABLE_NIP46=ON, the default) the target already
  # exists; in a stand-alone build fall back to pkg-config.
  if(TARGET nostr_nip46_core)
    target_link_libraries(nostr_auth_core PUBLIC nostr_nip46_core)
  else()
    pkg_check_modules(NOSTR_NIP46 REQUIRED IMPORTED_TARGET nostr-nip46)
    target_link_libraries(nostr_auth_core PUBLIC PkgConfig::NOSTR_NIP46)
  endif()

  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_auth_protocol tests/unit/test_auth_protocol.c)
    target_link_libraries(test_auth_protocol PRIVATE nostr_auth_core)
    add_executable(test_auth_transaction tests/unit/test_auth_transaction.c)
    target_link_libraries(test_auth_transaction PRIVATE nostr_auth_core)
    add_executable(test_auth_vault tests/unit/test_auth_vault.c)
    target_link_libraries(test_auth_vault PRIVATE nostr_auth_core)
    add_executable(test_auth_challenge tests/unit/test_auth_challenge.c)
    target_link_libraries(test_auth_challenge PRIVATE nostr_auth_core)
    add_executable(test_provider_local tests/integration/test_provider_local.c)
    target_link_libraries(test_provider_local PRIVATE nostr_auth_core)
    add_executable(test_ratelimit_persist tests/integration/test_ratelimit_persist.c)
    target_link_libraries(test_ratelimit_persist PRIVATE nostr_auth_core)
    add_executable(test_auth_worker tests/integration/test_auth_worker.c)
    target_link_libraries(test_auth_worker PRIVATE nostr_auth_core)

    add_test(NAME homed_auth_protocol COMMAND test_auth_protocol)
    add_test(NAME homed_auth_transaction COMMAND test_auth_transaction)
    add_test(NAME homed_auth_vault COMMAND test_auth_vault)
    add_test(NAME homed_auth_challenge COMMAND test_auth_challenge)
    add_test(NAME homed_provider_local COMMAND test_provider_local)
    add_test(NAME homed_ratelimit_persist COMMAND test_ratelimit_persist)
    add_test(NAME homed_auth_worker COMMAND test_auth_worker)
    set_tests_properties(
      homed_auth_protocol homed_auth_transaction homed_auth_vault homed_auth_challenge homed_provider_local homed_ratelimit_persist homed_auth_worker
      PROPERTIES LABELS "nostr-homed;portable;auth" TIMEOUT 60)

    # Live NIP-46 external-signer proof (nostrc-ot2c C6/C7). Provider-level;
    # dials the live bunker + relay when NH_NIP46_LIVE=1 is set at run time,
    # otherwise prints SKIP and returns 0. Registered under `live;manual;nip46`
    # labels so the default `ctest` run does not pick it up; invoke
    # explicitly with `ctest -L live` (or run the binary directly) after
    # ensuring outbound access to wss://bunker.sharegap.net and
    # wss://relay.sharegap.net and an interlocutor config on disk.
    add_executable(test_broker_login_nip46_live
      tests/integration/test_broker_login_nip46_live.c)
    if(TARGET nostr_nip46_core)
      target_link_libraries(test_broker_login_nip46_live PRIVATE
        nostr_auth_core nostr_nip46_core)
    else()
      target_link_libraries(test_broker_login_nip46_live PRIVATE
        nostr_auth_core PkgConfig::NOSTR_NIP46)
    endif()
    add_test(NAME homed_broker_login_nip46_live
      COMMAND test_broker_login_nip46_live)
    set_tests_properties(homed_broker_login_nip46_live
      PROPERTIES LABELS "nostr-homed;live;manual;nip46" TIMEOUT 180)
  endif()
endif()

if(NOSTR_HOMED_ENABLE_IDENTITY_CORE)
  find_package(SQLite3 REQUIRED)
  # find_package(SQLite3) creates a directory-scoped imported target; in an
  # in-tree/root build another subdirectory may have found SQLite3 first, leaving
  # SQLite3::SQLite3 invisible here. Synthesize it from the cached result if so.
  if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 INTERFACE IMPORTED)
    target_include_directories(SQLite3::SQLite3 INTERFACE ${SQLite3_INCLUDE_DIRS})
    target_link_libraries(SQLite3::SQLite3 INTERFACE ${SQLite3_LIBRARIES})
  endif()
  find_package(OpenSSL REQUIRED)

  add_library(nostr_identity_core STATIC
    src/identity/identity_common.c
    src/identity/identity_config.c
    src/identity/identity_store.c
    src/identity/identity_reader.c
    src/identity/identity_projection.c
    src/identity/identity_home.c)
  target_compile_options(nostr_identity_core PRIVATE -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr_identity_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/identity)
  target_link_libraries(nostr_identity_core PUBLIC SQLite3::SQLite3 OpenSSL::Crypto)

  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_identity_store tests/identity/test_identity_store.c)
    target_link_libraries(test_identity_store PRIVATE nostr_identity_core)
    add_executable(test_identity_config tests/identity/test_identity_config.c)
    target_link_libraries(test_identity_config PRIVATE nostr_identity_core)
    add_executable(test_identity_fixtures tests/identity/test_identity_fixtures.c)
    target_compile_definitions(test_identity_fixtures PRIVATE
      IDENTITY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/identity/fixtures")
    target_link_libraries(test_identity_fixtures PRIVATE nostr_identity_core)
    add_executable(test_two_accounts_seed tests/identity/test_two_accounts_seed.c)
    target_link_libraries(test_two_accounts_seed PRIVATE nostr_identity_core)
    add_executable(test_reader_negative tests/identity/test_reader_negative.c)
    target_link_libraries(test_reader_negative PRIVATE nostr_identity_core)
    add_test(NAME homed_identity_store COMMAND test_identity_store)
    add_test(NAME homed_identity_config COMMAND test_identity_config)
    add_test(NAME homed_identity_fixtures COMMAND test_identity_fixtures)
    add_test(NAME homed_two_accounts_seed COMMAND test_two_accounts_seed)
    add_test(NAME homed_reader_negative COMMAND test_reader_negative)
    set_tests_properties(homed_identity_store homed_identity_config homed_identity_fixtures homed_two_accounts_seed homed_reader_negative
      PROPERTIES LABELS "nostr-homed;portable;identity" TIMEOUT 60)
  endif()
endif()

if(NOSTR_HOMED_ENABLE_AUTH_CORE AND NOSTR_HOMED_ENABLE_IDENTITY_CORE)
  target_sources(nostr_auth_core PRIVATE src/auth/auth_transaction_identity.c)
  target_link_libraries(nostr_auth_core PUBLIC nostr_identity_core)
  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_auth_transaction_identity tests/integration/test_auth_transaction_identity.c)
    target_link_libraries(test_auth_transaction_identity PRIVATE nostr_auth_core)
    add_test(NAME homed_auth_transaction_identity COMMAND test_auth_transaction_identity)
    set_tests_properties(homed_auth_transaction_identity
      PROPERTIES LABELS "nostr-homed;portable;auth;identity" TIMEOUT 60)
  endif()
endif()

# A-owned NSS runtime: the read-only glibc module that projects the identity
# store's SQLite snapshot into passwd/group. Installs libnss_nostr.so.2 into the
# system library path plus the nss_nostr.conf sample. Requires the identity core.
if(NOSTR_HOMED_ENABLE_NSS)
  if(NOT NOSTR_HOMED_ENABLE_IDENTITY_CORE)
    message(FATAL_ERROR
      "NOSTR_HOMED_ENABLE_NSS requires NOSTR_HOMED_ENABLE_IDENTITY_CORE")
  endif()
  add_library(nss_nostr MODULE src/nss/nss_nostr.c)
  # glibc dlopens the module by the exact filename libnss_<service>.so.2.
  set_target_properties(nss_nostr PROPERTIES
    PREFIX "libnss_" OUTPUT_NAME "nostr" SUFFIX ".so.2")
  target_link_libraries(nss_nostr PRIVATE nostr_identity_core)
  # glibc's NSS loader dlopen()s libnss_nostr.so.2 by bare name and searches
  # ONLY the standard runtime library path -- on multiarch Debian/Ubuntu that is
  # /usr/lib/<triplet> (e.g. /usr/lib/x86_64-linux-gnu), NOT the PAM security/
  # subdir and NOT /usr/local/lib.  GNUInstallDirs resolves CMAKE_INSTALL_LIBDIR
  # to the multiarch libdir only when CMAKE_INSTALL_PREFIX is /usr, so a release
  # install MUST configure with -DCMAKE_INSTALL_PREFIX=/usr.  Warn loudly if the
  # resolved path is one glibc will not search, so a mis-set prefix fails visibly
  # at configure time instead of silently producing an NSS module getent ignores.
  install(TARGETS nss_nostr LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  message(STATUS
    "nostr-homed: NSS module installs to ${CMAKE_INSTALL_FULL_LIBDIR}/libnss_nostr.so.2")
  if(NOT CMAKE_INSTALL_FULL_LIBDIR MATCHES "^/(usr/)?lib")
    message(WARNING
      "nostr-homed: CMAKE_INSTALL_FULL_LIBDIR=${CMAKE_INSTALL_FULL_LIBDIR} is not a "
      "path glibc's NSS loader searches; libnss_nostr.so.2 will be installed where "
      "getent cannot find it. Configure with -DCMAKE_INSTALL_PREFIX=/usr for a real "
      "login install.")
  endif()
  # B7 (nostrc-rb0e.3 / packaging-plan §6.6): ship the real /etc/nss_nostr.conf
  # as a distro conffile so libnss_nostr.so.2 works out of the box. Debian/Fedora
  # packaging MUST mark this as %config(noreplace) / debian conffile so admin
  # edits survive upgrades — the CMake install(FILES) only stages the file into
  # DESTDIR; conffile semantics belong to the distro packaging layer.
  # DESTINATION uses CMAKE_INSTALL_FULL_SYSCONFDIR so the /usr -> /etc
  # special case in GNUInstallDirs resolves correctly (a relative
  # CMAKE_INSTALL_SYSCONFDIR of "etc" would land under /usr/etc when the
  # prefix is /usr).
  install(FILES config/nss_nostr.conf
    DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}")
  # Documentation sibling: the legacy roaming-cache key=value schema
  # (db_path/uid_base/uid_range). NOT the NSS module conffile — ships under
  # /usr/share/doc-adjacent so operators can rebuild the roaming experiment.
  install(FILES config/nostr-homed-cache.legacy.conf.sample
    DESTINATION "${CMAKE_INSTALL_DATADIR}/nostr-homed")
endif()

# B-owned auth runtime (stage 1): SOCK_SEQPACKET transport, SO_PEERCRED peer
# snapshot, ACL enforcement, and a broker connection handler serving
# CHECK_ACCOUNT against the identity authority. Not installed yet; the login
# proof flow, systemd units and AUTH_INSTALL wiring arrive in later stages.
if(NOSTR_HOMED_ENABLE_AUTH_RUNTIME)
  if(NOT (NOSTR_HOMED_ENABLE_AUTH_CORE AND NOSTR_HOMED_ENABLE_IDENTITY_CORE))
    message(FATAL_ERROR
      "NOSTR_HOMED_ENABLE_AUTH_RUNTIME requires NOSTR_HOMED_ENABLE_AUTH_CORE and NOSTR_HOMED_ENABLE_IDENTITY_CORE")
  endif()
  # Vendored Nayuki qrcodegen (MIT) + our thin half-block / PNG wrapper.
  # Linked into the runtime because the greeter-artifact writer
  # (nh_broker_greeter_artifact_write) produces the sibling PNG alongside
  # current.json in a single publish. See the producer notes in
  # gnome/nostr-homed/greeter-extension/README.md — the extension expects
  # both files present. This relaxes design decision D3 in exchange for a
  # self-contained artifact and a testable "png next to json" invariant.
  # The library is ~1 kLoC of pure arithmetic on URIs we generated
  # ourselves; the blast radius against sshd/login (which dlopen
  # pam_nostr) is unchanged because the shared code path lives in the
  # broker daemon, not the PAM module.
  add_library(nostr_qrcodegen STATIC
    third_party/qrcodegen/qrcodegen.c)
  target_include_directories(nostr_qrcodegen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen)
  target_compile_options(nostr_qrcodegen PRIVATE
    -Wall -Wno-error -Wno-misleading-indentation)
  set_property(TARGET nostr_qrcodegen PROPERTY POSITION_INDEPENDENT_CODE ON)

  add_library(nostr_auth_runtime STATIC
    src/auth/auth_peer.c src/auth/auth_client.c src/auth/auth_broker.c
    src/auth/auth_conf.c src/pam/pam_qr_render.c
    src/nip05/nip05_validate.c src/nip05/nip05_client.c
    src/nip05/nip05_cache.c
    # Portable-home Phase 2 broker glue (bead nostrc-89rj).
    # ALWAYS compiled — the #else branch inside supplies stubs
    # so provider_local.c can weak-link the wrap-seed cache
    # without pulling in libnostr_porthome. NH_AUTH_BROKER_ENABLE_PORTHOME
    # is defined below (only when the experimental option is on).
    src/auth/auth_porthome.c)
  target_include_directories(nostr_auth_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pam)
  target_compile_options(nostr_auth_runtime PRIVATE -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr_auth_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/auth)
  target_link_libraries(nostr_auth_runtime PUBLIC nostr_auth_core nostr_identity_core nostr_qrcodegen)
  add_executable(nostr-authd src/auth/nostr-authd.c)
  target_compile_options(nostr-authd PRIVATE -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_link_libraries(nostr-authd PRIVATE nostr_auth_runtime)
  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_broker_checkaccount tests/integration/test_broker_checkaccount.c)
    target_link_libraries(test_broker_checkaccount PRIVATE nostr_auth_runtime)
    add_test(NAME homed_broker_checkaccount COMMAND test_broker_checkaccount)
    set_tests_properties(homed_broker_checkaccount
      PROPERTIES LABELS "nostr-homed;auth-runtime" TIMEOUT 60)
    add_executable(test_broker_login tests/integration/test_broker_login.c)
    target_link_libraries(test_broker_login PRIVATE nostr_auth_runtime)
    add_test(NAME homed_broker_login COMMAND test_broker_login)
    set_tests_properties(homed_broker_login
      PROPERTIES LABELS "nostr-homed;auth-runtime" TIMEOUT 120)
    add_executable(test_broker_login_providers tests/integration/test_broker_login_providers.c)
    target_link_libraries(test_broker_login_providers PRIVATE nostr_auth_runtime)
    add_test(NAME homed_broker_login_providers COMMAND test_broker_login_providers)
    set_tests_properties(homed_broker_login_providers
      PROPERTIES LABELS "nostr-homed;auth-runtime" TIMEOUT 120)
    add_executable(test_broker_hardening tests/integration/test_broker_hardening.c)
    target_link_libraries(test_broker_hardening PRIVATE nostr_auth_runtime)
    add_test(NAME homed_broker_hardening COMMAND test_broker_hardening)
    set_tests_properties(homed_broker_hardening
      PROPERTIES LABELS "nostr-homed;auth-runtime" TIMEOUT 120)
    add_executable(test_broker_login_nip46_qr tests/integration/test_broker_login_nip46_qr.c)
    if(TARGET nostr_nip46_core)
      target_link_libraries(test_broker_login_nip46_qr PRIVATE
        nostr_auth_runtime nostr_nip46_core)
    else()
      target_link_libraries(test_broker_login_nip46_qr PRIVATE
        nostr_auth_runtime PkgConfig::NOSTR_NIP46)
    endif()
    add_test(NAME homed_broker_login_nip46_qr COMMAND test_broker_login_nip46_qr)
    set_tests_properties(homed_broker_login_nip46_qr
      PROPERTIES LABELS "nostr-homed;auth-runtime;nip46" TIMEOUT 120)
    add_executable(test_auth_conf tests/unit/test_auth_conf.c)
    target_link_libraries(test_auth_conf PRIVATE nostr_auth_runtime)
    add_test(NAME homed_auth_conf COMMAND test_auth_conf)
    set_tests_properties(homed_auth_conf
      PROPERTIES LABELS "nostr-homed;portable;auth-runtime" TIMEOUT 30)
    # Greeter-artifact confidentiality (beads nostrc-3c7n / nostrc-zcll.6):
    # assert mode 0640 + group ownership on current.json / current.png /
    # avatar.png and mode 0750 + group ownership on the drop dir. Runs
    # unprivileged; the test seam pins the group to one the running uid
    # already belongs to.
    add_executable(test_greeter_artifact_perms
      tests/unit/test_greeter_artifact_perms.c)
    target_link_libraries(test_greeter_artifact_perms PRIVATE
      nostr_auth_runtime)
    add_test(NAME homed_greeter_artifact_perms
      COMMAND test_greeter_artifact_perms)
    set_tests_properties(homed_greeter_artifact_perms
      PROPERTIES LABELS "nostr-homed;portable;auth-runtime;security" TIMEOUT 30)
    # B5-NIP-05 (nostrc-bit0): parser + URL assembly + well-known JSON
    # parser + cache TTL. The helper's live HTTPS behaviour is covered
    # by the aarch64 VM integration run (docs/reviews/nip05-login-*.md).
    add_executable(test_nip05 tests/unit/test_nip05.c)
    target_link_libraries(test_nip05 PRIVATE nostr_auth_runtime)
    add_test(NAME homed_nip05 COMMAND test_nip05)
    set_tests_properties(homed_nip05
      PROPERTIES LABELS "nostr-homed;portable;auth-runtime;nip05" TIMEOUT 30)
    add_executable(test_pairing_code tests/unit/test_pairing_code.c)
    target_link_libraries(test_pairing_code PRIVATE nostr_auth_runtime)
    add_test(NAME homed_pairing_code COMMAND test_pairing_code)
    set_tests_properties(homed_pairing_code
      PROPERTIES LABELS "nostr-homed;portable;auth-runtime" TIMEOUT 30)
    add_executable(test_broker_login_nip46 tests/integration/test_broker_login_nip46.c)
    # Test drives an in-process bunker via the nip46 core, so link it directly.
    if(TARGET nostr_nip46_core)
      target_link_libraries(test_broker_login_nip46 PRIVATE
        nostr_auth_runtime nostr_nip46_core)
    else()
      target_link_libraries(test_broker_login_nip46 PRIVATE
        nostr_auth_runtime PkgConfig::NOSTR_NIP46)
    endif()
    add_test(NAME homed_broker_login_nip46 COMMAND test_broker_login_nip46)
    set_tests_properties(homed_broker_login_nip46
      PROPERTIES LABELS "nostr-homed;auth-runtime" TIMEOUT 120)
  endif()

  # B4 (nostrc-rb0e.3): nh-seed-authority is the mandatory provisioning tool
  # documented in docs/INSTALLED_LOGIN.md §2. Build it unconditionally with
  # the auth runtime — the test suite still consumes the same binary as a
  # test helper, but its installability no longer depends on
  # NOSTR_HOMED_BUILD_TESTS. Install rule lives under AUTH_INSTALL and
  # renames it to `nostr-homed-seed` in sbin.
  add_executable(nh-seed-authority tests/integration/seed_authority.c)
  target_link_libraries(nh-seed-authority PRIVATE nostr_auth_runtime)

  # nostrc-zcll.7 acceptance: sibling seeder that enrols a NIP-46 provider
  # (QR / nostrconnect, or pre-paired bunker) instead of a local vault.
  # Test helper only; not installed. See tests/integration/seed_nip46_authority.c.
  add_executable(nh-seed-nip46-authority tests/integration/seed_nip46_authority.c)
  target_link_libraries(nh-seed-nip46-authority PRIVATE nostr_auth_runtime)

  # nostrc-zcll.7 acceptance: headless "phone signer" that consumes a live
  # nostrconnect:// URI, dials its relays with a supplied signing key, and
  # answers get_public_key + sign_event. Drives real-relay QR acceptance
  # (positive + wrong-key negative). Not installed.
  if(TARGET nostr_nip46_core)
    add_executable(qr_signer_standin tests/integration/qr_signer_standin.c)
    target_link_libraries(qr_signer_standin PRIVATE
      nostr_auth_runtime nostr_nip46_core)
  elseif(NOSTR_NIP46_FOUND)
    add_executable(qr_signer_standin tests/integration/qr_signer_standin.c)
    target_link_libraries(qr_signer_standin PRIVATE
      nostr_auth_runtime PkgConfig::NOSTR_NIP46)
  endif()
endif()

# B5-profile: Nostr kind-0 → AccountsService bridge. Split into three
# artefacts so the security-critical picture download runs unprivileged:
#   - nostr_profile_core: relay fetch + sanitisers + metadata parser
#                         + fork/exec runner + AccountsService installer.
#                         Linked into the broker and seeders (they call
#                         nh_profile_refresh() directly).
#   - nostr-homed-profile: operator CLI (installed as sbin) — invoked
#                         both by the broker post-login hook and by
#                         operators for `nostr-homed-profile refresh <user>`.
#   - nostr-homed-profile-image: unprivileged HTTP fetcher + GdkPixbuf
#                         decoder. Never linked into the broker.
if(NOSTR_HOMED_ENABLE_PROFILE)
  if(NOT NOSTR_HOMED_ENABLE_AUTH_RUNTIME)
    message(FATAL_ERROR
      "NOSTR_HOMED_ENABLE_PROFILE requires NOSTR_HOMED_ENABLE_AUTH_RUNTIME")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GDK_PIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)
  pkg_check_modules(GIO2 REQUIRED IMPORTED_TARGET gio-2.0)
  find_package(CURL REQUIRED)
  # relay_fetch.c also compiles into nostr_homed_common under the
  # experimental-roaming build; keep those two targets non-overlapping.
  if(NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING)
    message(FATAL_ERROR
      "NOSTR_HOMED_ENABLE_PROFILE and NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING "
      "both compile src/common/relay_fetch.c; enable only one at a time.")
  endif()

  add_library(nostr_profile_core STATIC
    src/common/relay_fetch.c
    src/profile/profile_sanitize.c
    src/profile/profile_metadata.c
    src/profile/profile_image.c
    src/profile/profile_accounts.c
    src/profile/profile_refresh.c)
  target_compile_options(nostr_profile_core PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr_profile_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auth)
  target_link_libraries(nostr_profile_core PUBLIC
    nostr_auth_runtime PkgConfig::JANSSON PkgConfig::GIO2 SQLite3::SQLite3)

  add_executable(nostr-homed-profile src/profile/nostr-homed-profile.c)
  target_link_libraries(nostr-homed-profile PRIVATE nostr_profile_core)

  add_executable(nostr-homed-profile-image
    src/profile/nostr-homed-profile-image.c
    src/profile/profile_sanitize.c)
  target_compile_options(nostr-homed-profile-image PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr-homed-profile-image PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(nostr-homed-profile-image PRIVATE
    PkgConfig::GDK_PIXBUF CURL::libcurl)

  # B5-NIP-05 (nostrc-bit0): unprivileged NIP-05 resolver. Shares the
  # profile_sanitize SSRF gate + refused-range list so the two greeter
  # fetchers (avatar picture, .well-known/nostr.json) stay on one
  # policy — a single security review covers both. Not linked into
  # the broker (the parent is nip05_client.c in nostr_auth_runtime).
  add_executable(nostr-homed-nip05
    src/nip05/nostr-homed-nip05.c
    src/nip05/nip05_validate.c
    src/profile/profile_sanitize.c)
  target_compile_options(nostr-homed-nip05 PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr-homed-nip05 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(nostr-homed-nip05 PRIVATE
    PkgConfig::JANSSON CURL::libcurl)

  if(NOSTR_HOMED_ENABLE_AUTH_INSTALL)
    install(TARGETS nostr-homed-profile
      RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
    install(TARGETS nostr-homed-profile-image
      RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/nostr-homed)
    install(TARGETS nostr-homed-nip05
      RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/nostr-homed)
  endif()

  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_profile_sanitize
      tests/unit/test_profile_sanitize.c
      src/profile/profile_sanitize.c)
    target_include_directories(test_profile_sanitize PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(test_profile_sanitize PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    add_test(NAME homed_profile_sanitize COMMAND test_profile_sanitize)
    set_tests_properties(homed_profile_sanitize
      PROPERTIES LABELS "nostr-homed;portable;profile" TIMEOUT 30)

    add_executable(test_profile_metadata
      tests/unit/test_profile_metadata.c)
    target_link_libraries(test_profile_metadata PRIVATE nostr_profile_core)
    add_test(NAME homed_profile_metadata COMMAND test_profile_metadata)
    set_tests_properties(homed_profile_metadata
      PROPERTIES LABELS "nostr-homed;portable;profile" TIMEOUT 30)
  endif()
endif()

# B4: broker-backed PAM module. Authentication succeeds only via broker proof.
if(NOSTR_HOMED_ENABLE_PAM)
  if(NOT NOSTR_HOMED_ENABLE_AUTH_RUNTIME)
    message(FATAL_ERROR "NOSTR_HOMED_ENABLE_PAM requires NOSTR_HOMED_ENABLE_AUTH_RUNTIME")
  endif()
  find_path(PAM_INCLUDE_DIR security/pam_modules.h)
  find_library(PAM_LIBRARY NAMES pam)
  if(NOT PAM_INCLUDE_DIR OR NOT PAM_LIBRARY)
    message(FATAL_ERROR "NOSTR_HOMED_ENABLE_PAM requires libpam headers (libpam0g-dev)")
  endif()
  # nostr_qrcodegen was moved to a top-level target above (runtime block)
  # so it is available to both the broker daemon and pam_nostr. Kept as a
  # comment for grep-lookup: the definition used to live here.

  add_library(pam_nostr MODULE
    src/pam/pam_nostr_broker.c)
  set_target_properties(pam_nostr PROPERTIES PREFIX "" OUTPUT_NAME "pam_nostr")
  target_compile_options(pam_nostr PRIVATE -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(pam_nostr PRIVATE
    ${PAM_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pam
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen)
  target_link_libraries(pam_nostr PRIVATE
    nostr_auth_runtime ${PAM_LIBRARY})

  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_qr_render tests/unit/test_qr_render.c
      src/pam/pam_qr_render.c)
    target_compile_options(test_qr_render PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    target_include_directories(test_qr_render PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/pam
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen)
    target_link_libraries(test_qr_render PRIVATE nostr_qrcodegen)
    add_test(NAME homed_qr_render COMMAND test_qr_render)
    set_tests_properties(homed_qr_render
      PROPERTIES LABELS "nostr-homed;portable;pam" TIMEOUT 30)
  endif()
endif()

if(NOSTR_HOMED_ENABLE_DOMAIN_CONFIG)
  if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "NOSTR_HOMED_ENABLE_DOMAIN_CONFIG requires Python 3")
  endif()
  set(_domain_share "${CMAKE_INSTALL_DATADIR}/nostr-homed/domain")
  install(FILES
    config/domain-route.conf.sample
    config/smb.conf.winbind.sample
    packaging/domain/domain-profile.manifest.json.sample
    packaging/domain/ROLLBACK.md
    packaging/pam/nostr-winbind-policy.md
    docs/SAMBA_AD_LOGIN.md
    DESTINATION "${_domain_share}")
  install(PROGRAMS packaging/domain/validate_domain_profile.py
    DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/nostr-homed")
  if(NOSTR_HOMED_BUILD_TESTS)
    add_test(NAME homed_domain_profile
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_domain_profile.py")
    set_tests_properties(homed_domain_profile
      PROPERTIES LABELS "nostr-homed;portable;domain" TIMEOUT 30)
  endif()
endif()

if(NOSTR_HOMED_BUILD_TESTS AND Python3_Interpreter_FOUND)
  add_test(NAME homed_acceptance_runner_contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance/test_acceptance_runner.py")
  set_tests_properties(homed_acceptance_runner_contract
    PROPERTIES LABELS "nostr-homed;portable;acceptance-contract" TIMEOUT 30)
endif()

if(NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)
  pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET
    glib-2.0>=2.56 gobject-2.0>=2.56 gio-2.0>=2.56)
  find_package(OpenSSL REQUIRED)
  find_package(Threads REQUIRED)
  find_package(SQLite3 REQUIRED)
  # find_package(SQLite3) creates a directory-scoped imported target; in an
  # in-tree/root build another subdirectory may have found SQLite3 first, leaving
  # SQLite3::SQLite3 invisible here. Synthesize it from the cached result if so.
  if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 INTERFACE IMPORTED)
    target_include_directories(SQLite3::SQLite3 INTERFACE ${SQLite3_INCLUDE_DIRS})
    target_link_libraries(SQLite3::SQLite3 INTERFACE ${SQLite3_LIBRARIES})
  endif()
  find_package(CURL REQUIRED)
  find_library(SECP256K1_LIB NAMES secp256k1 REQUIRED)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  find_package(FUSE3 REQUIRED)
  if(NOT TARGET nip19)
    message(FATAL_ERROR "Experimental roaming requires ENABLE_NIP19=ON")
  endif()

  add_library(nostr_homed_common STATIC
    src/common/nostr_manifest.c
    src/common/nostr_cache.c
    src/common/relay_fetch.c
    src/common/blossom_client.c
    src/common/secrets_tmpfs.c
    src/common/secrets_decrypt.c
    src/common/nip46_client_dbus.c
    src/common/util.c)
  target_include_directories(nostr_homed_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(nostr_homed_common PUBLIC
    libnostr nostr_json libgo nip19 PkgConfig::JANSSON PkgConfig::GLIB2
    OpenSSL::Crypto ${SECP256K1_LIB} SQLite3::SQLite3 CURL::libcurl Threads::Threads)

  add_executable(nostrfs src/fs/nostrfs.c)
  target_compile_definitions(nostrfs PRIVATE FUSE_USE_VERSION=30)
  target_compile_options(nostrfs PRIVATE -Wno-misleading-indentation -Wno-format-truncation)
  target_include_directories(nostrfs PRIVATE ${FUSE3_INCLUDE_DIRS})
  target_link_libraries(nostrfs PRIVATE nostr_homed_common ${FUSE3_LIBRARIES})
  add_executable(nostr-homectl src/ctl/nostr-homectl.c)
  target_link_libraries(nostr-homectl PRIVATE nostr_homed_common PkgConfig::GLIB2)

  install(TARGETS nostrfs nostr-homectl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

  # Focused roaming regression tests for beads nostrc-nxpb.7 and nostrc-nxpb.8.
  # These recompile nostr-homectl.c into a test binary (the daemon
  # production main is skipped via -DNH_HOMECTL_NO_MAIN=1) and use the
  # internal test seam (include/nostr_homectl_test_seam.h) to inject failures at each boundary
  # (relay fetches, systemctl start, mkdir, is_mountpoint). They do not
  # require D-Bus, systemd, FUSE, or a live relay; they run under ASan.
  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_homectl_relay_lifetime
      src/ctl/nostr-homectl.c
      tests/integ/test_homectl_relay_lifetime.c)
    target_compile_definitions(test_homectl_relay_lifetime PRIVATE
      NH_HOMECTL_NO_MAIN=1)
    target_include_directories(test_homectl_relay_lifetime PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../nips/nip19/include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../libnostr/include)
    target_compile_options(test_homectl_relay_lifetime PRIVATE
      -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
      -Wno-format-truncation)
    target_link_libraries(test_homectl_relay_lifetime PRIVATE
      PkgConfig::GLIB2 PkgConfig::JANSSON)
    add_test(NAME homed_homectl_relay_lifetime
      COMMAND test_homectl_relay_lifetime)
    set_tests_properties(homed_homectl_relay_lifetime PROPERTIES
      LABELS "nostr-homed;experimental-roaming;roaming-safety" TIMEOUT 60)

    add_executable(test_homectl_open_session_failures
      src/ctl/nostr-homectl.c
      tests/integ/test_homectl_open_session_failures.c)
    target_compile_definitions(test_homectl_open_session_failures PRIVATE
      NH_HOMECTL_NO_MAIN=1)
    target_include_directories(test_homectl_open_session_failures PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../nips/nip19/include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../libnostr/include)
    target_compile_options(test_homectl_open_session_failures PRIVATE
      -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
      -Wno-format-truncation)
    target_link_libraries(test_homectl_open_session_failures PRIVATE
      PkgConfig::GLIB2 PkgConfig::JANSSON)
    add_test(NAME homed_homectl_open_session_failures
      COMMAND test_homectl_open_session_failures)
    set_tests_properties(homed_homectl_open_session_failures PROPERTIES
      LABELS "nostr-homed;experimental-roaming;roaming-safety" TIMEOUT 60)
  endif()

  install(FILES dbus/org.nostr.Homed1.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/interfaces)
  pkg_get_variable(SYSTEMD_USER_UNIT_DIR systemd systemduserunitdir)
  if(NOT SYSTEMD_USER_UNIT_DIR)
    set(SYSTEMD_USER_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/user")
  endif()
  install(FILES
    systemd/user/nostr-homectl.service
    systemd/user/nostrfs@.service
    DESTINATION "${SYSTEMD_USER_UNIT_DIR}")
endif()


# D5-owned SMB credential authority core.  Isolated from the broker: mints
# short-lived random SMB passwords after the broker has produced an
# SMB_CREDENTIAL proof receipt, installs them via a pluggable passdb adapter,
# and journals issuance metadata (never the password) in SQLite.  Ships a
# real tdbsam adapter that shells out to smbpasswd/pdbedit for VM testing.
if(NOSTR_HOMED_ENABLE_SMB)
  if(NOT NOSTR_HOMED_ENABLE_IDENTITY_CORE)
    message(FATAL_ERROR
      "NOSTR_HOMED_ENABLE_SMB requires NOSTR_HOMED_ENABLE_IDENTITY_CORE for nh_identity_account")
  endif()
  find_package(SQLite3 REQUIRED)
  if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 INTERFACE IMPORTED)
    target_include_directories(SQLite3::SQLite3 INTERFACE ${SQLite3_INCLUDE_DIRS})
    target_link_libraries(SQLite3::SQLite3 INTERFACE ${SQLite3_LIBRARIES})
  endif()

  # Core (portable, mockable).  Never embeds the real adapter.
  add_library(nostr_smb_core STATIC src/smb/smb_credential.c)
  target_compile_options(nostr_smb_core PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr_smb_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/smb)
  # smb_credential.h pulls nostr_identity.h; and the .c uses secure_buf from
  # libnostr.  Link to the identity core (which already publishes the include
  # path) and to libnostr for secure_alloc/secure_free/secure_wipe.
  target_link_libraries(nostr_smb_core PUBLIC nostr_identity_core SQLite3::SQLite3)
  if(TARGET libnostr)
    target_link_libraries(nostr_smb_core PUBLIC libnostr)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NOSTR_FOR_SMB REQUIRED IMPORTED_TARGET nostr)
    target_link_libraries(nostr_smb_core PUBLIC PkgConfig::NOSTR_FOR_SMB)
  endif()

  # Real Samba adapter: not linked into the core, built separately so the
  # daemon (added in D6) can pick it up.  Requires a Samba VM to test end-to-end.
  add_library(nostr_smb_tdbsam STATIC src/smb/passdb_tdbsam.c)
  target_compile_options(nostr_smb_tdbsam PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr_smb_tdbsam PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/smb)
  target_link_libraries(nostr_smb_tdbsam PUBLIC nostr_smb_core)

  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_smb_credential
      tests/integration/test_smb_credential.c)
    target_compile_options(test_smb_credential PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    target_link_libraries(test_smb_credential PRIVATE nostr_smb_core)
    add_test(NAME homed_smb_credential COMMAND test_smb_credential)
    set_tests_properties(homed_smb_credential
      PROPERTIES LABELS "nostr-homed;smb" TIMEOUT 60)
  endif()

  # Standalone driver used by tests/acceptance/run_tdbsam.sh on a
  # Samba-provisioned VM.  Not a ctest — the acceptance script wires it
  # against real smbpasswd/pdbedit.  See beads nostrc-rb0e.7.
  add_executable(tdbsam_driver
    tests/integration/tdbsam_driver.c)
  target_compile_options(tdbsam_driver PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_compile_definitions(tdbsam_driver PRIVATE _DEFAULT_SOURCE=1)
  target_include_directories(tdbsam_driver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/smb)
  target_link_libraries(tdbsam_driver PRIVATE nostr_smb_tdbsam)
endif()

# Version metadata only: there is no installed nostr-homed link library.
configure_file(nostr-homed.pc.in nostr-homed.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostr-homed.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

# Installed login runtime (AUTH_INSTALL): broker daemon, PAM module and systemd
# unit. The NSS module is installed by its own block. Accounts are not shipped.
if(NOSTR_HOMED_ENABLE_AUTH_INSTALL)
  install(TARGETS nostr-authd RUNTIME DESTINATION "${CMAKE_INSTALL_SBINDIR}")
  set(NH_PAM_MODULE_DIR "${CMAKE_INSTALL_LIBDIR}/security"
      CACHE STRING "Directory for the installed pam_nostr.so module")
  install(TARGETS pam_nostr LIBRARY DESTINATION "${NH_PAM_MODULE_DIR}")
  set(SYSTEMD_SYSTEM_UNIT_DIR "")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_get_variable(SYSTEMD_SYSTEM_UNIT_DIR systemd systemdsystemunitdir)
  endif()
  if(NOT SYSTEMD_SYSTEM_UNIT_DIR)
    set(SYSTEMD_SYSTEM_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system")
  endif()
  set(NH_AUTHD_PATH "${CMAKE_INSTALL_FULL_SBINDIR}/nostr-authd")
  # On SMB-enabled builds the daemon also serves the own-account USER endpoint
  # (user.sock) and needs the SMB issuance journal path; otherwise it runs the
  # two-argument AUTH-only form.  nostr-authd tolerates the 4-arg form on
  # SMB-disabled builds (it warns and ignores the user socket), but emitting the
  # matching form keeps the shipped unit honest about the build.
  if(NOSTR_HOMED_ENABLE_SMB)
    set(NH_AUTHD_ARGS "/run/nostr-auth/auth.sock /var/lib/nostr-auth /run/nostr-auth/user.sock /var/lib/nostr-auth/smb.db")
  else()
    set(NH_AUTHD_ARGS "/run/nostr-auth/auth.sock /var/lib/nostr-auth")
  endif()
  configure_file(systemd/nostr-authd.service.in
    "${CMAKE_CURRENT_BINARY_DIR}/nostr-authd.service" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostr-authd.service"
    DESTINATION "${SYSTEMD_SYSTEM_UNIT_DIR}")
  install(FILES config/auth.conf.sample
    DESTINATION "${CMAKE_INSTALL_DATADIR}/nostr-homed")
  # Greeter-artifact confidentiality (beads nostrc-3c7n, references
  # nostrc-zcll.6): provision the nostr-auth-greeter system group so
  # the ExecStartPre in nostr-authd.service can `install -d -g
  # nostr-auth-greeter` /run/nostr-auth/greeter and the broker can
  # chown current.{json,png} + avatar.png to it. The sysusers.d(5)
  # snippet runs at boot / package install; packaging still needs to
  # add gdm to the group (postinst) and drop the pam_group sample into
  # /etc/security/group.conf.d — see the sample below for the exact
  # incantation.
  install(FILES packaging/sysusers.d/nostr-auth-greeter.conf
    DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/sysusers.d")
  # Phase 2.5B (bead nostrc-ww50): provision the dedicated helper user
  # `nostr-home-fetch` for the portable-home fetch sandbox. Only useful
  # when the experimental option is on, but the snippet is cheap to
  # ship unconditionally — an idle "u nostr-home-fetch" costs one row
  # in /etc/passwd, and shipping it here avoids a second postinst hook.
  install(FILES packaging/sysusers.d/nostr-home-fetch.conf
    DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/sysusers.d")
  install(FILES packaging/pam/nostr-auth-greeter.group.conf.sample
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/examples/pam")

  # B4 (nostrc-rb0e.3): install nh-seed-authority as /usr/sbin/nostr-homed-seed.
  # The build target keeps its original name so existing test infrastructure
  # that greps for `nh-seed-authority` in the build tree keeps working.
  # install(TARGETS) does not honour RENAME; use install(PROGRAMS) against the
  # generator-expression build path so the shipped filename matches
  # docs/INSTALLED_LOGIN.md §2 (`nostr-homed-seed`) while the build target
  # keeps its original name.
  install(PROGRAMS $<TARGET_FILE:nh-seed-authority>
    DESTINATION "${CMAKE_INSTALL_SBINDIR}"
    RENAME nostr-homed-seed)

  # B6 (nostrc-rb0e.3 / packaging-plan §6.3): pam-auth-update profile so admins
  # can compose pam_nostr into common-auth / common-account via
  # `pam-auth-update` (Debian) instead of overwriting gdm-password. Semantics
  # match packaging/pam/gdm-password.sample exactly (strict-deny posture).
  install(FILES packaging/pam/nostr
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pam-configs")
  install(FILES packaging/pam/gdm-password.sample
                packaging/pam/nostr-login.sample
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/examples/pam")
endif()

# B5 / D-11: headless identity-authority admin CLI. Builds nostr-homectl on
# top of nostr_identity_core alone -- no GLib, no FUSE, no D-Bus session bus.
# Independent of NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING; the target name
# collision is prevented by the mutual-exclusion check above.
if(NOSTR_HOMED_ENABLE_CTL)
  add_executable(nostr-homectl src/ctl/nostr-homectl-headless.c)
  target_compile_options(nostr-homectl PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr-homectl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(nostr-homectl PRIVATE nostr_identity_core)
  if(NOSTR_HOMED_ENABLE_AUTH_INSTALL)
    install(TARGETS nostr-homectl
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()
endif()


# ----------------------------------------------------------------------------
# D6 (nostrc-rb0e.7): SMB proof endpoint wiring.
#
# When both the auth runtime and the D5 SMB credential core are enabled,
# link the runtime against the SMB core, propagate the NH_AUTH_BROKER_ENABLE_SMB
# compile flag, and build the headless SMB proof integration test.
# Self-contained block; touches only targets defined earlier in this file.
# ----------------------------------------------------------------------------
if(NOSTR_HOMED_ENABLE_AUTH_RUNTIME AND NOSTR_HOMED_ENABLE_SMB)
  target_compile_definitions(nostr_auth_runtime PUBLIC NH_AUTH_BROKER_ENABLE_SMB=1)
  target_link_libraries(nostr_auth_runtime PUBLIC nostr_smb_core)
  # The daemon shells out through the real tdbsam adapter when the user socket
  # is configured; keep it linked so `nostr-authd` can drive Samba on a VM.
  target_link_libraries(nostr-authd PRIVATE nostr_smb_tdbsam)
  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_smb_proof tests/integration/test_smb_proof.c)
    target_compile_options(test_smb_proof PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    target_link_libraries(test_smb_proof PRIVATE nostr_auth_runtime nostr_smb_core)
    add_test(NAME homed_smb_proof COMMAND test_smb_proof)
    set_tests_properties(homed_smb_proof
      PROPERTIES LABELS "nostr-homed;auth-runtime;smb" TIMEOUT 120)
  endif()
endif()

# ----------------------------------------------------------------------------
# D7 (nostrc-rb0e.8): desktop SMB credential acquisition client.
#
# Builds `nostr-smb-acquire` — a user-facing tool that connects to
# /run/nostr-auth/user.sock, drives BEGIN_SMB_PROOF via the D6 auth_client
# helper, and delivers the minted password as a Samba credentials= file
# (mode 0600) or, with --stdout, to a one-shot pipe.  The proof/vault
# and passdb wiring live in the runtime + smb_core; this block adds only
# the new tool and its headless integration test.
#
# Self-contained: gated on the SAME options as the D6 block above so the
# targets it links against (nostr_auth_runtime, nostr_smb_core) are known
# to exist.  Adds NO new options and does not disturb targets defined
# earlier.  An install rule is intentionally NOT emitted here; the desktop
# packaging bucket owns tool install layouts.
# ----------------------------------------------------------------------------
# ----------------------------------------------------------------------------
# Greeter QR extension (nostrc-z1fb / zcll.6):
#
# A tiny gnome-shell extension that watches /run/nostr-auth/greeter/current.json
# and renders the pending NIP-46 QR pairing in the GDM login dialog.  Data-only:
# three files installed under the system-wide gnome-shell extensions dir so
# gdm picks it up in "gdm" session mode.  Enabling the extension for the
# greeter is a dconf keyfile change (see greeter-extension/README.md) and is
# intentionally NOT done from CMake — packagers own that policy.
# ----------------------------------------------------------------------------
if(NOSTR_HOMED_ENABLE_GREETER_EXTENSION)
  set(NH_GREETER_EXT_UUID "nostr-login-qr@nostrc")
  set(NH_GREETER_EXT_SRC
      "${CMAKE_CURRENT_SOURCE_DIR}/greeter-extension/${NH_GREETER_EXT_UUID}")
  set(NH_GREETER_EXT_DEST
      "${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${NH_GREETER_EXT_UUID}")
  install(FILES
      "${NH_GREETER_EXT_SRC}/metadata.json"
      "${NH_GREETER_EXT_SRC}/extension.js"
      "${NH_GREETER_EXT_SRC}/stylesheet.css"
    DESTINATION "${NH_GREETER_EXT_DEST}")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/greeter-extension/README.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/greeter-extension")
endif()

# Phase 5 I1 (bead nostrc-h10m.1): optional user-session status
# indicator extension. Data-only; three files land under the system
# gnome-shell extensions dir. Enabling per user is policy — the
# extension does NOT auto-enable itself.
if(NOSTR_HOMED_ENABLE_PORTHOME_STATUS_EXTENSION)
  set(NH_STATUS_EXT_UUID "nostr-home-status@nostrc")
  set(NH_STATUS_EXT_SRC
      "${CMAKE_CURRENT_SOURCE_DIR}/greeter-extension/${NH_STATUS_EXT_UUID}")
  set(NH_STATUS_EXT_DEST
      "${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${NH_STATUS_EXT_UUID}")
  install(FILES
      "${NH_STATUS_EXT_SRC}/metadata.json"
      "${NH_STATUS_EXT_SRC}/extension.js"
      "${NH_STATUS_EXT_SRC}/stylesheet.css"
    DESTINATION "${NH_STATUS_EXT_DEST}")
  install(FILES "${NH_STATUS_EXT_SRC}/README.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/status-extension")
endif()

if(NOSTR_HOMED_ENABLE_AUTH_RUNTIME AND NOSTR_HOMED_ENABLE_SMB)
  add_executable(nostr-smb-acquire
    src/smb/nostr-smb-acquire.c)
  target_compile_options(nostr-smb-acquire PRIVATE
    -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_compile_definitions(nostr-smb-acquire PRIVATE _DEFAULT_SOURCE=1)
  target_include_directories(nostr-smb-acquire PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/smb)
  target_link_libraries(nostr-smb-acquire PRIVATE
    nostr_auth_runtime nostr_smb_core)
  # D7 desktop mount helper (beads nostrc-rb0e.8): a shell wrapper that
  # consumes the credentials= file written by nostr-smb-acquire and mounts
  # the SMB share via mount.cifs (default) or gvfs (--mode gvfs).  It is
  # not compiled — it is a small POSIX shell tool — so we install it as a
  # PROGRAM alongside the acquire binary when AUTH_INSTALL is on.  The
  # acceptance script gnome/nostr-homed/tests/acceptance/run_smb_mount_vm.sh
  # exercises the full mint -> mount -> revoke path on a disposable VM.
  configure_file(src/smb/nostr-smb-mount.sh
    "${CMAKE_CURRENT_BINARY_DIR}/nostr-smb-mount" COPYONLY)
  if(NOSTR_HOMED_ENABLE_AUTH_INSTALL)
    install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/nostr-smb-mount"
            DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(TARGETS nostr-smb-acquire
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()
  if(NOSTR_HOMED_BUILD_TESTS)
    # Rebuild the acquire core into the test binary with NH_SMB_ACQUIRE_NO_MAIN
    # so we can drive it without exec'ing the CLI or depending on a tty.
    add_executable(test_smb_acquire
      tests/integration/test_smb_acquire.c
      src/smb/nostr-smb-acquire.c)
    target_compile_options(test_smb_acquire PRIVATE
      -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation)
    target_compile_definitions(test_smb_acquire PRIVATE
      _DEFAULT_SOURCE=1 NH_SMB_ACQUIRE_NO_MAIN=1)
    target_include_directories(test_smb_acquire PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/smb
      ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(test_smb_acquire PRIVATE
      nostr_auth_runtime nostr_smb_core)
    add_test(NAME homed_smb_acquire COMMAND test_smb_acquire)
    set_tests_properties(homed_smb_acquire
      PROPERTIES LABELS "nostr-homed;smb" TIMEOUT 120)
  endif()
endif()


# ────────────────────────────────────────────────────────────────────
# Portable-home Phase 1 primitives (bead nostrc-h10m). EXPERIMENTAL.
# ────────────────────────────────────────────────────────────────────
if(NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL)
  find_package(OpenSSL REQUIRED)

  # Reuse the in-tree HKDF/HMAC-SHA256 primitives from nip44 rather than
  # linking a whole libnip44. We compile the single translation unit into
  # our own static library — the file has no external state and no
  # non-crypto dependencies.
  set(_porthome_nip44_hkdf
    "${CMAKE_CURRENT_SOURCE_DIR}/../../nips/nip44/src/core/nip44_hkdf_hmac.c")
  if(NOT EXISTS "${_porthome_nip44_hkdf}")
    message(FATAL_ERROR
      "NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL needs "
      "${_porthome_nip44_hkdf} but the file is missing.")
  endif()

  # libhanami is the substrate for the Blossom wrapper. In the in-tree
  # build (root CMakeLists.txt) libhanami's add_subdirectory() runs
  # AFTER gnome/nostr-homed/, so `if(TARGET hanami)` here is always
  # false. CMake tolerates linking to a target defined later in the
  # same configure pass, so we just spell the name and let the linker
  # phase resolve it. A stand-alone build (with pkg-config-installed
  # libhanami) is out of Phase-1 scope; a future BUILD_STANDALONE knob
  # can add pkg_check_modules(HANAMI ...) here.
  if(NOT BUILD_LIBHANAMI)
    message(FATAL_ERROR
      "NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL requires BUILD_LIBHANAMI=ON "
      "(the in-tree libhanami provides the Blossom client).")
  endif()
  set(_porthome_hanami hanami)

  add_library(nostr_porthome STATIC
    src/porthome/nh_porthome_crypto.c
    src/porthome/nh_porthome_manifest.c
    src/porthome/nh_porthome_blossom.c
    src/porthome/nh_porthome_cbor.c
    src/porthome/nh_porthome_wrapkey.c
    src/porthome/nh_porthome_provision.c
    # Phase 2.5B: fork+drop-privs sandbox for the fetch helper
    # (bead nostrc-ww50). Header lives under include/. Only linked
    # when NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL is on.
    src/porthome/nh_porthome_sandbox.c
    ${_porthome_nip44_hkdf})
  target_compile_options(nostr_porthome PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_compile_definitions(nostr_porthome PUBLIC
    NH_PORTHOME_EXPERIMENTAL=1)
  target_include_directories(nostr_porthome PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_include_directories(nostr_porthome PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome)
  target_link_libraries(nostr_porthome
    PUBLIC OpenSSL::Crypto ${_porthome_hanami})

  # ────────────────────────────────────────────────────────────────
  # Phase 5 I1 (bead nostrc-h10m.1): unified UX seams.
  # nostr_porthome_common ships the shared notification throttler +
  # quiet-hours module (nh_porthome_notify.{h,c}) AND the atomic
  # porthome-status.json merge helper (nh_porthome_status.{h,c}).
  # STATIC and non-installed. Linked into nostr_syncd_core, the FUSE
  # binary, and the nostr-home-status CLI so all three writers merge
  # into one file with one throttle policy.
  # ────────────────────────────────────────────────────────────────
  add_library(nostr_porthome_common STATIC
    src/porthome-common/nh_porthome_notify.c
    src/porthome-common/nh_porthome_status.c)
  target_compile_options(nostr_porthome_common PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr_porthome_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  set_property(TARGET nostr_porthome_common PROPERTY
    POSITION_INDEPENDENT_CODE ON)

  # Operator CLI. Installed under bin when AUTH_INSTALL is on so a
  # release build ships `nostr-home-status` alongside the other homed
  # operator tools. Not built when AUTH_INSTALL is off — the CLI is a
  # tiny wrapper on the common library and testing the library
  # directly is more useful than exercising argv parsing.
  add_executable(nostr-home-status
    src/porthome-common/nostr-home-status.c)
  target_compile_options(nostr-home-status PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_link_libraries(nostr-home-status PRIVATE
    nostr_porthome_common)

  if(NOSTR_HOMED_ENABLE_AUTH_INSTALL)
    install(TARGETS nostr-home-status
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()

  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_porthome_notify
      tests/unit/test_porthome_notify.c)
    target_link_libraries(test_porthome_notify PRIVATE
      nostr_porthome_common)
    target_compile_options(test_porthome_notify PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    add_test(NAME homed_porthome_notify COMMAND test_porthome_notify)
    set_tests_properties(homed_porthome_notify PROPERTIES
      LABELS "nostr-homed;porthome;portable" TIMEOUT 30
      ENVIRONMENT "NOSTR_HOMED_SYNCD_NOTIFY=0")

    add_executable(test_porthome_status
      tests/unit/test_porthome_status.c)
    target_link_libraries(test_porthome_status PRIVATE
      nostr_porthome_common)
    target_compile_options(test_porthome_status PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    add_test(NAME homed_porthome_status COMMAND test_porthome_status)
    set_tests_properties(homed_porthome_status PROPERTIES
      LABELS "nostr-homed;porthome;portable" TIMEOUT 30)
  endif()

  # ────────────────────────────────────────────────────────────────
  # Portable-home Phase 2.5 (bead nostrc-9k4g):
  # unprivileged fetch helper (nostr-home-fetch) + broker spawner.
  # The helper is a separate executable so its libnostr / libcurl /
  # libhanami dep closure never enters nostr-authd or pam_nostr.
  # ────────────────────────────────────────────────────────────────

  # Tiny standalone parser library — no libnostr / libhanami dep. Used
  # by the helper AND by the broker spawner, so both parsers agree by
  # construction. Also independently unit-testable.
  add_library(nostr_porthome_fetch_ctl STATIC
    src/porthome-fetch/porthome_fetch_ctl.c)
  target_compile_options(nostr_porthome_fetch_ctl PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr_porthome_fetch_ctl PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-fetch)

  # The helper binary. Only built when we have a libnostr (for the
  # relay REQ / EOSE lifecycle) plus curl (transitively via libhanami)
  # plus the porthome primitives. Runs unprivileged — never installed
  # into the base package.
  find_package(PkgConfig QUIET)
  set(_porthome_fetch_libnostr_ok TRUE)
  if(NOT TARGET nostr)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(NOSTR QUIET IMPORTED_TARGET libnostr)
      if(NOT NOSTR_FOUND)
        set(_porthome_fetch_libnostr_ok FALSE)
      endif()
    else()
      set(_porthome_fetch_libnostr_ok FALSE)
    endif()
  endif()
  find_package(PkgConfig QUIET)
  pkg_check_modules(_PH_GLIB QUIET IMPORTED_TARGET glib-2.0)
  if(NOT _PH_GLIB_FOUND)
    set(_porthome_fetch_libnostr_ok FALSE)
  endif()

  if(_porthome_fetch_libnostr_ok)
    add_executable(nostr-home-fetch
      src/porthome-fetch/nostr-home-fetch.c)
    target_compile_options(nostr-home-fetch PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    target_include_directories(nostr-home-fetch PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-fetch)
    target_link_libraries(nostr-home-fetch PRIVATE
      nostr_porthome nostr_porthome_fetch_ctl
      $<IF:$<TARGET_EXISTS:nostr>,nostr,PkgConfig::NOSTR>
      PkgConfig::_PH_GLIB
      OpenSSL::Crypto)
    # Phase 5 packaging (bead nostrc-h10m.2): install the unprivileged
    # fetch helper under libexec so the nostr-home-sync binary package
    # can ship it. The broker resolves the helper via the
    # NH_PORTHOME_FETCH_HELPER env override (see auth_porthome_fetch.c);
    # packagers should export NH_PORTHOME_FETCH_HELPER=%{_libexecdir}/nostr-homed/nostr-home-fetch
    # in a distro drop-in if the compile-time default (build tree) is
    # not the shipped location.
    if(NOSTR_HOMED_ENABLE_AUTH_INSTALL)
      install(TARGETS nostr-home-fetch
              RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/nostr-homed")
    endif()
  endif()


  # ────────────────────────────────────────────────────────────────
  # Portable-home Phase 2 follow-up (bead nostrc-89rj / nostrc-5fdu):
  # nostr-homed-provision CLI — user-facing operator tool mirroring
  # the broker's PROVISION_HOME code path. Ships in $BINDIR under
  # NOSTR_HOMED_ENABLE_AUTH_INSTALL alongside nostr-home-status.
  #
  # Links the full porthome stack (libhanami transitively via
  # nostr_porthome, libnostr for kind-30078 signing).  It is a
  # USER-SPACE operator tool; the dep-purity gate on nostr-authd +
  # pam_nostr is enforced separately below (they never grow a link
  # to this binary or to nostr_provision_cli).
  # ────────────────────────────────────────────────────────────────

  # Non-network helper library — pure JSON codec + filesystem walk.
  # Unit-testable without any transport dep.
  add_library(nostr_provision_cli STATIC
    src/porthome/nh_provision_cli.c)
  target_compile_options(nostr_provision_cli PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr_provision_cli PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome
    ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-fetch
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(nostr_provision_cli PUBLIC
    nostr_porthome nostr_porthome_common)
  set_property(TARGET nostr_provision_cli PROPERTY POSITION_INDEPENDENT_CODE ON)

  # Only wire the binary when libnostr is available (for keypair gen)
  # and the fetch helper builds — mirrors the nostr-home-fetch gate.
  if(_porthome_fetch_libnostr_ok)
    add_executable(nostr-homed-provision
      src/porthome/nostr-homed-provision.c)
    target_compile_options(nostr-homed-provision PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    target_include_directories(nostr-homed-provision PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome
      ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-fetch)
    target_link_libraries(nostr-homed-provision PRIVATE
      nostr_provision_cli
      nostr_porthome nostr_porthome_fetch_ctl nostr_porthome_common
      $<IF:$<TARGET_EXISTS:nostr>,nostr,PkgConfig::NOSTR>
      OpenSSL::Crypto)
    # If nostr-home-fetch was built above, point the pull spawner at
    # the sibling build-tree binary so a source build "just works".
    if(TARGET nostr-home-fetch)
      target_compile_definitions(nostr-homed-provision PRIVATE
        NH_PORTHOME_PROVISION_HELPER="$<TARGET_FILE:nostr-home-fetch>")
    endif()
    if(NOSTR_HOMED_ENABLE_AUTH_INSTALL)
      install(TARGETS nostr-homed-provision
              RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
  endif()

  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_provision_cli
      tests/unit/test_provision_cli.c)
    target_compile_options(test_provision_cli PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    target_link_libraries(test_provision_cli PRIVATE
      nostr_provision_cli nostr_porthome_fetch_ctl)
    add_test(NAME homed_provision_cli COMMAND test_provision_cli)
    set_tests_properties(homed_provision_cli PROPERTIES
      LABELS "nostr-homed;porthome;portable" TIMEOUT 30)
  endif()

  # ────────────────────────────────────────────────────────────────
  # Portable-home Phase 2 (bead nostrc-89rj, nostrc-ck6i):
  # broker-side glue. Only when the auth runtime is also built.
  # Adds src/auth/auth_porthome.c to nostr_auth_runtime with
  # NH_AUTH_BROKER_ENABLE_PORTHOME defined so PROVISION_HOME/
  # WAIT_HOME dispatch handlers do real work. The runtime picks
  # up nostr_porthome as a PUBLIC dep so pam_nostr and the broker
  # can both call into the porthome primitives — the packaging
  # gate is that this whole branch only runs when the operator
  # has explicitly enabled NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL.
  if(NOSTR_HOMED_ENABLE_AUTH_RUNTIME)
    # Turn on the real handlers inside the always-compiled
    # auth_porthome.c and pull in libnostr_porthome. When the
    # experimental option is OFF this whole block is skipped, so
    # the runtime keeps the #else stubs and does not link the
    # porthome archive / libhanami.
    target_compile_definitions(nostr_auth_runtime PUBLIC
      NH_AUTH_BROKER_ENABLE_PORTHOME=1)
    target_sources(nostr_auth_runtime PRIVATE
      src/auth/auth_porthome_fetch.c
      # Bead nostrc-3o91 — provisioner "provisioner" key writer.
      # Compiled behind NH_AUTH_BROKER_ENABLE_PORTHOME so the
      # base link closure is unchanged when experimental is off.
      src/auth/auth_porthome_status.c)
    target_link_libraries(nostr_auth_runtime PUBLIC
      nostr_porthome nostr_porthome_fetch_ctl nostr_porthome_common)
    # If the helper was built above, point the spawner at its build-time
    # location by default so an installed run without the packaging
    # split still finds it. Operators override with NH_PORTHOME_FETCH_HELPER.
    if(TARGET nostr-home-fetch)
      target_compile_definitions(nostr_auth_runtime PRIVATE
        NH_PORTHOME_FETCH_HELPER_PATH="$<TARGET_FILE:nostr-home-fetch>")
    endif()
  endif()

  # ────────────────────────────────────────────────────────────
  # Phase-2 unit tests: wrap-key derivation vectors and
  # provisioner LIMITED_MODE interlock.
  # ────────────────────────────────────────────────────────────
  # Phase 1 does NOT install — the library is an intermediate for the
  # Phase-2 provisioner and sync daemon. Explicitly no install rules.

  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_porthome_crypto tests/unit/test_porthome_crypto.c)
    target_link_libraries(test_porthome_crypto PRIVATE nostr_porthome)
    target_compile_options(test_porthome_crypto PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    add_test(NAME homed_porthome_crypto COMMAND test_porthome_crypto)
    set_tests_properties(homed_porthome_crypto PROPERTIES
      LABELS "nostr-homed;porthome;portable" TIMEOUT 60)

    add_executable(test_porthome_manifest tests/unit/test_porthome_manifest.c)
    target_link_libraries(test_porthome_manifest PRIVATE nostr_porthome)
    target_compile_options(test_porthome_manifest PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    add_test(NAME homed_porthome_manifest COMMAND test_porthome_manifest)
    set_tests_properties(homed_porthome_manifest PROPERTIES
      LABELS "nostr-homed;porthome;portable" TIMEOUT 60)

    add_executable(test_porthome_blossom_guards
      tests/unit/test_porthome_blossom_guards.c)
    target_link_libraries(test_porthome_blossom_guards PRIVATE nostr_porthome)
    target_compile_options(test_porthome_blossom_guards PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    add_test(NAME homed_porthome_blossom_guards
      COMMAND test_porthome_blossom_guards)
    set_tests_properties(homed_porthome_blossom_guards PROPERTIES
      LABELS "nostr-homed;porthome;portable" TIMEOUT 30)

    # Phase 2.5 (bead nostrc-9k4g): control-payload parser + progress-line
    # codec. Deliberately links ONLY the tiny standalone library — no
    # libnostr / libhanami / libcurl — so the base test matrix runs it
    # even when the helper binary itself does not build.
    add_executable(test_porthome_fetch_ctl
      tests/unit/test_porthome_fetch_ctl.c)
    target_link_libraries(test_porthome_fetch_ctl PRIVATE
      nostr_porthome_fetch_ctl)
    target_compile_options(test_porthome_fetch_ctl PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    add_test(NAME homed_porthome_fetch_ctl COMMAND test_porthome_fetch_ctl)
    set_tests_properties(homed_porthome_fetch_ctl PROPERTIES
      LABELS "nostr-homed;porthome;portable" TIMEOUT 30)

    # Bead nostrc-9k4g: SSRF pre-resolution helpers used by the fetch
    # binary. The test embeds a copy of the helpers so it links only
    # against libc / netdb — no libnostr / libhanami / libcurl —
    # keeping the base test matrix able to run it.
    add_executable(test_porthome_fetch_ssrf
      tests/unit/test_porthome_fetch_ssrf.c)
    target_compile_options(test_porthome_fetch_ssrf PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    add_test(NAME homed_porthome_fetch_ssrf COMMAND test_porthome_fetch_ssrf)
    set_tests_properties(homed_porthome_fetch_ssrf PROPERTIES
      LABELS "nostr-homed;porthome;portable" TIMEOUT 30)


    # Driver used by the Python integration test; not a standalone add_test.
    add_executable(porthome_smallhome_driver
      tests/integration/porthome_smallhome_driver.c)
    target_link_libraries(porthome_smallhome_driver PRIVATE nostr_porthome)
    target_compile_options(porthome_smallhome_driver PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)

    # Bead nostrc-9k4g.1: nostr-home-publisher — operator / test tool
    # that captures + uploads a fixture directory AND publishes the
    # kind-30078 pointer to a real relay. NOT installed in the base
    # package. Links libnostr for the relay publish path (already
    # available at the base level).
    add_executable(nostr-home-publisher
      tests/integration/nostr_home_publisher.c)
    target_link_libraries(nostr-home-publisher PRIVATE
      nostr_porthome nostr)
    target_compile_options(nostr-home-publisher PRIVATE
      -Wall -Wextra -Wno-misleading-indentation -Wno-unused-parameter)

    if(Python3_Interpreter_FOUND)
      add_test(NAME homed_porthome_smallhome
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_porthome_smallhome.py"
          "$<TARGET_FILE:porthome_smallhome_driver>")
      set_tests_properties(homed_porthome_smallhome PROPERTIES
        LABELS "nostr-homed;porthome;portable" TIMEOUT 180
        ENVIRONMENT "NH_PORTHOME_ALLOW_INSECURE=1")
    endif()

    # Phase 2: wrap-seed derivation vectors (nostrc-ck6i).
    add_executable(test_porthome_wrapkey tests/unit/test_porthome_wrapkey.c)
    target_link_libraries(test_porthome_wrapkey PRIVATE nostr_porthome)
    target_compile_options(test_porthome_wrapkey PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    add_test(NAME homed_porthome_wrapkey COMMAND test_porthome_wrapkey)
    set_tests_properties(homed_porthome_wrapkey PROPERTIES
      LABELS "nostr-homed;porthome;portable" TIMEOUT 30)

    # Phase 2: provisioner (materialize + LIMITED_MODE interlock).
    add_executable(test_porthome_provision tests/unit/test_porthome_provision.c)
    target_link_libraries(test_porthome_provision PRIVATE nostr_porthome)
    target_compile_options(test_porthome_provision PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    add_test(NAME homed_porthome_provision COMMAND test_porthome_provision)
    set_tests_properties(homed_porthome_provision PROPERTIES
      LABELS "nostr-homed;porthome;portable" TIMEOUT 60)

    # Phase 2.5B: fork+drop-privs sandbox around the fetch helper
    # (bead nostrc-ww50). Runs unprivileged (parent uid != 0) — the
    # sandbox recognises this via NH_PORTHOME_SANDBOX_ALLOW_NONROOT and
    # exercises fd-hygiene, no_new_privs, rlimit, and env allow-list
    # code paths without requiring a root test rig.
    add_executable(test_porthome_sandbox tests/unit/test_porthome_sandbox.c)
    target_link_libraries(test_porthome_sandbox PRIVATE nostr_porthome)
    target_compile_options(test_porthome_sandbox PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    add_test(NAME homed_porthome_sandbox COMMAND test_porthome_sandbox)
    set_tests_properties(homed_porthome_sandbox PROPERTIES
      LABELS "nostr-homed;porthome;portable;security" TIMEOUT 60
      ENVIRONMENT "NH_PORTHOME_SANDBOX_ALLOW_NONROOT=1")

    # Companion stub the sandbox integration test forks. It attempts a
    # trio of privileged actions (write /etc/passwd, bind :80, spin
    # CPU) and reports outcomes via its exit code. Kept small so a
    # single translation unit exercises three sandbox invariants
    # without dragging test infrastructure into the child.
    add_executable(porthome_sandbox_stub
      tests/integration/porthome_sandbox_stub.c)
    target_compile_options(porthome_sandbox_stub PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)

    add_executable(test_porthome_sandbox_integration
      tests/integration/test_porthome_sandbox_integration.c)
    target_link_libraries(test_porthome_sandbox_integration PRIVATE
      nostr_porthome)
    target_compile_options(test_porthome_sandbox_integration PRIVATE
      -Wall -Wextra -Werror -Wno-misleading-indentation)
    add_dependencies(test_porthome_sandbox_integration porthome_sandbox_stub)
    add_test(NAME homed_porthome_sandbox_integration
      COMMAND test_porthome_sandbox_integration
        $<TARGET_FILE:porthome_sandbox_stub>)
    set_tests_properties(homed_porthome_sandbox_integration PROPERTIES
      LABELS "nostr-homed;porthome;portable;security" TIMEOUT 60
      ENVIRONMENT "NH_PORTHOME_SANDBOX_ALLOW_NONROOT=1")

    # Phase 2: broker integration test (PROVISION_HOME + WAIT_HOME
    # dispatch, no relay). Requires the AUTH_RUNTIME so it links into
    # the same broker binary that includes auth_porthome.c.
    if(NOSTR_HOMED_ENABLE_AUTH_RUNTIME)
      add_executable(test_porthome_broker_wait
        tests/integration/test_porthome_broker_wait.c)
      target_link_libraries(test_porthome_broker_wait PRIVATE
        nostr_auth_runtime nostr_porthome)
      target_compile_options(test_porthome_broker_wait PRIVATE
        -Wall -Wextra -Werror -Wno-misleading-indentation)
      add_test(NAME homed_porthome_broker_wait
        COMMAND test_porthome_broker_wait)
      set_tests_properties(homed_porthome_broker_wait PROPERTIES
        LABELS "nostr-homed;porthome;portable;auth-runtime" TIMEOUT 60
        SKIP_RETURN_CODE 77)

      # Bead nostrc-3o91 — provisioner "provisioner" key writer:
      # state machine + peer-key preservation.
      add_executable(test_porthome_provisioner_status
        tests/unit/test_porthome_provisioner_status.c)
      target_link_libraries(test_porthome_provisioner_status PRIVATE
        nostr_auth_runtime)
      target_compile_options(test_porthome_provisioner_status PRIVATE
        -Wall -Wextra -Werror -Wno-misleading-indentation)
      add_test(NAME homed_porthome_provisioner_status
        COMMAND test_porthome_provisioner_status)
      set_tests_properties(homed_porthome_provisioner_status PROPERTIES
        LABELS "nostr-homed;porthome;portable;auth-runtime" TIMEOUT 30)
    endif()
  endif()
endif()


# ────────────────────────────────────────────────────────────────────
# Portable-home Phase 3 (bead nostrc-p6qp) — user-scoped sync daemon.
# EXPERIMENTAL. Gated behind NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL on
# top of NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL so a base build
# never drags libnostr / libhanami into nostr-authd / pam_nostr.
#
# Ships:
#   - libnostr_syncd_core (STATIC, non-installed): state, ignore
#     matcher, batcher, lock, interlocks, subscription seam and push
#     closure. Unit + integration tests link against this without
#     bringing in the inotify main loop.
#   - nostr-home-syncd (executable, non-installed in I1): the daemon.
#     I3 owns the systemd --user unit and any install rules.
# ────────────────────────────────────────────────────────────────────
if(NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL)
  if(NOT NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL)
    message(FATAL_ERROR
      "NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL requires NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)
  find_package(Threads REQUIRED)

  add_library(nostr_syncd_core STATIC
    src/porthome-syncd/nh_syncd_state.c
    src/porthome-syncd/nh_syncd_ignore.c
    src/porthome-syncd/nh_syncd_batcher.c
    src/porthome-syncd/nh_syncd_lock.c
    src/porthome-syncd/nh_syncd_interlocks.c
    src/porthome-syncd/nh_syncd_pusher.c
    src/porthome-syncd/nh_syncd_subscription.c
    # I3 (bead nostrc-p6qp): local blob cache + generation pin ring +
    # weekly HEAD sweep (§6.5). Compiled unconditionally when SYNCD is
    # ON so tests link against a single archive.
    src/porthome-syncd/nh_syncd_cache.c
    src/porthome-syncd/nh_syncd_pin_ring.c
    src/porthome-syncd/nh_syncd_sweep.c
    # I2 additions (bead nostrc-p6qp) — pull path + reconciler.
    src/porthome-syncd/nh_syncd_reconcile.c
    src/porthome-syncd/nh_syncd_pull.c
    # Phase 5 I3 / h10m.1.1 (bead nostrc-8hw8 + nostrc-h10m.1.1):
    # syncd contribution to porthome-status.json. Compiled into
    # nostr_syncd_core so both the daemon binary and unit tests
    # can drive it without touching the fuse writer.
    src/porthome-syncd/nh_syncd_status.c
    # Phase 4 P4-I (bead nostrc-1u55) — lazy-subtree prefix matcher.
    # Additive: an empty matcher never matches so behaviour is
    # preserved for callers that pass NULL for cfg->lazy.
    src/porthome-syncd/nh_syncd_lazy.c)
  target_compile_options(nostr_syncd_core PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter)
  target_compile_definitions(nostr_syncd_core PUBLIC
    NH_SYNCD_EXPERIMENTAL=1)
  target_include_directories(nostr_syncd_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(nostr_syncd_core PUBLIC
    nostr_porthome nostr_porthome_common PkgConfig::JANSSON Threads::Threads)
  # libnostr is required for the kind-30078 publish path and the
  # SimplePool subscription. In in-tree build the target `nostr` is
  # defined by libnostr's subdirectory; stand-alone builds fall back
  # to pkg-config.
  if(TARGET nostr)
    target_link_libraries(nostr_syncd_core PUBLIC nostr)
  else()
    pkg_check_modules(NOSTR_FOR_SYNCD REQUIRED IMPORTED_TARGET libnostr)
    target_link_libraries(nostr_syncd_core PUBLIC PkgConfig::NOSTR_FOR_SYNCD)
  endif()

  # Daemon binary. Only additional source is the inotify watcher +
  # the poll-loop main. Not installed in I1.
  add_executable(nostr-home-syncd
    src/porthome-syncd/nostr-home-syncd.c
    src/porthome-syncd/nh_syncd_watcher.c)
  target_compile_options(nostr-home-syncd PRIVATE
    -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter
    -Wno-format-truncation)
  target_include_directories(nostr-home-syncd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-syncd)
  target_link_libraries(nostr-home-syncd PRIVATE nostr_syncd_core)

  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_syncd_ignore  tests/unit/test_syncd_ignore.c)
    target_link_libraries(test_syncd_ignore  PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_ignore COMMAND test_syncd_ignore)
    set_tests_properties(homed_syncd_ignore PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 30)

    add_executable(test_syncd_batcher tests/unit/test_syncd_batcher.c)
    target_link_libraries(test_syncd_batcher PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_batcher COMMAND test_syncd_batcher)
    set_tests_properties(homed_syncd_batcher PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 30)

    add_executable(test_syncd_state tests/unit/test_syncd_state.c)
    target_link_libraries(test_syncd_state PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_state COMMAND test_syncd_state)
    set_tests_properties(homed_syncd_state PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 30)

    add_executable(test_syncd_push_e2e
      tests/integration/test_syncd_push_e2e.c)
    target_link_libraries(test_syncd_push_e2e PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_push_e2e COMMAND test_syncd_push_e2e)
    set_tests_properties(homed_syncd_push_e2e PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 120)

    # ── I3 (bead nostrc-p6qp) — cache + pin ring + sweep tests ──────
    add_executable(test_syncd_cache tests/unit/test_syncd_cache.c)
    target_link_libraries(test_syncd_cache PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_cache COMMAND test_syncd_cache)
    set_tests_properties(homed_syncd_cache PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 30)

    add_executable(test_syncd_cache_lru tests/unit/test_syncd_cache_lru.c)
    target_link_libraries(test_syncd_cache_lru PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_cache_lru COMMAND test_syncd_cache_lru)
    set_tests_properties(homed_syncd_cache_lru PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 30)

    # ── Phase 5 I3 (nostrc-8hw8) — quota + thrash + auto-eviction ──
    add_executable(test_syncd_quota tests/unit/test_syncd_quota.c)
    target_link_libraries(test_syncd_quota PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_quota COMMAND test_syncd_quota)
    set_tests_properties(homed_syncd_quota PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 30)

    # ── Phase 5 h10m.1.1 — daemon-side status writers + recent ring ──
    add_executable(test_porthome_status_daemon_writers
      tests/unit/test_porthome_status_daemon_writers.c)
    target_link_libraries(test_porthome_status_daemon_writers PRIVATE
      nostr_syncd_core)
    add_test(NAME homed_porthome_status_daemon_writers
      COMMAND test_porthome_status_daemon_writers)
    set_tests_properties(homed_porthome_status_daemon_writers PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 30)

    add_executable(test_syncd_pin_ring tests/unit/test_syncd_pin_ring.c)
    target_link_libraries(test_syncd_pin_ring PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_pin_ring COMMAND test_syncd_pin_ring)
    set_tests_properties(homed_syncd_pin_ring PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 30)

    # W(1) — daemon-wire coverage: promote_from_snapshot walks a
    # ${state_dir}/snapshot.json exactly like the daemon does after
    # nh_syncd_push_batch and nh_syncd_reconcile succeed.
    add_executable(test_syncd_pin_ring_from_snapshot
      tests/unit/test_syncd_pin_ring_from_snapshot.c)
    target_link_libraries(test_syncd_pin_ring_from_snapshot PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_pin_ring_from_snapshot
      COMMAND test_syncd_pin_ring_from_snapshot)
    set_tests_properties(homed_syncd_pin_ring_from_snapshot PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 30)

    add_executable(test_syncd_sweep tests/integration/test_syncd_sweep.c)
    target_link_libraries(test_syncd_sweep PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_sweep COMMAND test_syncd_sweep)
    set_tests_properties(homed_syncd_sweep PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 60)

    # W(2) — real-Blossom sweep operator probe (bead nostrc-p6qp).
    # NOT a CTest case: it hits real infra and must be operator-driven.
    add_executable(nostr-home-sweep-probe
      tests/integration/nostr_home_sweep_probe.c)
    target_link_libraries(nostr-home-sweep-probe PRIVATE
      nostr_syncd_core nostr_porthome nostr)
    target_compile_options(nostr-home-sweep-probe PRIVATE
      -Wall -Wextra -Wno-misleading-indentation -Wno-unused-parameter)

    # W(3) — broker-seed-drop <-> syncd handoff coverage (bead nostrc-p6qp).
    add_executable(test_syncd_broker_seed_drop
      tests/integration/test_syncd_broker_seed_drop.c)
    target_link_libraries(test_syncd_broker_seed_drop PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_broker_seed_drop
      COMMAND test_syncd_broker_seed_drop $<TARGET_FILE:nostr-home-syncd>)
    set_tests_properties(homed_syncd_broker_seed_drop PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 30)

    # OFF-closure verification: when AUTH_RUNTIME is also on, prove
    # nostr-authd has no nh_syncd_cache_ / nostr-home-syncd symbols.
    # (When AUTH_RUNTIME is off the check is a no-op — nothing to
    # inspect.)
    if(NOSTR_HOMED_ENABLE_AUTH_RUNTIME AND TARGET nostr-authd)
      add_test(NAME homed_syncd_off_closure
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_syncd_off_closure.sh
                $<TARGET_FILE:nostr-authd>)
      set_tests_properties(homed_syncd_off_closure PROPERTIES
        LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 15)
    endif()

    # I2 (bead nostrc-p6qp) — reconciler unit + pull integration.
    add_executable(test_syncd_reconcile tests/unit/test_syncd_reconcile.c)
    target_link_libraries(test_syncd_reconcile PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_reconcile COMMAND test_syncd_reconcile)
    set_tests_properties(homed_syncd_reconcile PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 60)

    # Phase 4 P4-I (bead nostrc-1u55) — lazy matcher unit + reconcile
    # additive-suppression regression.
    add_executable(test_syncd_lazy tests/unit/test_syncd_lazy.c)
    target_link_libraries(test_syncd_lazy PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_lazy COMMAND test_syncd_lazy)
    set_tests_properties(homed_syncd_lazy PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable-fuse;portable" TIMEOUT 30)

    add_executable(test_syncd_lazy_reconcile tests/unit/test_syncd_lazy_reconcile.c)
    target_link_libraries(test_syncd_lazy_reconcile PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_lazy_reconcile COMMAND test_syncd_lazy_reconcile)
    set_tests_properties(homed_syncd_lazy_reconcile PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable-fuse;portable" TIMEOUT 60)

    # ── xnxd part 1 — per-server upload accounting (multi-server pusher) ──
    add_executable(test_syncd_pusher_multi_server
      tests/unit/test_syncd_pusher_multi_server.c)
    target_link_libraries(test_syncd_pusher_multi_server PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_pusher_multi_server
      COMMAND test_syncd_pusher_multi_server)
    set_tests_properties(homed_syncd_pusher_multi_server PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 30)

    # ── xnxd part 2 — full-tree rescan diff (missed-inotify recovery) ──
    add_executable(test_syncd_rescan_overflow
      tests/unit/test_syncd_rescan_overflow.c)
    target_link_libraries(test_syncd_rescan_overflow PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_rescan_overflow
      COMMAND test_syncd_rescan_overflow)
    set_tests_properties(homed_syncd_rescan_overflow PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 30)

    add_executable(test_syncd_pull_e2e tests/integration/test_syncd_pull_e2e.c)
    target_link_libraries(test_syncd_pull_e2e PRIVATE nostr_syncd_core)
    add_test(NAME homed_syncd_pull_e2e COMMAND test_syncd_pull_e2e)
    set_tests_properties(homed_syncd_pull_e2e PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 120)
  endif()

  # ── I3 (bead nostrc-p6qp) — install rule for the daemon + user unit ──
  # Suggested package name for downstream packaging (Phase 3 wrap-up):
  #   Debian:  nostr-home-sync   (Provides: nostr-home-syncd)
  #   Fedora:  nostr-home-sync
  # Actual debian/ and rpm/ specs land under nostrc-rb0e; this block only
  # stages the payload at the FHS-correct locations.
  if(NOSTR_HOMED_ENABLE_AUTH_INSTALL)
    install(TARGETS nostr-home-syncd RUNTIME
            DESTINATION "${CMAKE_INSTALL_BINDIR}")

    pkg_get_variable(NH_SYNCD_USER_UNIT_DIR systemd systemduserunitdir)
    if(NOT NH_SYNCD_USER_UNIT_DIR)
      set(NH_SYNCD_USER_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/user")
    endif()
    set(NH_SYNCD_BIN_PATH "${CMAKE_INSTALL_FULL_BINDIR}/nostr-home-syncd")
    configure_file(systemd/user/nostr-home-sync.service.in
                   "${CMAKE_CURRENT_BINARY_DIR}/nostr-home-sync.service"
                   @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostr-home-sync.service"
            DESTINATION "${NH_SYNCD_USER_UNIT_DIR}")
    # The cache library is compiled into nostr_syncd_core (STATIC) —
    # it's part of the daemon binary, not a separately installed .so.
  endif()

  # Packaging smoke test (DESTDIR staging). Runs `cmake --install` into
  # a scratch DESTDIR and asserts the binary + unit landed at their
  # expected relative paths. Fast, hermetic, no root required.
  if(NOSTR_HOMED_BUILD_TESTS AND NOSTR_HOMED_ENABLE_AUTH_INSTALL)
    add_test(NAME homed_syncd_install_smoke
      COMMAND ${CMAKE_COMMAND} -E env
              CMAKE_BINARY_DIR=${CMAKE_BINARY_DIR}
              CMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
              NH_SYNCD_USER_UNIT_DIR=${NH_SYNCD_USER_UNIT_DIR}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_syncd_install_smoke.sh)
    set_tests_properties(homed_syncd_install_smoke PROPERTIES
      LABELS "nostr-homed;porthome;syncd;portable" TIMEOUT 60)
  endif()
endif()


# ────────────────────────────────────────────────────────────────────
# Portable-home Phase 4 (bead nostrc-1u55) — read-only FUSE overlay.
# EXPERIMENTAL. Gated behind NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL
# on top of NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL (which requires
# NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL). Nothing is built or linked
# into the base pam_nostr / nostr-authd closure when OFF. See
# docs/designs/nostrfs-porthome-overlay.md.
# ────────────────────────────────────────────────────────────────────
if(NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL)
  if(NOT NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL)
    message(FATAL_ERROR
      "NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL requires "
      "NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL (the FUSE overlay reuses "
      "the syncd cache/pin/manifest machinery).")
  endif()
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  find_package(FUSE3 REQUIRED)

  add_executable(nostr-home-fuse
    src/porthome-fuse/nostr-home-fuse.c
    src/porthome-fuse/nh_fuse_key.c
    src/porthome-fuse/nh_fuse_reload.c
    src/porthome-fuse/nh_fuse_source.c
    src/porthome-fuse/nh_fuse_status.c
    src/porthome-fuse/nh_fuse_status_writer.c
    src/porthome-fuse/nh_fuse_table.c)
  target_compile_definitions(nostr-home-fuse PRIVATE FUSE_USE_VERSION=31)
  target_compile_options(nostr-home-fuse PRIVATE
    -Wall -Wextra -Wno-error -Wno-misleading-indentation
    -Wno-unused-parameter -Wno-format-truncation)
  target_include_directories(nostr-home-fuse PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-fuse
    ${FUSE3_INCLUDE_DIRS})
  find_package(Threads REQUIRED)
  target_link_libraries(nostr-home-fuse PRIVATE
    nostr_syncd_core nostr_porthome nostr_porthome_common ${FUSE3_LIBRARIES}
    Threads::Threads)
  # sd_notify is optional — link when the systemd library is present.
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SYSTEMD QUIET IMPORTED_TARGET libsystemd)
    if(SYSTEMD_FOUND)
      target_link_libraries(nostr-home-fuse PRIVATE PkgConfig::SYSTEMD)
    endif()
  endif()

  if(NOSTR_HOMED_BUILD_TESTS)
    add_executable(test_fuse_table tests/unit/test_fuse_table.c
      src/porthome-fuse/nh_fuse_table.c)
    target_compile_options(test_fuse_table PRIVATE
      -Wall -Wextra -Wno-error -Wno-misleading-indentation
      -Wno-unused-parameter)
    target_include_directories(test_fuse_table PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-fuse)
    target_link_libraries(test_fuse_table PRIVATE nostr_syncd_core)
    add_test(NAME homed_fuse_table COMMAND test_fuse_table)
    set_tests_properties(homed_fuse_table PROPERTIES
      LABELS "nostr-homed;porthome;portable-fuse;portable" TIMEOUT 30)

    add_executable(test_fuse_source tests/unit/test_fuse_source.c
      src/porthome-fuse/nh_fuse_source.c)
    target_compile_options(test_fuse_source PRIVATE
      -Wall -Wextra -Wno-error -Wno-misleading-indentation
      -Wno-unused-parameter)
    target_include_directories(test_fuse_source PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-fuse)
    target_link_libraries(test_fuse_source PRIVATE
      nostr_syncd_core nostr_porthome)
    add_test(NAME homed_fuse_source COMMAND test_fuse_source)
    set_tests_properties(homed_fuse_source PROPERTIES
      LABELS "nostr-homed;porthome;portable-fuse;portable" TIMEOUT 60)

    add_executable(test_fuse_key tests/unit/test_fuse_key.c
      src/porthome-fuse/nh_fuse_key.c)
    target_compile_options(test_fuse_key PRIVATE
      -Wall -Wextra -Wno-error -Wno-misleading-indentation
      -Wno-unused-parameter)
    target_include_directories(test_fuse_key PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-fuse)
    target_link_libraries(test_fuse_key PRIVATE nostr_porthome)
    add_test(NAME homed_fuse_key COMMAND test_fuse_key)
    set_tests_properties(homed_fuse_key PROPERTIES
      LABELS "nostr-homed;porthome;portable-fuse;portable" TIMEOUT 30)

    # Regression: tier-2 AEAD failure must fall through to tier 3
    # (bead nostrc-zw8v). Wrong-bytes-for-hash cache entry + stub
    # Blossom serving the correct sealed bytes → read succeeds AND
    # the poisoned cache entry is evicted / replaced.
    add_executable(test_fuse_source_poison
      tests/unit/test_fuse_source_poison.c
      src/porthome-fuse/nh_fuse_source.c)
    target_compile_options(test_fuse_source_poison PRIVATE
      -Wall -Wextra -Wno-error -Wno-misleading-indentation
      -Wno-unused-parameter)
    target_include_directories(test_fuse_source_poison PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-fuse)
    target_link_libraries(test_fuse_source_poison PRIVATE
      nostr_syncd_core nostr_porthome)
    add_test(NAME homed_fuse_source_poison COMMAND test_fuse_source_poison)
    set_tests_properties(homed_fuse_source_poison PROPERTIES
      LABELS "nostr-homed;porthome;portable-fuse;portable" TIMEOUT 60)

    # Regression: fuse-status.json's mounted flag reflects the session
    # (bead nostrc-h4tv). Exercises the extracted writer with a fake
    # non-NULL session pointer and then with NULL to prove both
    # transitions. Full-mount coverage is deferred to the harness /
    # live-smoke run.
    add_executable(test_fuse_status
      tests/unit/test_fuse_status.c
      src/porthome-fuse/nh_fuse_status.c)
    target_compile_options(test_fuse_status PRIVATE
      -Wall -Wextra -Wno-error -Wno-misleading-indentation
      -Wno-unused-parameter)
    target_include_directories(test_fuse_status PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-fuse)
    # nh_fuse_source.h transitively pulls in libhanami's public headers;
    # linking nostr_porthome exports the include path we need.
    target_link_libraries(test_fuse_status PRIVATE nostr_porthome nostr_porthome_common)
    add_test(NAME homed_fuse_status COMMAND test_fuse_status)
    set_tests_properties(homed_fuse_status PROPERTIES
      LABELS "nostr-homed;porthome;portable-fuse;portable" TIMEOUT 30)

    # nostrc-k4j4 — evict-thrash callback + rich fuse status writer.
    add_executable(test_fuse_evict_notify
      tests/unit/test_fuse_evict_notify.c
      src/porthome-fuse/nh_fuse_status.c
      src/porthome-fuse/nh_fuse_status_writer.c)
    target_compile_options(test_fuse_evict_notify PRIVATE
      -Wall -Wextra -Wno-error -Wno-misleading-indentation
      -Wno-unused-parameter)
    target_include_directories(test_fuse_evict_notify PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-fuse)
    target_link_libraries(test_fuse_evict_notify PRIVATE
      nostr_syncd_core nostr_porthome nostr_porthome_common Threads::Threads)
    add_test(NAME homed_fuse_evict_notify COMMAND test_fuse_evict_notify)
    set_tests_properties(homed_fuse_evict_notify PROPERTIES
      LABELS "nostr-homed;porthome;portable-fuse;portable" TIMEOUT 60)

    # nostrc-plo4 — inotify-driven snapshot reload semantics.
    add_executable(test_fuse_snapshot_reload
      tests/unit/test_fuse_snapshot_reload.c
      src/porthome-fuse/nh_fuse_reload.c
      src/porthome-fuse/nh_fuse_table.c)
    target_compile_options(test_fuse_snapshot_reload PRIVATE
      -Wall -Wextra -Wno-error -Wno-misleading-indentation
      -Wno-unused-parameter)
    target_include_directories(test_fuse_snapshot_reload PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src/porthome-fuse)
    target_link_libraries(test_fuse_snapshot_reload PRIVATE
      nostr_syncd_core nostr_porthome nostr_porthome_common Threads::Threads)
    add_test(NAME homed_fuse_snapshot_reload COMMAND test_fuse_snapshot_reload)
    set_tests_properties(homed_fuse_snapshot_reload PROPERTIES
      LABELS "nostr-homed;porthome;portable-fuse;portable" TIMEOUT 60)

    # Integration: harness mode exercises the wiring without needing
    # /dev/fuse — CI-friendly. Ships alongside real-mount scripts that
    # SKIP when /dev/fuse or fusermount3 is absent.
    add_test(NAME homed_fuse_harness
      COMMAND ${CMAKE_COMMAND} -E env
              NH_FUSE_TEST_HARNESS=1
              NH_FUSE_STATE_DIR=${CMAKE_CURRENT_BINARY_DIR}/fuse_test_state
              NH_FUSE_MOUNTPOINT=${CMAKE_CURRENT_BINARY_DIR}/fuse_test_mp
              NH_FUSE_STATUS_DIR=${CMAKE_CURRENT_BINARY_DIR}/fuse_test_status
              NH_FUSE_SEED_HEX=1111111111111111111111111111111111111111111111111111111111111111
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_porthome_fuse_harness.sh
              $<TARGET_FILE:nostr-home-fuse>)
    set_tests_properties(homed_fuse_harness PROPERTIES
      LABELS "nostr-homed;porthome;portable-fuse;portable" TIMEOUT 60)

    # Live-acceptance operator tool (bead nostrc-1u55). Publishes a
    # fixture home to real Blossom + relay AND writes a snapshot.json
    # the FUSE mount can consume. Not a CTest case — operator-driven
    # for the porthome-fuse live smoke.
    add_executable(porthome-fuse-fixture
      tests/integration/porthome_fuse_fixture.c)
    target_compile_options(porthome-fuse-fixture PRIVATE
      -Wall -Wextra -Wno-error -Wno-misleading-indentation
      -Wno-unused-parameter)
    target_include_directories(porthome-fuse-fixture PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(porthome-fuse-fixture PRIVATE
      nostr_syncd_core nostr_porthome nostr)
  endif()

  if(NOSTR_HOMED_ENABLE_AUTH_INSTALL)
    install(TARGETS nostr-home-fuse RUNTIME
            DESTINATION "${CMAKE_INSTALL_BINDIR}")
    pkg_get_variable(NH_FUSE_USER_UNIT_DIR systemd systemduserunitdir)
    if(NOT NH_FUSE_USER_UNIT_DIR)
      set(NH_FUSE_USER_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/user")
    endif()
    set(NH_FUSE_BIN_PATH "${CMAKE_INSTALL_FULL_BINDIR}/nostr-home-fuse")
    configure_file(systemd/user/nostr-home-fuse.service.in
                   "${CMAKE_CURRENT_BINARY_DIR}/nostr-home-fuse.service"
                   @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostr-home-fuse.service"
            DESTINATION "${NH_FUSE_USER_UNIT_DIR}")
    install(FILES packaging/ignore.d/portable
            DESTINATION "${CMAKE_INSTALL_DATADIR}/nostr-homed/ignore.d")
  endif()
endif()

