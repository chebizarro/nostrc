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
