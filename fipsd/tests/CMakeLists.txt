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
