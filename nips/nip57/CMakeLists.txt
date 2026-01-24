project(nip57)

# Vendored nostrdb path (matches libnostr configuration)
set(LIBNOSTR_NDB_VENDOR_PATH "${CMAKE_SOURCE_DIR}/third_party/nostrdb" CACHE PATH "Path to vendored nostrdb source")

# Core NIP-57 library (Lightning Zaps)
add_library(nostr_nip57_core OBJECT
    src/nip57.c
)

target_include_directories(nostr_nip57_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
    PRIVATE
        # BOLT11 parsing requires nostrdb headers
        "${LIBNOSTR_NDB_VENDOR_PATH}/src"
        "${LIBNOSTR_NDB_VENDOR_PATH}/src/bolt11"
        "${LIBNOSTR_NDB_VENDOR_PATH}/ccan"
        "${LIBNOSTR_NDB_VENDOR_PATH}/ccan/ccan"
)

target_link_libraries(nostr_nip57_core
    PUBLIC
        nostr
)

# Install headers
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include
        FILES_MATCHING PATTERN "*.h")

# Tests
if(BUILD_TESTING)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip57.c)
        add_executable(test_nip57 tests/test_nip57.c)
        target_include_directories(test_nip57
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/libnostr/include
        )
        target_link_libraries(test_nip57
            nostr_nip57_core
            nostr
        )
        add_test(NAME test_nip57 COMMAND test_nip57)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip57_zap_request.c)
        add_executable(test_nip57_zap_request tests/test_nip57_zap_request.c)
        target_include_directories(test_nip57_zap_request
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/libnostr/include
        )
        target_link_libraries(test_nip57_zap_request
            nostr_nip57_core
            nostr
        )
        add_test(NAME test_nip57_zap_request COMMAND test_nip57_zap_request)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip57_zap_receipt.c)
        add_executable(test_nip57_zap_receipt tests/test_nip57_zap_receipt.c)
        target_include_directories(test_nip57_zap_receipt
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/libnostr/include
        )
        target_link_libraries(test_nip57_zap_receipt
            nostr_nip57_core
            nostr
        )
        add_test(NAME test_nip57_zap_receipt COMMAND test_nip57_zap_receipt)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip57_lnurl.c)
        add_executable(test_nip57_lnurl tests/test_nip57_lnurl.c)
        target_include_directories(test_nip57_lnurl
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/libnostr/include
        )
        target_link_libraries(test_nip57_lnurl
            nostr_nip57_core
            nostr
        )
        add_test(NAME test_nip57_lnurl COMMAND test_nip57_lnurl)
    endif()
endif()

# Examples
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/zap_example.c)
    add_executable(nip57_zap_example examples/zap_example.c)
    target_include_directories(nip57_zap_example
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/libnostr/include
    )
    target_link_libraries(nip57_zap_example
        nostr_nip57_core
        nostr
    )
endif()
