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
