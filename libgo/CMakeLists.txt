cmake_minimum_required(VERSION 3.50)
# Concurrency primitives inspired by Go: channels, contexts, wait groups, etc.
project(libgo)

# Include the header files
include_directories(include)

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

target_compile_options(libgo PRIVATE $<$<CONFIG:Debug>:-Werror>)

target_include_directories(libgo PUBLIC include)

# No additional link requirements for the core library here.

# Find the nsync library and headers manually; used by tests and may be used by
# consumers for synchronization in conjunction with libgo.
find_library(NSYNC_LIB nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)

# Include the nsync headers
include_directories(${NSYNC_INCLUDE_DIR})


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

# Link the context library to each test
target_link_libraries(go_context_cancel_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_channel_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_context_timeout_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_ticker_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_select_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_wait_group_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_channel_close_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_select_multi_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_channel_stress_test PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(go_context_stress_test PRIVATE libgo ${NSYNC_LIB} pthread)

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
target_link_libraries(error_demo PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(refptr_demo PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(hash_map_demo PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(arrays_demo PRIVATE libgo ${NSYNC_LIB} pthread)
target_link_libraries(long_adder_demo PRIVATE libgo ${NSYNC_LIB} pthread)
# install(TARGETS nostr DESTINATION lib)
# install(DIRECTORY include/ DESTINATION include)
