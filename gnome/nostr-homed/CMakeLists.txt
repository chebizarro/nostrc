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
option(NOSTR_HOMED_BUILD_TESTS "Build tests for enabled nostr-homed features" ON)

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

  add_library(nostr_auth_core STATIC
    src/auth/auth_protocol.c
    src/auth/auth_transaction.c
    src/auth/auth_challenge.c
    src/auth/auth_ratelimit.c
    src/auth/auth_vault.c
    src/auth/provider_local.c
    src/auth/provider_nip46.c)
  target_compile_options(nostr_auth_core PRIVATE -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr_auth_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auth)
  target_link_libraries(nostr_auth_core PUBLIC
    ${_homed_nostr} PkgConfig::JANSSON OpenSSL::Crypto)
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

    add_test(NAME homed_auth_protocol COMMAND test_auth_protocol)
    add_test(NAME homed_auth_transaction COMMAND test_auth_transaction)
    add_test(NAME homed_auth_vault COMMAND test_auth_vault)
    add_test(NAME homed_auth_challenge COMMAND test_auth_challenge)
    add_test(NAME homed_provider_local COMMAND test_provider_local)
    set_tests_properties(
      homed_auth_protocol homed_auth_transaction homed_auth_vault homed_auth_challenge homed_provider_local
      PROPERTIES LABELS "nostr-homed;portable;auth" TIMEOUT 60)
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
  install(TARGETS nss_nostr LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  install(FILES config/nss.conf.sample
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
  add_library(nostr_auth_runtime STATIC
    src/auth/auth_peer.c src/auth/auth_client.c src/auth/auth_broker.c)
  target_compile_options(nostr_auth_runtime PRIVATE -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr_auth_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/auth)
  target_link_libraries(nostr_auth_runtime PUBLIC nostr_auth_core nostr_identity_core)
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
    add_executable(nh-seed-authority tests/integration/seed_authority.c)
    target_link_libraries(nh-seed-authority PRIVATE nostr_auth_runtime)
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
  add_library(pam_nostr MODULE src/pam/pam_nostr_broker.c)
  set_target_properties(pam_nostr PROPERTIES PREFIX "" OUTPUT_NAME "pam_nostr")
  target_compile_options(pam_nostr PRIVATE -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(pam_nostr PRIVATE ${PAM_INCLUDE_DIR})
  target_link_libraries(pam_nostr PRIVATE nostr_auth_runtime ${PAM_LIBRARY})
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
  configure_file(systemd/nostr-authd.service.in
    "${CMAKE_CURRENT_BINARY_DIR}/nostr-authd.service" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostr-authd.service"
    DESTINATION "${SYSTEMD_SYSTEM_UNIT_DIR}")
  install(FILES config/auth.conf.sample
    DESTINATION "${CMAKE_INSTALL_DATADIR}/nostr-homed")
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
  # Install: intentionally not wired — desktop packaging owns final layout.
  # A follow-up may install to CMAKE_INSTALL_BINDIR under a packaging opt.
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
