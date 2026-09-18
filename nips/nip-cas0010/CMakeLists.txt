# nip-cas0010: NIP-CAS-0010 OTLP-over-Nostr transport primitives.
#
# Producer/consumer for OTLP payloads carried as ephemeral kinds 24900-24902.
# The module transports OPAQUE OTLP bytes; it never links an OTLP/protobuf
# implementation.

project(nostr_nip_cas0010 LANGUAGES C)

# SECURITY (G9): the TEST-ONLY local (raw private key) signer
# (src/otel_local_signer.c, nostr_otel_local_signer_*) is NOT part of the
# shipped library. It is compiled into the core object library only when this
# option is explicitly turned ON; the module's own tests get it as a
# test-local helper instead, so a default build exports no raw-key signing
# symbol. Production producers supply their own NostrOtelSignFn (Signet/NIP-46).
option(NOSTR_OTEL_ENABLE_TEST_SIGNER
       "Compile the TEST-ONLY nip-cas0010 local raw-key signer into the library" OFF)

add_library(nostr_nip_cas0010_core OBJECT
    src/otel_base64.c
    src/otel_encoding.c
    src/otel_producer.c
    src/otel_consumer.c
)

target_include_directories(nostr_nip_cas0010_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(nostr_nip_cas0010_core PUBLIC nostr)

if(NOSTR_OTEL_ENABLE_TEST_SIGNER)
    message(WARNING "nip-cas0010: TEST-ONLY local raw-key signer compiled into the library "
                    "(NOSTR_OTEL_ENABLE_TEST_SIGNER=ON) — do not ship this build")
    target_sources(nostr_nip_cas0010_core PRIVATE src/otel_local_signer.c)
    target_compile_definitions(nostr_nip_cas0010_core PUBLIC NOSTR_OTEL_ENABLE_TEST_SIGNER)
endif()

set_target_properties(nostr_nip_cas0010_core PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# Optional zstd body compression. `identity` is mandatory and always available;
# zstd is advertised only when libzstd is present at configure time.
option(ENABLE_NIP_CAS0010_ZSTD "Enable zstd bodies for NIP-CAS-0010" ON)
set(NIP_CAS0010_ZSTD_FOUND OFF)
if(ENABLE_NIP_CAS0010_ZSTD)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(NIP_CAS0010_ZSTD QUIET IMPORTED_TARGET libzstd)
    endif()
    if(TARGET PkgConfig::NIP_CAS0010_ZSTD)
        set(NIP_CAS0010_ZSTD_FOUND ON)
        target_link_libraries(nostr_nip_cas0010_core PUBLIC PkgConfig::NIP_CAS0010_ZSTD)
        target_compile_definitions(nostr_nip_cas0010_core PUBLIC NOSTR_OTEL_HAVE_ZSTD)
        message(STATUS "nip-cas0010: zstd bodies enabled (libzstd ${NIP_CAS0010_ZSTD_VERSION})")
    else()
        message(STATUS "nip-cas0010: libzstd not found — identity-only bodies")
    endif()
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/nostr/nip_cas0010/
        DESTINATION include/nostr/nip_cas0010
        FILES_MATCHING PATTERN "*.h")

set(NIP_CAS0010_TESTS
    test_otel_encoding
    test_otel_producer
    test_otel_consumer
    test_otel_roundtrip
)

if(BUILD_TESTING)
    foreach(test_name ${NIP_CAS0010_TESTS})
        set(test_sources tests/${test_name}.c)
        if(NOT NOSTR_OTEL_ENABLE_TEST_SIGNER)
            # Test-local copy of the raw-key signer: linked into the test binary
            # only, never into the library the fleet ships.
            list(APPEND test_sources src/otel_local_signer.c)
        endif()
        add_executable(${test_name} ${test_sources})
        target_compile_definitions(${test_name} PRIVATE NOSTR_OTEL_ENABLE_TEST_SIGNER)
        target_include_directories(${test_name}
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${CMAKE_SOURCE_DIR}/libnostr/include
        )
        target_link_libraries(${test_name} nostr_nip_cas0010_core nostr)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# Default OFF (G9): the example signs with the test-only local signer.
option(BUILD_NIP_CAS0010_EXAMPLES "Build NIP-CAS-0010 examples" OFF)
if(BUILD_NIP_CAS0010_EXAMPLES)
    set(otel_example_sources examples/otel_nostr_publish.c)
    if(NOT NOSTR_OTEL_ENABLE_TEST_SIGNER)
        list(APPEND otel_example_sources src/otel_local_signer.c)
    endif()
    add_executable(otel_nostr_publish ${otel_example_sources})
    target_compile_definitions(otel_nostr_publish PRIVATE NOSTR_OTEL_ENABLE_TEST_SIGNER)
    target_include_directories(otel_nostr_publish
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/libnostr/include
    )
    target_link_libraries(otel_nostr_publish nostr_nip_cas0010_core nostr)
endif()
