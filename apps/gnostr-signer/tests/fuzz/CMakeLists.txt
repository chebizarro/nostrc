# Fuzz testing targets for gnostr-signer
#
# This builds fuzz targets for security-critical components:
# - NIP-49 encryption/decryption
# - BIP-39 mnemonic parsing
# - JSON event parsing
#
# Issue: nostrc-p7f6
#
# Usage:
#   cmake -DBUILD_FUZZ_TARGETS=ON -DCMAKE_C_COMPILER=clang ..
#   cmake --build . --target fuzz-nip49
#   ./tests/fuzz/fuzz-nip49 tests/fuzz/corpus/nip49
#
# For AFL++:
#   cmake -DBUILD_FUZZ_TARGETS=ON -DFUZZ_ENGINE=AFL ..

cmake_minimum_required(VERSION 3.30)

# Fuzz targets are optional and off by default
option(BUILD_FUZZ_TARGETS "Build fuzz testing targets" OFF)
option(FUZZ_ENGINE "Fuzzing engine: LIBFUZZER (default) or AFL" "LIBFUZZER")

if(NOT BUILD_FUZZ_TARGETS)
    return()
endif()

message(STATUS "Building fuzz targets with engine: ${FUZZ_ENGINE}")

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)

# Common include directories
set(FUZZ_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_SOURCE_DIR}/nips/nip49/include
    ${CMAKE_SOURCE_DIR}/nips/nip19/include
    ${CMAKE_SOURCE_DIR}/libnostr/include
)

# Base libraries for all fuzz targets
set(FUZZ_BASE_LIBS
    PkgConfig::GLIB
)

# Setup compiler flags for fuzzing
if(FUZZ_ENGINE STREQUAL "LIBFUZZER")
    # Require Clang for libFuzzer
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "libFuzzer requires Clang compiler. Set CMAKE_C_COMPILER=clang")
    endif()

    set(FUZZ_COMPILE_FLAGS
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
        -g
        -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    )
    set(FUZZ_LINK_FLAGS
        -fsanitize=fuzzer,address,undefined
    )
elseif(FUZZ_ENGINE STREQUAL "AFL")
    # AFL++ instrumentation
    if(NOT AFL_CC)
        find_program(AFL_CC afl-clang-fast afl-clang-lto afl-gcc)
        if(NOT AFL_CC)
            message(FATAL_ERROR "AFL++ compiler not found. Install AFL++ or set AFL_CC")
        endif()
    endif()

    set(CMAKE_C_COMPILER ${AFL_CC})
    set(FUZZ_COMPILE_FLAGS
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -g
    )
    set(FUZZ_LINK_FLAGS
        -fsanitize=address,undefined
    )
else()
    message(FATAL_ERROR "Unknown FUZZ_ENGINE: ${FUZZ_ENGINE}. Use LIBFUZZER or AFL")
endif()

# Helper function to create fuzz targets
function(add_fuzz_target target_name source_file)
    add_executable(${target_name} ${source_file})
    target_include_directories(${target_name} PRIVATE ${FUZZ_INCLUDE_DIRS})
    target_compile_options(${target_name} PRIVATE ${FUZZ_COMPILE_FLAGS})
    target_link_options(${target_name} PRIVATE ${FUZZ_LINK_FLAGS})
    target_compile_features(${target_name} PRIVATE c_std_11)

    # Link base libraries
    target_link_libraries(${target_name} PRIVATE ${FUZZ_BASE_LIBS})

    # Link additional libraries from ARGN
    if(ARGN)
        target_link_libraries(${target_name} PRIVATE ${ARGN})
    endif()

    # Copy corpus directory if it exists
    set(CORPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${target_name})
    if(EXISTS ${CORPUS_DIR})
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CORPUS_DIR}
                ${CMAKE_CURRENT_BINARY_DIR}/corpus/${target_name}
            COMMENT "Copying corpus for ${target_name}"
        )
    endif()
endfunction()

# ==============================================================================
# Fuzz Target: NIP-49 Encryption/Decryption
# ==============================================================================
set(FUZZ_NIP49_LIBS)
if(TARGET nostr_nip49_core)
    list(APPEND FUZZ_NIP49_LIBS nostr_nip49_core)
endif()
if(TARGET nip19)
    list(APPEND FUZZ_NIP49_LIBS nip19)
endif()

add_fuzz_target(fuzz-nip49 fuzz_nip49.c ${FUZZ_NIP49_LIBS})

# Copy nip49 corpus
add_custom_command(TARGET fuzz-nip49 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/corpus/nip49
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/corpus/nip49
        ${CMAKE_CURRENT_BINARY_DIR}/corpus/nip49
    COMMENT "Copying NIP-49 corpus"
)

# ==============================================================================
# Fuzz Target: BIP-39 Mnemonic Parsing
# ==============================================================================
set(FUZZ_BIP39_LIBS)
if(TARGET nostr)
    list(APPEND FUZZ_BIP39_LIBS nostr)
elseif(TARGET libnostr)
    list(APPEND FUZZ_BIP39_LIBS libnostr)
endif()

add_fuzz_target(fuzz-bip39 fuzz_bip39.c ${FUZZ_BIP39_LIBS})

# Copy bip39 corpus
add_custom_command(TARGET fuzz-bip39 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/corpus/bip39
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/corpus/bip39
        ${CMAKE_CURRENT_BINARY_DIR}/corpus/bip39
    COMMENT "Copying BIP-39 corpus"
)

# ==============================================================================
# Fuzz Target: JSON Event Parsing
# ==============================================================================
set(FUZZ_JSON_LIBS
    PkgConfig::JSON_GLIB
)

add_fuzz_target(fuzz-json-event fuzz_json_event.c ${FUZZ_JSON_LIBS})

# Copy json_event corpus
add_custom_command(TARGET fuzz-json-event POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/corpus/json_event
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/corpus/json_event
        ${CMAKE_CURRENT_BINARY_DIR}/corpus/json_event
    COMMENT "Copying JSON event corpus"
)

# ==============================================================================
# Convenience target for all fuzz targets
# ==============================================================================
add_custom_target(fuzz-targets
    DEPENDS fuzz-nip49 fuzz-bip39 fuzz-json-event
    COMMENT "Building all fuzz targets"
)

# ==============================================================================
# Fuzz run helper scripts
# ==============================================================================
# Generate a script to run all fuzz targets
set(FUZZ_RUN_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/run-fuzz.sh)
file(WRITE ${FUZZ_RUN_SCRIPT} "#!/bin/bash
# Auto-generated fuzz runner script
# Issue: nostrc-p7f6

set -e

FUZZ_DIR=\"\$(dirname \"\$0\")\"
TIMEOUT=\${FUZZ_TIMEOUT:-60}
JOBS=\${FUZZ_JOBS:-1}

echo \"Running fuzz tests with timeout=\${TIMEOUT}s, jobs=\${JOBS}\"

# NIP-49 fuzzing
echo \"\\n=== Fuzzing NIP-49 ===\"
\"\${FUZZ_DIR}/fuzz-nip49\" \"\${FUZZ_DIR}/corpus/nip49\" \\
    -max_total_time=\${TIMEOUT} \\
    -jobs=\${JOBS} \\
    -workers=\${JOBS} \\
    2>&1 | tail -20

# BIP-39 fuzzing
echo \"\\n=== Fuzzing BIP-39 ===\"
\"\${FUZZ_DIR}/fuzz-bip39\" \"\${FUZZ_DIR}/corpus/bip39\" \\
    -max_total_time=\${TIMEOUT} \\
    -jobs=\${JOBS} \\
    -workers=\${JOBS} \\
    2>&1 | tail -20

# JSON event fuzzing
echo \"\\n=== Fuzzing JSON Event ===\"
\"\${FUZZ_DIR}/fuzz-json-event\" \"\${FUZZ_DIR}/corpus/json_event\" \\
    -max_total_time=\${TIMEOUT} \\
    -jobs=\${JOBS} \\
    -workers=\${JOBS} \\
    2>&1 | tail -20

echo \"\\nFuzz testing completed successfully\"
")

# Make the script executable after build
add_custom_command(TARGET fuzz-targets POST_BUILD
    COMMAND chmod +x ${FUZZ_RUN_SCRIPT}
    COMMENT "Making fuzz runner script executable"
)

message(STATUS "Fuzz targets configured:")
message(STATUS "  - fuzz-nip49: NIP-49 encryption/decryption")
message(STATUS "  - fuzz-bip39: BIP-39 mnemonic parsing")
message(STATUS "  - fuzz-json-event: JSON event parsing")
message(STATUS "  - fuzz-targets: Build all fuzz targets")
message(STATUS "")
message(STATUS "Run with: ./fuzz-nip49 corpus/nip49 -max_total_time=60")
