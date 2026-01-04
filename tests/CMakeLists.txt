cmake_minimum_required(VERSION 3.30)
project(nostrc-tests C)

# Optional fuzzing support
option(ENABLE_FUZZING "Build fuzzing targets" OFF)
option(ENABLE_FUZZING_RUNTIME "Link libFuzzer runtime (requires Clang libclang_rt.fuzzer)" OFF)

# Unit/integration test binaries for nostrc. Uses CTest (enabled at the root).

enable_testing()
# Directory-wide fallback timeout (seconds) so no test can run forever
set_property(DIRECTORY PROPERTY TEST_TIMEOUT 90)

# Sanitizer helper (Debug builds)
function(apply_sanitizers target)
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    if (GO_ENABLE_TSAN)
      target_compile_options(${target} PRIVATE -fsanitize=thread)
      target_link_options(${target} PRIVATE -fsanitize=thread)
    else()
      if (GO_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address)
        target_link_options(${target} PRIVATE -fsanitize=address)
      endif()
      if (GO_ENABLE_UBSAN)
        target_compile_options(${target} PRIVATE -fsanitize=undefined)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
      endif()
    endif()
  endif()
endfunction()

# Find OpenSSL for hashing/crypto used by tests
find_package(OpenSSL REQUIRED)
if (OpenSSL_FOUND)
    message(STATUS "Found OpenSSL version ${OpenSSL_VERSION}")
else()
    message(FATAL_ERROR "OpenSSL not found")
endif()

# Find secp256k1 (assuming pkg-config is available)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)
if (SECP256K1_FOUND)
    message(STATUS "Found secp256k1 version ${SECP256K1_VERSION}")
else()
    message(FATAL_ERROR "libsecp256k1 not found")
endif()

# Find the nsync library and headers manually for synchronization primitives
find_library(NSYNC_LIB nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)

# Include the nsync headers
include_directories(${NSYNC_INCLUDE_DIR})


# Make Jansson available to all test targets
link_libraries(PkgConfig::JANSSON)
include_directories(${JANSSON_INCLUDE_DIRS})
link_directories(${JANSSON_LIBRARY_DIRS})

# Test executables
add_executable(test_nostr tests_nostr.c)

add_executable(test_relay test_relay.c)
add_executable(test_relay_shutdown test_relay_shutdown.c)
add_executable(test_subscription_lifecycle test_subscription_lifecycle.c)
add_executable(test_subscription_backpressure test_subscription_backpressure.c)
add_executable(test_subscription_backpressure_long test_subscription_backpressure_long.c)
add_executable(test_subscription_blocking_depth test_subscription_blocking_depth.c)
add_executable(test_relay_unsubscribe test_relay_unsubscribe.c)
add_executable(test_connection_shutdown_order test_connection_shutdown_order.c)
add_executable(test_envelope_parse test_envelope_parse.c)
add_executable(test_json_event test_json_event.c)
add_executable(test_json_event_fuzzlite test_json_event_fuzzlite.c)
add_executable(test_json_envelope test_json_envelope.c)
add_executable(test_json_filter test_json_filter.c)
add_executable(test_json_filter_tags test_json_filter_tags.c)
add_executable(test_json_filter_robust test_json_filter_robust.c)
add_executable(test_json_filter_fuzzlite test_json_filter_fuzzlite.c)
add_executable(test_json_int_array test_json_int_array.c)
add_executable(test_json_raw test_json_raw.c)
add_executable(test_filters_move_semantics test_filters_move_semantics.c)
add_executable(microbench_json microbench_json.c)
add_executable(test_nip86_policy test_nip86_policy.c)

# New tests for event canonicalization and NIP-04 AEAD
add_executable(test_event_canonical test_event_canonical.c)
add_executable(test_nip04_aead test_nip04_aead.c)
add_executable(test_relay_ingress_policy test_relay_ingress_policy.c)

# Link OpenSSL, libsecp256k1, nsync, and project libraries
target_link_libraries(test_nostr PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})

target_link_libraries(test_relay PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_relay_shutdown PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_subscription_lifecycle PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_subscription_backpressure PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_subscription_backpressure_long PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_subscription_blocking_depth PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_relay_unsubscribe PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_connection_shutdown_order PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_envelope_parse PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_json_event PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_json_event_fuzzlite PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_json_envelope PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_json_filter PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_json_filter_tags PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_json_filter_robust PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_json_filter_fuzzlite PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES} ${JANSSON_LIBRARIES})
target_link_libraries(test_json_int_array PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_json_raw PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_filters_move_semantics PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(microbench_json PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_nip86_policy PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES} nip86)
target_include_directories(test_nip86_policy PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/nips/nip86/include)
target_link_directories(test_nip86_policy PRIVATE ${SECP256K1_LIBRARY_DIRS})

# Link new tests
target_link_libraries(test_event_canonical PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_nip04_aead PRIVATE nip04 libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_include_directories(test_nip04_aead PRIVATE ${CMAKE_SOURCE_DIR}/nips/nip04/include)
target_link_libraries(test_relay_ingress_policy PRIVATE relayd_core libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_include_directories(test_relay_ingress_policy PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/apps/relayd/include)

# Apply sanitizers to tests if enabled
apply_sanitizers(test_nostr)
apply_sanitizers(test_relay)
apply_sanitizers(test_relay_shutdown)
apply_sanitizers(test_subscription_lifecycle)
apply_sanitizers(test_subscription_backpressure)
apply_sanitizers(test_subscription_backpressure_long)
apply_sanitizers(test_subscription_blocking_depth)
apply_sanitizers(test_relay_unsubscribe)
apply_sanitizers(test_connection_shutdown_order)
apply_sanitizers(test_envelope_parse)
apply_sanitizers(test_json_event)
apply_sanitizers(test_json_event_fuzzlite)
apply_sanitizers(test_json_envelope)
apply_sanitizers(test_json_filter)
apply_sanitizers(test_json_filter_tags)
apply_sanitizers(test_json_filter_robust)
apply_sanitizers(test_json_filter_fuzzlite)
apply_sanitizers(test_json_int_array)
apply_sanitizers(test_json_raw)
apply_sanitizers(test_filters_move_semantics)
apply_sanitizers(microbench_json)
apply_sanitizers(test_event_canonical)
apply_sanitizers(test_nip04_aead)
apply_sanitizers(test_relay_ingress_policy)

# Register tests for CTest. Run with: `ctest --output-on-failure`
add_test(NAME test_nostr COMMAND test_nostr)
add_test(NAME test_relay COMMAND test_relay)
add_test(NAME test_relay_shutdown COMMAND test_relay_shutdown)
add_test(NAME test_subscription_lifecycle COMMAND test_subscription_lifecycle)
add_test(NAME test_subscription_backpressure COMMAND test_subscription_backpressure)
add_test(NAME test_subscription_backpressure_long COMMAND test_subscription_backpressure_long)
add_test(NAME test_subscription_blocking_depth COMMAND test_subscription_blocking_depth)
add_test(NAME test_relay_unsubscribe COMMAND test_relay_unsubscribe)
add_test(NAME test_connection_shutdown_order COMMAND test_connection_shutdown_order)
add_test(NAME test_envelope_parse COMMAND test_envelope_parse)
add_test(NAME test_json_event COMMAND test_json_event)
add_test(NAME test_json_event_fuzzlite COMMAND test_json_event_fuzzlite)

# LibFuzzer target for event deserialization (optional)
if(ENABLE_FUZZING)
  if(ENABLE_FUZZING_RUNTIME)
    add_executable(fuzz_event_deserialize fuzz_event_deserialize.c)
    target_link_libraries(fuzz_event_deserialize PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
    target_include_directories(fuzz_event_deserialize PRIVATE ${SECP256K1_INCLUDE_DIRS})
    target_link_directories(fuzz_event_deserialize PRIVATE ${SECP256K1_LIBRARY_DIRS})
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(fuzz_event_deserialize PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
      target_link_options(fuzz_event_deserialize PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
    # Run with corpus directory, e.g.:
    #   ./fuzz_event_deserialize ${CMAKE_SOURCE_DIR}/tests/fuzz_seeds/events -runs=0

    add_executable(fuzz_envelope_deserialize fuzz_envelope_deserialize.c)
    target_link_libraries(fuzz_envelope_deserialize PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
    target_include_directories(fuzz_envelope_deserialize PRIVATE ${SECP256K1_INCLUDE_DIRS})
    target_link_directories(fuzz_envelope_deserialize PRIVATE ${SECP256K1_LIBRARY_DIRS})
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(fuzz_envelope_deserialize PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
      target_link_options(fuzz_envelope_deserialize PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
    # Run with corpus directory, e.g.:
    #   ./fuzz_envelope_deserialize ${CMAKE_SOURCE_DIR}/tests/fuzz_seeds/filters -runs=0
  else()
    add_executable(fuzz_event_driver fuzz_event_deserialize.c fuzz_driver_main.c)
    target_link_libraries(fuzz_event_driver PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
    target_include_directories(fuzz_event_driver PRIVATE ${SECP256K1_INCLUDE_DIRS})
    target_link_directories(fuzz_event_driver PRIVATE ${SECP256K1_LIBRARY_DIRS})
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(fuzz_event_driver PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(fuzz_event_driver PRIVATE -fsanitize=address,undefined)
    endif()
    # Run with a corpus directory or file list, e.g.:
    #   ./fuzz_event_driver ${CMAKE_SOURCE_DIR}/tests/fuzz_seeds/events

    add_executable(fuzz_envelope_driver fuzz_envelope_deserialize.c fuzz_driver_main.c)
    target_link_libraries(fuzz_envelope_driver PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
    target_include_directories(fuzz_envelope_driver PRIVATE ${SECP256K1_INCLUDE_DIRS})
    target_link_directories(fuzz_envelope_driver PRIVATE ${SECP256K1_LIBRARY_DIRS})
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(fuzz_envelope_driver PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(fuzz_envelope_driver PRIVATE -fsanitize=address,undefined)
    endif()
    # Run with a corpus directory or file list, e.g.:
    #   ./fuzz_envelope_driver ${CMAKE_SOURCE_DIR}/tests/fuzz_seeds/filters
  endif()
endif()

# Filter parser fuzz harness
if(ENABLE_FUZZING)
  if(ENABLE_FUZZING_RUNTIME)
    add_executable(fuzz_filter_parse fuzz_filter_parse.c)
    target_link_libraries(fuzz_filter_parse PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
    target_include_directories(fuzz_filter_parse PRIVATE ${SECP256K1_INCLUDE_DIRS})
    target_link_directories(fuzz_filter_parse PRIVATE ${SECP256K1_LIBRARY_DIRS})
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(fuzz_filter_parse PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
      target_link_options(fuzz_filter_parse PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
  else()
    add_executable(fuzz_filter_parse_driver fuzz_filter_parse.c fuzz_driver_main.c)
    target_link_libraries(fuzz_filter_parse_driver PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
    target_include_directories(fuzz_filter_parse_driver PRIVATE ${SECP256K1_INCLUDE_DIRS})
    target_link_directories(fuzz_filter_parse_driver PRIVATE ${SECP256K1_LIBRARY_DIRS})
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(fuzz_filter_parse_driver PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(fuzz_filter_parse_driver PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endif()

# Verify wrapper fuzz harness
if(ENABLE_FUZZING)
  if(ENABLE_FUZZING_RUNTIME)
    add_executable(fuzz_verify_wrapper fuzz_verify_wrapper.c)
    target_link_libraries(fuzz_verify_wrapper PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
    target_include_directories(fuzz_verify_wrapper PRIVATE ${SECP256K1_INCLUDE_DIRS})
    target_link_directories(fuzz_verify_wrapper PRIVATE ${SECP256K1_LIBRARY_DIRS})
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(fuzz_verify_wrapper PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
      target_link_options(fuzz_verify_wrapper PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
  else()
    add_executable(fuzz_verify_wrapper_driver fuzz_verify_wrapper.c fuzz_driver_main.c)
    target_link_libraries(fuzz_verify_wrapper_driver PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
    target_include_directories(fuzz_verify_wrapper_driver PRIVATE ${SECP256K1_INCLUDE_DIRS})
    target_link_directories(fuzz_verify_wrapper_driver PRIVATE ${SECP256K1_LIBRARY_DIRS})
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(fuzz_verify_wrapper_driver PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(fuzz_verify_wrapper_driver PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endif()

# Additional compact-event fuzz harness (libFuzzer runtime recommended)
if(ENABLE_FUZZING)
  if(ENABLE_FUZZING_RUNTIME)
    add_executable(fuzz_event_parse fuzz_event_parse.c)
    target_link_libraries(fuzz_event_parse PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
    target_include_directories(fuzz_event_parse PRIVATE ${SECP256K1_INCLUDE_DIRS})
    target_link_directories(fuzz_event_parse PRIVATE ${SECP256K1_LIBRARY_DIRS})
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(fuzz_event_parse PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
      target_link_options(fuzz_event_parse PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
  else()
    add_executable(fuzz_event_parse_driver fuzz_event_parse.c fuzz_driver_main.c)
    target_link_libraries(fuzz_event_parse_driver PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
    target_include_directories(fuzz_event_parse_driver PRIVATE ${SECP256K1_INCLUDE_DIRS})
    target_link_directories(fuzz_event_parse_driver PRIVATE ${SECP256K1_LIBRARY_DIRS})
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(fuzz_event_parse_driver PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(fuzz_event_parse_driver PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endif()
add_test(NAME test_json_envelope COMMAND test_json_envelope)
add_test(NAME test_json_filter COMMAND test_json_filter)
add_test(NAME test_json_filter_tags COMMAND test_json_filter_tags)
add_test(NAME test_json_filter_robust COMMAND test_json_filter_robust)
add_test(NAME test_json_filter_fuzzlite COMMAND test_json_filter_fuzzlite)
add_test(NAME test_json_int_array COMMAND test_json_int_array)
add_test(NAME test_json_raw COMMAND test_json_raw)
add_test(NAME test_filters_move_semantics COMMAND test_filters_move_semantics)
add_test(NAME test_nip86_policy COMMAND test_nip86_policy)
add_test(NAME test_event_canonical COMMAND test_event_canonical)
add_test(NAME test_nip04_aead COMMAND test_nip04_aead)
add_test(NAME test_relay_ingress_policy COMMAND test_relay_ingress_policy)

# Per-test timeouts (seconds) — keep tight to surface stalls early
set_tests_properties(test_nostr PROPERTIES TIMEOUT 20)
set_tests_properties(test_relay PROPERTIES TIMEOUT 30)
set_tests_properties(test_relay_shutdown PROPERTIES TIMEOUT 30)
set_tests_properties(test_subscription_lifecycle PROPERTIES TIMEOUT 30)
set_tests_properties(test_subscription_backpressure PROPERTIES TIMEOUT 45)
set_tests_properties(test_subscription_backpressure_long PROPERTIES TIMEOUT 90)
set_tests_properties(test_subscription_blocking_depth PROPERTIES TIMEOUT 45)
set_tests_properties(test_relay_unsubscribe PROPERTIES TIMEOUT 30)
set_tests_properties(test_connection_shutdown_order PROPERTIES TIMEOUT 30)
set_tests_properties(test_envelope_parse PROPERTIES TIMEOUT 20)
set_tests_properties(test_json_event PROPERTIES TIMEOUT 20)
set_tests_properties(test_json_event_fuzzlite PROPERTIES TIMEOUT 30)
set_tests_properties(test_json_envelope PROPERTIES TIMEOUT 20)
set_tests_properties(test_json_filter PROPERTIES TIMEOUT 20)
set_tests_properties(test_json_filter_tags PROPERTIES TIMEOUT 20)
set_tests_properties(test_json_filter_robust PROPERTIES TIMEOUT 45)
set_tests_properties(test_json_filter_fuzzlite PROPERTIES TIMEOUT 45)
set_tests_properties(test_json_int_array PROPERTIES TIMEOUT 20)
set_tests_properties(test_json_raw PROPERTIES TIMEOUT 20)
set_tests_properties(test_filters_move_semantics PROPERTIES TIMEOUT 20)
set_tests_properties(test_event_canonical PROPERTIES TIMEOUT 20)
set_tests_properties(test_nip04_aead PROPERTIES TIMEOUT 20)
set_tests_properties(test_relay_ingress_policy PROPERTIES TIMEOUT 20)

# Include directories for libsecp256k1 headers
target_include_directories(test_nostr PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_relay PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_relay_shutdown PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_subscription_lifecycle PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_subscription_backpressure PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_subscription_backpressure_long PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_subscription_blocking_depth PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_relay_unsubscribe PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_connection_shutdown_order PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_envelope_parse PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_json_event PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_json_event_fuzzlite PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_json_envelope PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_json_filter PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_json_filter_tags PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_json_filter_robust PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_json_filter_fuzzlite PRIVATE ${SECP256K1_INCLUDE_DIRS} ${JANSSON_INCLUDE_DIRS})
target_include_directories(test_json_int_array PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_json_raw PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_filters_move_semantics PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(microbench_json PRIVATE ${SECP256K1_INCLUDE_DIRS})

# Link directories for libsecp256k1
target_link_directories(test_nostr PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_json_filter PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_json_filter_tags PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_json_filter_robust PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_json_filter_fuzzlite PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_json_int_array PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_json_raw PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_filters_move_semantics PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(microbench_json PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_json_envelope PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_json_event PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_json_event_fuzzlite PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_envelope_parse PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_relay PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_subscription_lifecycle PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_subscription_backpressure PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_subscription_backpressure_long PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_subscription_blocking_depth PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_relay_unsubscribe PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_connection_shutdown_order PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_relay_shutdown PRIVATE ${SECP256K1_LIBRARY_DIRS})

# GTK Application Integration Tests
# These tests require the gnostr binary to be built

# Find gnostr binary
set(GNOSTR_BINARY "${CMAKE_BINARY_DIR}/apps/gnostr/gnostr")

# Test: Run gnostr with ASan/UBSan (if enabled)
if(SANITIZE MATCHES "address" OR SANITIZE MATCHES "undefined")
  add_test(NAME gnostr_asan_test
    COMMAND ${CMAKE_COMMAND} -E env
      ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1
      UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
      GNOSTR_LIVE=FALSE
      ${GNOSTR_BINARY}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/apps/gnostr
  )
  set_tests_properties(gnostr_asan_test PROPERTIES
    TIMEOUT 60
    LABELS "sanitizer;gtk"
  )
endif()

# Test: Run gnostr with ThreadSanitizer (if enabled)
if(SANITIZE MATCHES "thread")
  add_test(NAME gnostr_tsan_test
    COMMAND ${CMAKE_COMMAND} -E env
      TSAN_OPTIONS=halt_on_error=1:history_size=7:second_deadlock_stack=1
      GNOSTR_LIVE=FALSE
      ${GNOSTR_BINARY}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/apps/gnostr
  )
  set_tests_properties(gnostr_tsan_test PROPERTIES
    TIMEOUT 90
    LABELS "sanitizer;gtk;concurrency"
  )
endif()

# Test: Valgrind crash detection (no leak checking - too noisy)
find_program(VALGRIND_EXECUTABLE valgrind)
if(VALGRIND_EXECUTABLE AND NOT SANITIZE)
  add_test(NAME gnostr_valgrind_crash_test
    COMMAND ${VALGRIND_EXECUTABLE}
      --tool=memcheck
      --track-origins=yes
      --leak-check=no
      --error-limit=no
      --errors-for-leak-kinds=none
      --suppressions=${CMAKE_SOURCE_DIR}/valgrind.supp
      --error-exitcode=1
      ${GNOSTR_BINARY}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/apps/gnostr
  )
  set_tests_properties(gnostr_valgrind_crash_test PROPERTIES
    TIMEOUT 180
    LABELS "valgrind;gtk"
  )
endif()

# ==============================================================================
# Concurrency Test Suite (No GTK required)
# ==============================================================================

# Test: Channel operations
add_executable(test_concurrency_channels test_concurrency_channels.c)
target_link_libraries(test_concurrency_channels PRIVATE libgo nostr pthread)
apply_sanitizers(test_concurrency_channels)
add_test(NAME concurrency_channels COMMAND test_concurrency_channels)
set_tests_properties(concurrency_channels PROPERTIES
  TIMEOUT 30
  LABELS "concurrency;unit"
)

# Test: Subscription shutdown invariants
add_executable(test_concurrency_subscription_shutdown test_concurrency_subscription_shutdown.c)
target_link_libraries(test_concurrency_subscription_shutdown PRIVATE libgo nostr pthread)
apply_sanitizers(test_concurrency_subscription_shutdown)
add_test(NAME concurrency_subscription_shutdown COMMAND test_concurrency_subscription_shutdown)
set_tests_properties(concurrency_subscription_shutdown PROPERTIES
  TIMEOUT 60
  LABELS "concurrency;shutdown"
  ENVIRONMENT "NOSTR_TEST_MODE=1"
)

# Test: Subscription shutdown with ASan (detects use-after-free)
if(GO_ENABLE_ASAN AND NOT GO_ENABLE_TSAN)
  add_test(NAME concurrency_subscription_shutdown_asan
    COMMAND test_concurrency_subscription_shutdown)
  set_tests_properties(concurrency_subscription_shutdown_asan PROPERTIES
    TIMEOUT 90
    LABELS "concurrency;shutdown;asan"
    ENVIRONMENT "NOSTR_TEST_MODE=1;ASAN_OPTIONS=detect_leaks=1:halt_on_error=1"
  )
endif()

# Test: Subscription shutdown with TSAN (detects races)
if(GO_ENABLE_TSAN)
  add_test(NAME concurrency_subscription_shutdown_tsan
    COMMAND test_concurrency_subscription_shutdown)
  set_tests_properties(concurrency_subscription_shutdown_tsan PROPERTIES
    TIMEOUT 120
    LABELS "concurrency;shutdown;tsan"
    ENVIRONMENT "NOSTR_TEST_MODE=1;TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1"
  )
endif()

# Test: GLib main loop async integration
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)
add_executable(test_glib_async_integration test_glib_async_integration.c)
target_include_directories(test_glib_async_integration PRIVATE ${GLIB_INCLUDE_DIRS})
target_link_libraries(test_glib_async_integration PRIVATE libgo nostr pthread ${GLIB_LIBRARIES})
apply_sanitizers(test_glib_async_integration)
add_test(NAME glib_async_integration COMMAND test_glib_async_integration)
set_tests_properties(glib_async_integration PROPERTIES
  TIMEOUT 5
  LABELS "concurrency;glib;integration"
)
