cmake_minimum_required(VERSION 3.18)
project(nostr_homed VERSION 0.2.0 LANGUAGES C)

include(GNUInstallDirs)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(NOSTR_HOMED_ENABLE_AUTH_CORE
  "Build non-installed authentication protocol/provider core and tests" OFF)
option(NOSTR_HOMED_ENABLE_IDENTITY_CORE
  "Build non-installed identity authority/reader core and tests" OFF)
option(NOSTR_HOMED_ENABLE_DOMAIN_CONFIG
  "Install inert winbind/domain examples and validator (never activates PAM)" OFF)
option(NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING
  "Build the legacy FUSE/Blossom roaming-home experiment" OFF)
option(NOSTR_HOMED_ENABLE_AUTH_INSTALL
  "Install the system broker/PAM authentication product" OFF)
option(NOSTR_HOMED_BUILD_TESTS "Build tests for enabled nostr-homed features" ON)

if(NOSTR_HOMED_ENABLE_AUTH_INSTALL)
  message(FATAL_ERROR
    "nostr-homed authentication installation is not available yet: the broker, "
    "PAM client, peer binding, and activation policy are incomplete. Build "
    "NOSTR_HOMED_ENABLE_AUTH_CORE for portable tests only.")
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
    src/auth/auth_vault.c
    src/auth/provider_local.c)
  target_compile_options(nostr_auth_core PRIVATE -Wall -Wextra -Werror -Wno-misleading-indentation)
  target_include_directories(nostr_auth_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auth)
  target_link_libraries(nostr_auth_core PUBLIC
    ${_homed_nostr} PkgConfig::JANSSON OpenSSL::Crypto)

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
    src/identity/identity_projection.c)
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
    add_test(NAME homed_identity_store COMMAND test_identity_store)
    add_test(NAME homed_identity_config COMMAND test_identity_config)
    add_test(NAME homed_identity_fixtures COMMAND test_identity_fixtures)
    set_tests_properties(homed_identity_store homed_identity_config homed_identity_fixtures
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

# Version metadata only: there is no installed nostr-homed link library.
configure_file(nostr-homed.pc.in nostr-homed.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostr-homed.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
