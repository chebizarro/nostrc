get_filename_component(_nh_unit_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NH_UNIT_JANSSON REQUIRED IMPORTED_TARGET jansson)
find_package(SQLite3 REQUIRED)
if(NOT TARGET SQLite3::SQLite3)
  add_library(SQLite3::SQLite3 INTERFACE IMPORTED)
  target_include_directories(SQLite3::SQLite3 INTERFACE ${SQLite3_INCLUDE_DIRS})
  target_link_libraries(SQLite3::SQLite3 INTERFACE ${SQLite3_LIBRARIES})
endif()
find_package(OpenSSL REQUIRED)

add_executable(test_manifest
  ${_nh_unit_root}/tests/unit/test_manifest.c ${_nh_unit_root}/src/common/nostr_manifest.c)
target_include_directories(test_manifest PRIVATE ${_nh_unit_root}/include)
target_link_libraries(test_manifest PRIVATE PkgConfig::NH_UNIT_JANSSON)
add_test(NAME homed_manifest COMMAND test_manifest)

foreach(nh_cache_unit cache cache_config uid_map groups)
  add_executable(test_${nh_cache_unit}
    ${_nh_unit_root}/tests/unit/test_${nh_cache_unit}.c ${_nh_unit_root}/src/common/nostr_cache.c)
  target_include_directories(test_${nh_cache_unit} PRIVATE ${_nh_unit_root}/include)
  target_link_libraries(test_${nh_cache_unit} PRIVATE SQLite3::SQLite3 OpenSSL::Crypto)
  add_test(NAME homed_${nh_cache_unit} COMMAND test_${nh_cache_unit})
endforeach()
set_tests_properties(homed_manifest homed_cache homed_cache_config homed_uid_map homed_groups
  PROPERTIES LABELS "nostr-homed;portable;manifest-cache" TIMEOUT 60)
