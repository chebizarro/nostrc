# Language standard and warnings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(GO_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

function(apply_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  if (GO_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

cmake_minimum_required(VERSION 3.30)
# Concurrency primitives inspired by Go: channels, contexts, wait groups, etc.
project(libgo)

# Optional: enable more aggressive local optimizations for libgo
include(CheckCCompilerFlag)
include(CheckIPOSupported)
option(GO_OPTIMIZE_NATIVE "Build libgo with -O3 -march=native and related perf flags" OFF)
option(GO_ENABLE_LTO "Enable Link-Time Optimization for libgo" OFF)
function(apply_libgo_optimizations target)
  if (GO_OPTIMIZE_NATIVE)
    # Compile-time optimization level (does not override user's global, only augments target)
    check_c_compiler_flag("-O3" HAS_O3)
    if (HAS_O3)
      target_compile_options(${target} PRIVATE -O3)
    endif()
    # CPU tuning to local machine
    check_c_compiler_flag("-march=native" HAS_MARCH_NATIVE)
    if (HAS_MARCH_NATIVE)
      target_compile_options(${target} PRIVATE -march=native)
    endif()
    # Avoid interposition overheads inside this TU
    check_c_compiler_flag("-fno-semantic-interposition" HAS_FNO_SEM_INT)
    if (HAS_FNO_SEM_INT)
      target_compile_options(${target} PRIVATE -fno-semantic-interposition)
    endif()
  endif()
endfunction()

# Include the header files
include_directories(include)
# Also include libnostr public headers for metrics integration
include_directories(${CMAKE_SOURCE_DIR}/libnostr/include)

set(LIBGO_SOURCES
    src/go.c
    src/context.c
    src/channel.c
	src/wait_group.c
	src/hash_map.c
	src/counter.c
	src/select.c
	src/string_array.c
	src/int_array.c
    src/ticker.c
    src/error.c
)

# Enable CTest for module-specific tests
enable_testing()

# Build static library exposing public headers from `include/`.
add_library(libgo ${LIBGO_SOURCES})

target_include_directories(libgo PUBLIC include ${CMAKE_SOURCE_DIR}/libnostr/include)

# No additional link requirements for the core library here.

# Sanitizer options
option(GO_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(GO_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(GO_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

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

apply_warnings(libgo)
apply_sanitizers(libgo)
apply_libgo_optimizations(libgo)

# Optional LTO for libgo
if (GO_ENABLE_LTO)
  check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
  if (ipo_supported)
    set_property(TARGET libgo PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  else()
    check_c_compiler_flag("-flto" HAS_FLTO)
    if (HAS_FLTO)
      target_compile_options(libgo PRIVATE -flto)
      target_link_options(libgo PRIVATE -flto)
    endif()
  endif()
endif()

# Find the nsync library and headers manually; used by tests and may be used by
# consumers for synchronization in conjunction with libgo.
find_library(NSYNC_LIB nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)

# Include the nsync headers
include_directories(${NSYNC_INCLUDE_DIR})

# When building libgo as a shared library, we must link nsync directly;
# otherwise undefined symbols (nsync_*) will fail the link step.
# Expose nsync to consumers as PUBLIC so downstreams inherit it via CMake.
target_link_libraries(libgo PUBLIC ${NSYNC_LIB} pthread nostr)
target_include_directories(libgo PUBLIC ${NSYNC_INCLUDE_DIR})

# Create the test executables for channels and contexts
add_executable(go_context_cancel_test tests/go_context_cancel_test.c)
add_executable(go_channel_test tests/go_channel_test.c)
add_executable(go_context_timeout_test tests/go_context_timeout_test.c)
add_executable(go_ticker_test tests/go_ticker_test.c)
add_executable(go_select_test tests/go_select_test.c)
add_executable(go_wait_group_test tests/go_wait_group_test.c)
add_executable(go_channel_close_test tests/go_channel_close_test.c)
add_executable(go_select_multi_test tests/go_select_multi_test.c)
add_executable(go_channel_stress_test tests/go_channel_stress_test.c)
add_executable(go_context_stress_test tests/go_context_stress_test.c)
add_executable(error_test tests/error_test.c)
add_executable(refptr_test tests/refptr_test.c)
add_executable(hash_map_test2 tests/hash_map_test2.c)
add_executable(int_array_test tests/int_array_test.c)
add_executable(string_array_test tests/string_array_test.c)
add_executable(long_adder_stress_test tests/long_adder_stress_test.c)
add_executable(go_select_closed_test tests/go_select_closed_test.c)
add_executable(go_select_cancel_test tests/go_select_cancel_test.c)

# Link the context library to each test
target_link_libraries(go_context_cancel_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_channel_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_context_timeout_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_ticker_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_wait_group_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_select_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_select_multi_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_channel_stress_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_context_stress_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_channel_close_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_select_closed_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_select_cancel_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(error_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(refptr_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(hash_map_test2 PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(int_array_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(string_array_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(long_adder_stress_test PRIVATE libgo ${NSYNC_LIB} pthread)

apply_sanitizers(error_test)
apply_sanitizers(refptr_test)
apply_sanitizers(hash_map_test2)
apply_sanitizers(int_array_test)
apply_sanitizers(string_array_test)
apply_sanitizers(long_adder_stress_test)
apply_warnings(error_test)
apply_warnings(refptr_test)
apply_warnings(hash_map_test2)
apply_warnings(int_array_test)
apply_warnings(string_array_test)
apply_warnings(long_adder_stress_test)
apply_sanitizers(go_context_cancel_test)
apply_sanitizers(go_channel_test)
apply_sanitizers(go_context_timeout_test)
apply_sanitizers(go_wait_group_test)
apply_sanitizers(go_select_test)
apply_sanitizers(go_select_multi_test)
apply_sanitizers(go_channel_stress_test)
apply_sanitizers(go_context_stress_test)
apply_sanitizers(go_ticker_test)
apply_sanitizers(go_channel_close_test)
apply_sanitizers(go_select_closed_test)
apply_sanitizers(go_select_cancel_test)
apply_warnings(go_context_cancel_test)
apply_warnings(go_channel_test)
apply_warnings(go_context_timeout_test)
apply_warnings(go_wait_group_test)
apply_warnings(go_select_test)
apply_warnings(go_select_multi_test)
apply_warnings(go_channel_stress_test)
apply_warnings(go_context_stress_test)
apply_warnings(go_ticker_test)
apply_warnings(go_channel_close_test)
apply_warnings(go_select_closed_test)
apply_warnings(go_select_cancel_test)



# Add tests to CTest
add_test(NAME GoContextCancelTest COMMAND go_context_cancel_test)
add_test(NAME GoChannelTest COMMAND go_channel_test)
add_test(NAME GoContextTimeoutTest COMMAND go_context_timeout_test)
add_test(NAME GoTickerTest COMMAND go_ticker_test)
add_test(NAME GoSelectTest COMMAND go_select_test)
add_test(NAME GoWaitGroupTest COMMAND go_wait_group_test)
add_test(NAME GoChannelCloseTest COMMAND go_channel_close_test)
add_test(NAME GoSelectMultiTest COMMAND go_select_multi_test)
add_test(NAME GoChannelStressTest COMMAND go_channel_stress_test)
add_test(NAME GoContextStressTest COMMAND go_context_stress_test)
add_test(NAME GoSelectClosedTest COMMAND go_select_closed_test)
add_test(NAME GoSelectCancelTest COMMAND go_select_cancel_test)

# Register utility tests
add_test(NAME GoErrorTest COMMAND error_test)
add_test(NAME GoRefPtrTest COMMAND refptr_test)
add_test(NAME GoHashMapTest2 COMMAND hash_map_test2)
add_test(NAME GoIntArrayTest COMMAND int_array_test)
add_test(NAME GoStringArrayTest COMMAND string_array_test)
add_test(NAME GoLongAdderStressTest COMMAND long_adder_stress_test)
set_tests_properties(GoLongAdderStressTest PROPERTIES TIMEOUT 45)

# Optionally, set timeout for each test (in seconds)
set_tests_properties(GoContextCancelTest PROPERTIES TIMEOUT 10)
set_tests_properties(GoChannelTest PROPERTIES TIMEOUT 10)
set_tests_properties(GoContextTimeoutTest PROPERTIES TIMEOUT 10)
set_tests_properties(GoTickerTest PROPERTIES TIMEOUT 10)
set_tests_properties(GoSelectTest PROPERTIES TIMEOUT 10)
set_tests_properties(GoWaitGroupTest PROPERTIES TIMEOUT 10)
set_tests_properties(GoChannelCloseTest PROPERTIES TIMEOUT 10)
set_tests_properties(GoSelectMultiTest PROPERTIES TIMEOUT 10)
set_tests_properties(GoChannelStressTest PROPERTIES TIMEOUT 30)
set_tests_properties(GoContextStressTest PROPERTIES TIMEOUT 30)


# Optionally, you can specify installation rules

# Examples
add_executable(go_context_demo examples/go_context_demo.c)
add_executable(go_select_demo examples/go_select_demo.c)
add_executable(go_ticker_demo examples/go_ticker_demo.c)
target_link_libraries(go_context_demo PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_select_demo PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_ticker_demo PRIVATE libgo ${NSYNC_LIB} pthread)

add_executable(error_demo examples/error_demo.c)
add_executable(refptr_demo examples/refptr_demo.c)
add_executable(hash_map_demo examples/hash_map_demo.c)
add_executable(arrays_demo examples/arrays_demo.c)
add_executable(long_adder_demo examples/long_adder_demo.c)
add_executable(chan_bench examples/chan_bench.c)
target_link_libraries(error_demo PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(refptr_demo PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(hash_map_demo PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(arrays_demo PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(long_adder_demo PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(chan_bench PRIVATE libgo ${NSYNC_LIB} pthread)
apply_sanitizers(go_context_demo)
apply_sanitizers(go_select_demo)
apply_sanitizers(go_ticker_demo)
apply_sanitizers(error_demo)
apply_sanitizers(refptr_demo)
apply_sanitizers(hash_map_demo)
apply_sanitizers(arrays_demo)
apply_sanitizers(long_adder_demo)
apply_sanitizers(chan_bench)
# Intentionally do not apply warnings-as-errors to examples to keep demos buildable across toolchains
# install(TARGETS nostr DESTINATION lib)
# install(DIRECTORY include/ DESTINATION include)
