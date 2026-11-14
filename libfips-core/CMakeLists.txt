cmake_minimum_required(VERSION 3.22)
project(libfips-core C)

option(FIPS_ENABLE_FUZZERS "Build clang libFuzzer harnesses" OFF)

declare_component_version(fips_core 0 1 0)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FIPS_CORE_SECP256K1 REQUIRED IMPORTED_TARGET libsecp256k1)
pkg_check_modules(FIPS_CORE_SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

# Follow BUILD_SHARED_LIBS, as the other nostrc libraries do.
add_library(fips_core src/core.c src/host.c)

# Proto: bounds-checked wire primitives.
target_sources(fips_core PRIVATE src/proto/codec.c)
# Transport value types (sans-IO).
target_sources(fips_core PRIVATE src/transport/types.c)
# Proto rate-limit primitives.
target_sources(fips_core PRIVATE src/proto/rate_limit.c)

# Node bookkeeping (sans-IO, explicit host instants).
target_sources(fips_core PRIVATE src/node/rate_limit.c src/node/stats_history.c)

# Coordinate cache (sans-IO; caller-owned slots and eviction indexes).
target_sources(fips_core PRIVATE src/cache/coord_cache.c src/cache/entry.c)
if(UNIX)
  target_link_libraries(fips_core PRIVATE m)
  set(FIPS_CORE_PC_MATH_LIB "-lm")
endif()
set_target_properties(fips_core PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  COMPONENT_VERSION_HEADER_INSTALL_DIR "${CMAKE_INSTALL_INCLUDEDIR}/fips")
target_include_directories(fips_core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE "${fips_core_VERSION_HEADER_DIR}")
target_link_libraries(fips_core PRIVATE
  PkgConfig::FIPS_CORE_SECP256K1 PkgConfig::FIPS_CORE_SODIUM OpenSSL::Crypto)
nostrc_library_visibility(fips_core FIPS_CORE_API fips/export.h
  "${CMAKE_INSTALL_INCLUDEDIR}/fips")
apply_versioning(fips_core)
apply_warnings(fips_core)
if(GO_ENABLE_TSAN)
  # Override inherited workspace instrumentation: threading belongs to fipsd.
  target_compile_options(fips_core PRIVATE -fno-sanitize=thread)
else()
  apply_sanitizers(fips_core)
endif()

set(FIPS_CORE_PC_STATIC_CFLAGS "")
if(NOT BUILD_SHARED_LIBS)
  set(FIPS_CORE_PC_STATIC_CFLAGS "-DFIPS_CORE_STATIC_DEFINE")
endif()
configure_file(libfips-core.pc.in libfips-core.pc @ONLY)
install(TARGETS fips_core
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/fips DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libfips-core.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

if(FIPS_ENABLE_FUZZERS)
  add_subdirectory(fuzz)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# Shared utilities (nostrc-u50yr.3): bounded caller storage, host entropy only.
target_sources(fips_core PRIVATE
  src/utils/index.c src/utils/hash.c src/utils/onlink.c src/proto/math.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/proto/math.c PROPERTIES
    COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
endif()
# Identity primitives (explicit host entropy, no hot-path allocations).
target_sources(fips_core PRIVATE
  src/identity/mod.c src/identity/key.c src/identity/local.c
  src/identity/peer.c src/identity/node_addr.c src/identity/address.c
  src/identity/encoding.c src/identity/auth.c src/identity/prefix.c)
option(FIPS_ENABLE_BENCH "Build libfips-core micro-benchmarks (not run by ctest)" OFF)
if(FIPS_ENABLE_BENCH)
  add_subdirectory(bench)
endif()

# Noise DH: SHA-256 of shared x, using the identity context.
target_sources(fips_core PRIVATE src/noise/ecdh.c)
