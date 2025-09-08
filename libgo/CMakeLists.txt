# Option to enable verbose fiber scheduler debug logging
option(GOF_DEBUG "Enable debug logging in libgo_fiber scheduler" OFF)
# Language standard and warnings
set(CMAKE_C_STANDARD 11)

set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(GO_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

function(apply_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  if(GO_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

cmake_minimum_required(VERSION 3.30)
# Concurrency primitives inspired by Go: channels, contexts, wait groups, etc.
project(libgo C)
enable_language(ASM)

# Optional: enable more aggressive local optimizations for libgo
include(CheckCCompilerFlag)
include(CheckIPOSupported)
option(GO_OPTIMIZE_NATIVE "Build libgo with -O3 -march=native and related perf flags" OFF)
option(GO_ENABLE_LTO "Enable Link-Time Optimization for libgo" OFF)
function(apply_libgo_optimizations target)
  if(GO_OPTIMIZE_NATIVE)
    # Compile-time optimization level (does not override user's global, only augments target)
    check_c_compiler_flag("-O3" HAS_O3)
    if(HAS_O3)
      target_compile_options(${target} PRIVATE -O3)
    endif()
    # CPU tuning to local machine
    check_c_compiler_flag("-march=native" HAS_MARCH_NATIVE)
    if(HAS_MARCH_NATIVE)
      target_compile_options(${target} PRIVATE -march=native)
    endif()
    # Avoid interposition overheads inside this TU
    check_c_compiler_flag("-fno-semantic-interposition" HAS_FNO_SEM_INT)
    if(HAS_FNO_SEM_INT)
      target_compile_options(${target} PRIVATE -fno-semantic-interposition)
    endif()
  endif()
endfunction()

# Include the header files
include_directories(include)
# Also include libnostr public headers for metrics integration
# Use path relative to this CMakeLists.txt so it works standalone and as a subdir
include_directories(${CMAKE_CURRENT_LIST_DIR}/../libnostr/include)

# Allow tuning of channel prefetch distance at compile time.
# Default matches channel.c internal default when not defined.
set(NOSTR_PREFETCH_DISTANCE "1" CACHE STRING "Prefetch distance (in slots) for libgo channels")

# Allow configuration of cacheline size for padding/alignment.
set(NOSTR_CACHELINE "64" CACHE STRING "Assumed cacheline size for padding/alignment in libgo (bytes)")

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
  src/metrics_stub.c
)

# Enable CTest for module-specific tests
enable_testing()

# Build static library exposing public headers from `include/`.
add_library(libgo ${LIBGO_SOURCES})

target_include_directories(libgo PUBLIC include ${CMAKE_CURRENT_LIST_DIR}/../libnostr/include)
target_compile_definitions(libgo PUBLIC NOSTR_PREFETCH_DISTANCE=${NOSTR_PREFETCH_DISTANCE})
target_compile_definitions(libgo PUBLIC NOSTR_CACHELINE=${NOSTR_CACHELINE})

# Allow A/B testing of refined signaling vs broadcast.
option(GO_CHANNEL_REFINED_SIGNALING "Use single-wakeup signaling in channels (signal on 0->1 and full->not-full)" ON)
if(GO_CHANNEL_REFINED_SIGNALING)
  target_compile_definitions(libgo PUBLIC NOSTR_REFINED_SIGNALING=1)
else()
  target_compile_definitions(libgo PUBLIC NOSTR_REFINED_SIGNALING=0)
endif()

# Optionally derive channel occupancy from indices to avoid hot size updates
option(GO_CHANNEL_DERIVE_SIZE "Compute channel occupancy from indices instead of maintaining size" OFF)
option(GO_CHANNEL_ATOMIC_TRY "Use atomic lockless full/empty check in try paths" ON)
option(GO_CHANNEL_MPMC_SLOTS "Enable MPMC lock-free per-slot sequence protocol for channels" ON)

# Under ThreadSanitizer, prefer fully locked try paths and disable MPMC slot protocol
if (GO_ENABLE_TSAN)
  set(GO_CHANNEL_ATOMIC_TRY OFF CACHE BOOL "Use atomic lockless full/empty check in try paths" FORCE)
  set(GO_CHANNEL_MPMC_SLOTS OFF CACHE BOOL "Enable MPMC lock-free per-slot sequence protocol for channels" FORCE)
endif()
if(GO_CHANNEL_DERIVE_SIZE)
  target_compile_definitions(libgo PRIVATE NOSTR_CHANNEL_DERIVE_SIZE=1)
endif()
if(GO_CHANNEL_ATOMIC_TRY)
  target_compile_definitions(libgo PRIVATE NOSTR_CHANNEL_ATOMIC_TRY=1)
else()
  target_compile_definitions(libgo PRIVATE NOSTR_CHANNEL_ATOMIC_TRY=0)
endif()
if(GO_CHANNEL_MPMC_SLOTS)
  target_compile_definitions(libgo PRIVATE NOSTR_CHANNEL_MPMC_SLOTS=1)
else()
  target_compile_definitions(libgo PRIVATE NOSTR_CHANNEL_MPMC_SLOTS=0)
endif()

# No additional link requirements for the core library here.

# Optionally disable metrics in hot paths for maximum throughput
option(GO_CHANNEL_DISABLE_METRICS "Compile out channel hot-path metrics" OFF)
if(GO_CHANNEL_DISABLE_METRICS)
  target_compile_definitions(libgo PRIVATE NOSTR_CHANNEL_DISABLE_METRICS=1)
endif()

# Optionally enforce power-of-two capacity (round up) for faster masking
option(GO_CHANNEL_ENFORCE_POW2_CAP "Round capacity up to next power of two for channels" OFF)
if(GO_CHANNEL_ENFORCE_POW2_CAP)
  target_compile_definitions(libgo PRIVATE NOSTR_CHANNEL_ENFORCE_POW2_CAP=1)
endif()

# Optional arch-specific short-wait hints
option(GO_CHANNEL_X86_TPAUSE "Use x86 WAITPKG TPAUSE for short waits" OFF)
if(GO_CHANNEL_X86_TPAUSE)
  target_compile_definitions(libgo PRIVATE NOSTR_X86_TPAUSE=1)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|i.86")
    target_compile_options(libgo PRIVATE -mwaitpkg)
  endif()
endif()

option(GO_CHANNEL_ARM_WFE "Use ARM WFE/SEV for short waits and wakeups" OFF)
if(GO_CHANNEL_ARM_WFE)
  target_compile_definitions(libgo PRIVATE NOSTR_ARM_WFE=1)
endif()

# Sanitizer options
if (NOT DEFINED GO_ENABLE_ASAN)
  option(GO_ENABLE_ASAN "Enable AddressSanitizer" OFF)
endif()
if (NOT DEFINED GO_ENABLE_UBSAN)
  option(GO_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
endif()
if (NOT DEFINED GO_ENABLE_TSAN)
  option(GO_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
endif()

function(apply_sanitizers target)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(GO_ENABLE_TSAN)
      target_compile_options(${target} PRIVATE -fsanitize=thread)
      target_link_options(${target} PRIVATE -fsanitize=thread)
    else()
      if(GO_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address)
        target_link_options(${target} PRIVATE -fsanitize=address)
      endif()
      if(GO_ENABLE_UBSAN)
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
if(GO_ENABLE_LTO)
  check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
  if(ipo_supported)
    set_property(TARGET libgo PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  else()
    check_c_compiler_flag("-flto" HAS_FLTO)
    if(HAS_FLTO)
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
target_link_libraries(libgo PUBLIC ${NSYNC_LIB} pthread)
set_property(TARGET libgo PROPERTY POSITION_INDEPENDENT_CODE ON)
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


# Ensure usleep is declared on Linux/glibc builds for test targets
if(UNIX AND NOT APPLE)
  target_compile_definitions(go_wait_group_test PRIVATE _XOPEN_SOURCE=700)
  target_compile_definitions(go_select_test PRIVATE _XOPEN_SOURCE=700)
  target_compile_definitions(go_ticker_test PRIVATE _XOPEN_SOURCE=700)
  target_compile_definitions(go_channel_close_test PRIVATE _XOPEN_SOURCE=700)
  target_compile_definitions(go_select_multi_test PRIVATE _XOPEN_SOURCE=700)
endif()


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

###############################################
# Fiber runtime (separate library): go_fiber  #
###############################################

option(GOF_PORTABLE_CONTEXT "Use portable setjmp/longjmp for fiber context" OFF)
option(GOF_ENABLE_TRACING "Enable lightweight tracing in go_fiber" OFF)

set(GOF_FIBER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fiber)
set(GOF_PUBLIC_INC ${GOF_FIBER_DIR}/include)

set(GOF_COMMON_SOURCES
  ${GOF_FIBER_DIR}/sched/api.c
  ${GOF_FIBER_DIR}/sched/sched.c
  ${GOF_FIBER_DIR}/sched/park.c
  ${GOF_FIBER_DIR}/stack/stack.c
  ${GOF_FIBER_DIR}/chan/chan.c
  ${GOF_FIBER_DIR}/io/io.c
  ${GOF_FIBER_DIR}/io/netpoll_epoll.c
  ${GOF_FIBER_DIR}/io/netpoll_kqueue.c
  ${GOF_FIBER_DIR}/io/netpoll_iocp.c
  ${GOF_FIBER_DIR}/timers/timer_bridge.c
  ${GOF_FIBER_DIR}/debug/trace.c
  ${GOF_FIBER_DIR}/debug/introspect.c
)

if(GOF_PORTABLE_CONTEXT)
  # Prevent deprecated portable context on macOS
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "GOF_PORTABLE_CONTEXT is not supported on macOS; use assembly backends. Set GOF_PORTABLE_CONTEXT=OFF.")
  endif()
  list(APPEND GOF_COMMON_SOURCES ${GOF_FIBER_DIR}/context/context_portable.c)
else()
  # Prefer assembly backend where supported. Provide Darwin/arm64 and Darwin/x86_64 backends.
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    list(APPEND GOF_COMMON_SOURCES
      ${GOF_FIBER_DIR}/context/context_arm64.S
      ${GOF_FIBER_DIR}/context/context_asm.c)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    list(APPEND GOF_COMMON_SOURCES
      ${GOF_FIBER_DIR}/context/context_x86_64.S
      ${GOF_FIBER_DIR}/context/context_asm.c)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    list(APPEND GOF_COMMON_SOURCES
      ${GOF_FIBER_DIR}/context/context_x86_64_linux.S
      ${GOF_FIBER_DIR}/context/context_asm.c)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    list(APPEND GOF_COMMON_SOURCES
      ${GOF_FIBER_DIR}/context/context_arm64_linux.S
      ${GOF_FIBER_DIR}/context/context_asm.c)
  else()
    # Fallback to portable backend on unsupported platforms to keep builds working.
    list(APPEND GOF_COMMON_SOURCES ${GOF_FIBER_DIR}/context/context_portable.c)
  endif()
endif()

add_library(go_fiber STATIC ${GOF_COMMON_SOURCES})
target_include_directories(go_fiber PUBLIC ${GOF_PUBLIC_INC})
if(GOF_PORTABLE_CONTEXT)
  target_compile_definitions(go_fiber PUBLIC GOF_PORTABLE_CONTEXT=1)
else()
  target_compile_definitions(go_fiber PUBLIC GOF_PORTABLE_CONTEXT=0)
endif()
apply_warnings(go_fiber)
apply_sanitizers(go_fiber)

# Propagate GOF_DEBUG to the fiber library build only
if(GOF_DEBUG)
  target_compile_definitions(go_fiber PRIVATE GOF_DEBUG=1)
endif()

if(GOF_ENABLE_TRACING)
  target_compile_definitions(go_fiber PRIVATE GOF_ENABLE_TRACING=1)
endif()

# Platform switches for netpoll
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(go_fiber PRIVATE GOF_NETPOLL_EPOLL=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_compile_definitions(go_fiber PRIVATE GOF_NETPOLL_KQUEUE=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  target_compile_definitions(go_fiber PRIVATE GOF_NETPOLL_IOCP=1)
endif()

# Simple unit tests for go_fiber (standalone binaries with asserts)
add_executable(gof_test_basic fiber/tests/test_basic.c)
add_executable(gof_test_context fiber/tests/test_context.c)
add_executable(gof_test_chan fiber/tests/test_chan.c)
add_executable(gof_test_io fiber/tests/test_io.c)
add_executable(gof_test_io_timeout fiber/tests/test_io_timeout.c)
add_executable(gof_test_starvation fiber/tests/test_starvation.c)
target_link_libraries(gof_test_basic PRIVATE go_fiber)
target_link_libraries(gof_test_context PRIVATE go_fiber)
target_link_libraries(gof_test_chan PRIVATE go_fiber)
target_link_libraries(gof_test_io PRIVATE go_fiber)
target_link_libraries(gof_test_io_timeout PRIVATE go_fiber)
target_link_libraries(gof_test_starvation PRIVATE go_fiber)
apply_warnings(gof_test_basic)
apply_warnings(gof_test_context)
apply_warnings(gof_test_chan)
apply_warnings(gof_test_io)
apply_warnings(gof_test_io_timeout)
apply_warnings(gof_test_starvation)
apply_sanitizers(gof_test_basic)
apply_sanitizers(gof_test_context)
apply_sanitizers(gof_test_chan)
apply_sanitizers(gof_test_io)
apply_sanitizers(gof_test_io_timeout)
apply_sanitizers(gof_test_starvation)

add_test(NAME GoFiberBasicTest COMMAND gof_test_basic)
add_test(NAME GoFiberContextTest COMMAND gof_test_context)
add_test(NAME GoFiberChanTest COMMAND gof_test_chan)
add_test(NAME GoFiberIoTest COMMAND gof_test_io)
add_test(NAME GoFiberIoTimeoutTest COMMAND gof_test_io_timeout)
add_test(NAME GoFiberStarvationTest COMMAND gof_test_starvation)

# Set short timeouts for fiber tests to catch hangs
set_tests_properties(GoFiberBasicTest PROPERTIES TIMEOUT 10)
set_tests_properties(GoFiberContextTest PROPERTIES TIMEOUT 10)
set_tests_properties(GoFiberChanTest PROPERTIES TIMEOUT 15)
set_tests_properties(GoFiberIoTest PROPERTIES TIMEOUT 10)
set_tests_properties(GoFiberIoTimeoutTest PROPERTIES TIMEOUT 2)
set_tests_properties(GoFiberStarvationTest PROPERTIES TIMEOUT 20)

# Benchmarks (not added to CTest by default)
add_executable(gof_bench_yield fiber/bench/bench_yield.c)
add_executable(gof_bench_pingpong fiber/bench/bench_pingpong.c)
add_executable(gof_bench_sleep fiber/bench/bench_sleep.c)
add_executable(gof_bench_poll_partition fiber/bench/bench_poll_partition.c)
target_link_libraries(gof_bench_yield PRIVATE go_fiber)
target_link_libraries(gof_bench_pingpong PRIVATE go_fiber)
target_link_libraries(gof_bench_sleep PRIVATE go_fiber)
target_link_libraries(gof_bench_poll_partition PRIVATE go_fiber)
apply_warnings(gof_bench_yield)
apply_warnings(gof_bench_pingpong)
apply_warnings(gof_bench_sleep)
apply_warnings(gof_bench_poll_partition)
apply_sanitizers(gof_bench_yield)
apply_sanitizers(gof_bench_pingpong)
apply_sanitizers(gof_bench_sleep)
apply_sanitizers(gof_bench_poll_partition)
