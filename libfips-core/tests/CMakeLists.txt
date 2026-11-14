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
