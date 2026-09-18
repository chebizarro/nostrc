# nip-cas0010: NIP-CAS-0010 OTLP-over-Nostr transport primitives.
#
# Producer/consumer for OTLP payloads carried as ephemeral kinds 24900-24902.
# The module transports OPAQUE OTLP bytes; it never links an OTLP/protobuf
# implementation.

project(nostr_nip_cas0010 LANGUAGES C)

add_library(nostr_nip_cas0010_core OBJECT
    src/otel_base64.c
    src/otel_encoding.c
    src/otel_producer.c
    src/otel_consumer.c
    src/otel_replay.c
)

target_include_directories(nostr_nip_cas0010_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(Threads REQUIRED)
target_link_libraries(nostr_nip_cas0010_core PUBLIC nostr Threads::Threads)

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
    test_otel_replay
    test_otel_roundtrip
)

if(BUILD_TESTING)
    foreach(test_name ${NIP_CAS0010_TESTS})
        add_executable(${test_name} tests/${test_name}.c)
        target_include_directories(${test_name}
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_SOURCE_DIR}/libnostr/include
        )
        target_link_libraries(${test_name} nostr_nip_cas0010_core nostr)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

option(BUILD_NIP_CAS0010_EXAMPLES "Build NIP-CAS-0010 examples" ON)
if(BUILD_NIP_CAS0010_EXAMPLES)
    add_executable(otel_nostr_publish examples/otel_nostr_publish.c)
    target_include_directories(otel_nostr_publish
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/libnostr/include
    )
    target_link_libraries(otel_nostr_publish nostr_nip_cas0010_core nostr)
endif()
