add_executable(fips_core_skeleton_test test_core.c)
target_link_libraries(fips_core_skeleton_test PRIVATE fips_core)
set_target_properties(fips_core_skeleton_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_skeleton_test)
apply_sanitizers(fips_core_skeleton_test)
add_test(NAME FipsCoreSkeletonTest COMMAND fips_core_skeleton_test)
set_tests_properties(FipsCoreSkeletonTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreSkeletonTest OWNER libfips-core TARGETS fips_core_skeleton_test)

add_executable(fips_core_host_test test_host.c fakes/host.c)
target_link_libraries(fips_core_host_test PRIVATE fips_core)
set_target_properties(fips_core_host_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_host_test)
apply_sanitizers(fips_core_host_test)
add_test(NAME FipsCoreHostTest COMMAND fips_core_host_test)
set_tests_properties(FipsCoreHostTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreHostTest OWNER libfips-core TARGETS fips_core_host_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/seeded_vectors.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/generate_seeded_vectors.py")

add_subdirectory(diff)

add_executable(fips_core_codec_test test_codec.c)
target_link_libraries(fips_core_codec_test PRIVATE fips_core)
set_target_properties(fips_core_codec_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_codec_test)
apply_sanitizers(fips_core_codec_test)
add_test(NAME FipsCoreCodecTest COMMAND fips_core_codec_test)
set_tests_properties(FipsCoreCodecTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreCodecTest OWNER libfips-core TARGETS fips_core_codec_test)
# Keep the existing explicit FIPS CI build closure complete.
add_dependencies(fips_core_host_test fips_core_codec_test)
add_executable(fips_core_transport_types_test test_transport_types.c)
target_link_libraries(fips_core_transport_types_test PRIVATE fips_core)
set_target_properties(fips_core_transport_types_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_transport_types_test)
apply_sanitizers(fips_core_transport_types_test)
add_test(NAME FipsCoreTransportTypesTest COMMAND fips_core_transport_types_test)
set_tests_properties(FipsCoreTransportTypesTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreTransportTypesTest OWNER libfips-core TARGETS fips_core_transport_types_test)
add_executable(fips_core_rate_limit_test test_rate_limit.c fakes/host.c)
target_link_libraries(fips_core_rate_limit_test PRIVATE fips_core)
set_target_properties(fips_core_rate_limit_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_rate_limit_test)
apply_sanitizers(fips_core_rate_limit_test)
add_test(NAME FipsCoreRateLimitTest COMMAND fips_core_rate_limit_test)
set_tests_properties(FipsCoreRateLimitTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreRateLimitTest OWNER libfips-core TARGETS fips_core_rate_limit_test)

add_executable(fips_core_stats_history_test test_stats_history.c fakes/host.c)
target_link_libraries(fips_core_stats_history_test PRIVATE fips_core)
set_target_properties(fips_core_stats_history_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_stats_history_test)
apply_sanitizers(fips_core_stats_history_test)
add_test(NAME FipsCoreStatsHistoryTest COMMAND fips_core_stats_history_test)
set_tests_properties(FipsCoreStatsHistoryTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreStatsHistoryTest OWNER libfips-core TARGETS fips_core_stats_history_test)

# FIPS CI builds the established host-test entrypoint explicitly before ctest.
# Keep these registered tests in that build closure without changing CI lanes.
add_dependencies(fips_core_host_test fips_core_rate_limit_test fips_core_stats_history_test)

# Shared utility/math ports (nostrc-u50yr.3), independent of sibling blocks.
add_executable(fips_core_utils_test test_utils.c fakes/host.c)
target_link_libraries(fips_core_utils_test PRIVATE fips_core)
set_target_properties(fips_core_utils_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_utils_test)
apply_sanitizers(fips_core_utils_test)
add_test(NAME FipsCoreUtilsTest COMMAND fips_core_utils_test)
set_tests_properties(FipsCoreUtilsTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreUtilsTest OWNER libfips-core TARGETS fips_core_utils_test)

add_executable(fips_core_math_test test_math.c)
target_link_libraries(fips_core_math_test PRIVATE fips_core)
set_target_properties(fips_core_math_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_math_test)
apply_sanitizers(fips_core_math_test)
add_test(NAME FipsCoreMathTest COMMAND fips_core_math_test)
set_tests_properties(FipsCoreMathTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreMathTest OWNER libfips-core TARGETS fips_core_math_test)
add_dependencies(fips_core_host_test fips_core_utils_test fips_core_math_test)
# Identity behavioral ports and C ownership/bounds cases.
add_executable(fips_core_identity_test test_identity.c fakes/host.c)
target_link_libraries(fips_core_identity_test PRIVATE fips_core PkgConfig::FIPS_CORE_SODIUM)
set_target_properties(fips_core_identity_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_identity_test)
apply_sanitizers(fips_core_identity_test)
add_test(NAME FipsCoreIdentityTest COMMAND fips_core_identity_test)
set_tests_properties(FipsCoreIdentityTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreIdentityTest OWNER libfips-core TARGETS fips_core_identity_test)
add_dependencies(fips_core_host_test fips_core_identity_test)
add_executable(fips_core_coord_cache_test test_coord_cache.c fakes/host.c)
target_link_libraries(fips_core_coord_cache_test PRIVATE fips_core)
set_target_properties(fips_core_coord_cache_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_coord_cache_test)
apply_sanitizers(fips_core_coord_cache_test)
add_test(NAME FipsCoreCoordCacheTest COMMAND fips_core_coord_cache_test)
# Default-capacity fills (50,000 entries) run under sanitizers.
set_tests_properties(FipsCoreCoordCacheTest PROPERTIES TIMEOUT 120 LABELS "fips;unit")
nostrc_describe_test(FipsCoreCoordCacheTest OWNER libfips-core TARGETS fips_core_coord_cache_test)
# Keep the explicit FIPS CI build closure complete (see the host-test note above).
add_dependencies(fips_core_host_test fips_core_coord_cache_test)

# Compile the same ECDH source with hooks in this executable only. The library
# remains uninstrumented; its separate ECDH object is not needed by this target.
add_executable(fips_core_ecdh_test test_ecdh.c fakes/host.c ../src/noise/ecdh.c)
target_link_libraries(fips_core_ecdh_test PRIVATE fips_core
  PkgConfig::FIPS_CORE_SODIUM PkgConfig::FIPS_CORE_SECP256K1)
target_compile_definitions(fips_core_ecdh_test PRIVATE FIPS_TEST_HOOKS FIPS_CORE_STATIC_DEFINE)
set_target_properties(fips_core_ecdh_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_ecdh_test)
apply_sanitizers(fips_core_ecdh_test)
add_test(NAME FipsCoreEcdhTest COMMAND fips_core_ecdh_test)
set_tests_properties(FipsCoreEcdhTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreEcdhTest OWNER libfips-core TARGETS fips_core_ecdh_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/ecdh_vectors.py")
add_dependencies(fips_core_host_test fips_core_ecdh_test)
# Noise AEAD vectors and failure atomicity (nostrc-pwo3h.1).
add_executable(fips_core_aead_test test_aead.c)
target_link_libraries(fips_core_aead_test PRIVATE fips_core PkgConfig::FIPS_CORE_SODIUM)
set_target_properties(fips_core_aead_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_aead_test)
apply_sanitizers(fips_core_aead_test)
add_test(NAME FipsCoreAeadTest COMMAND fips_core_aead_test)
set_tests_properties(FipsCoreAeadTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreAeadTest OWNER libfips-core TARGETS fips_core_aead_test)
add_dependencies(fips_core_host_test fips_core_aead_test)
# Tree coordinate codec and routing helpers (nostrc-82g9n.1).
add_executable(fips_core_coord_test test_coord.c)
target_link_libraries(fips_core_coord_test PRIVATE fips_core)
set_target_properties(fips_core_coord_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_coord_test)
apply_sanitizers(fips_core_coord_test)
add_test(NAME FipsCoreCoordTest COMMAND fips_core_coord_test)
set_tests_properties(FipsCoreCoordTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreCoordTest OWNER libfips-core TARGETS fips_core_coord_test)
add_dependencies(fips_core_host_test fips_core_coord_test)
# Link catalog, session datagram and framing math (nostrc-82g9n.6). The test also
# replays the pinned proto fixture through the harness loader, so it links the
# test-only loader/schema and Jansson exactly as fips_diff does and reuses its
# vectors/pin definitions rather than a second hand-maintained constant.
pkg_check_modules(FIPS_LINK_JANSSON REQUIRED IMPORTED_TARGET jansson)
add_executable(fips_core_link_test test_link.c diff/loader.c diff/schema.c diff/proto_link.c fakes/host.c)
get_target_property(fips_link_diff_definitions fips_diff COMPILE_DEFINITIONS)
target_compile_definitions(fips_core_link_test PRIVATE ${fips_link_diff_definitions})
target_link_libraries(fips_core_link_test PRIVATE fips_core PkgConfig::FIPS_LINK_JANSSON OpenSSL::Crypto)
set_target_properties(fips_core_link_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_link_test)
apply_sanitizers(fips_core_link_test)
add_test(NAME FipsCoreLinkTest COMMAND fips_core_link_test)
set_tests_properties(FipsCoreLinkTest PROPERTIES TIMEOUT 30 LABELS "fips;unit")
nostrc_describe_test(FipsCoreLinkTest OWNER libfips-core TARGETS fips_core_link_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto.json"
         "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto-session-datagram.bin"
         "${CMAKE_CURRENT_SOURCE_DIR}/../UPSTREAM.md")
add_dependencies(fips_core_host_test fips_core_link_test)

# FMP lifecycle upstream ports and C bounded-storage/ingress regressions.
add_executable(fips_core_fmp_test test_fmp.c fakes/host.c)
target_link_libraries(fips_core_fmp_test PRIVATE fips_core)
set_target_properties(fips_core_fmp_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_fmp_test)
apply_sanitizers(fips_core_fmp_test)
add_test(NAME FipsCoreFmpTest COMMAND fips_core_fmp_test)
set_tests_properties(FipsCoreFmpTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreFmpTest OWNER libfips-core TARGETS fips_core_fmp_test)

add_executable(fips_core_fmp_vectors_test test_fmp_vectors.c
  diff/loader.c diff/schema.c diff/proto_fmp.c diff/proto_link.c fakes/host.c)
get_target_property(fips_fmp_diff_definitions fips_diff COMPILE_DEFINITIONS)
target_compile_definitions(fips_core_fmp_vectors_test PRIVATE ${fips_fmp_diff_definitions})
target_link_libraries(fips_core_fmp_vectors_test PRIVATE
  fips_core PkgConfig::FIPS_LINK_JANSSON OpenSSL::Crypto)
set_target_properties(fips_core_fmp_vectors_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_fmp_vectors_test)
apply_sanitizers(fips_core_fmp_vectors_test)
add_test(NAME FipsCoreFmpVectorsTest COMMAND fips_core_fmp_vectors_test)
set_tests_properties(FipsCoreFmpVectorsTest PROPERTIES TIMEOUT 30 LABELS "fips;diff")
file(GLOB fmp_vector_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/*")
nostrc_describe_test(FipsCoreFmpVectorsTest OWNER libfips-core TARGETS fips_core_fmp_vectors_test
  INPUTS ${fmp_vector_inputs} "${CMAKE_CURRENT_SOURCE_DIR}/../UPSTREAM.md")
add_dependencies(fips_core_host_test fips_core_fmp_test fips_core_fmp_vectors_test)
