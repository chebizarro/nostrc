add_test(NAME FipsdSkeletonTest
  COMMAND "${CMAKE_COMMAND}"
    "-DFIPSD=$<TARGET_FILE:fipsd>"
    "-DEXPECTED_VERSION=${FIPSD_VERSION}"
    "-DEXPECTED_CORE_VERSION=${fips_core_VERSION}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/smoke.cmake")
set_tests_properties(FipsdSkeletonTest PROPERTIES TIMEOUT 15 LABELS "fips;smoke"
  ENVIRONMENT "GOF_NWORKERS=1")
nostrc_describe_test(FipsdSkeletonTest OWNER fipsd TARGETS fipsd
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/smoke.cmake")

add_executable(fipsd_host_os_test test_host_os.c ../src/host_os.c)
target_include_directories(fipsd_host_os_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(fipsd_host_os_test PRIVATE fips_core PkgConfig::FIPSD_SODIUM)
set_target_properties(fipsd_host_os_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fipsd_host_os_test)
apply_sanitizers(fipsd_host_os_test)
add_test(NAME FipsdHostOsTest COMMAND fipsd_host_os_test)
set_tests_properties(FipsdHostOsTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsdHostOsTest OWNER fipsd TARGETS fipsd_host_os_test)
