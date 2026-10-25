cmake_minimum_required(VERSION 3.22)
project(libblossom C)
include(GNUInstallDirs)
include(GenerateExportHeader)
include(CMakePackageConfigHelpers)
if(NOT COMMAND declare_component_version)
  include("${CMAKE_CURRENT_LIST_DIR}/../cmake/VersionHelpers.cmake")
endif()
if(NOT TARGET nostrc::bud02)
  message(FATAL_ERROR "libblossom requires bud02. Set ENABLE_BUD02=ON or BUILD_LIBBLOSSOM=OFF.")
endif()
find_package(CURL 7.85 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(BLOSSOM_JSON REQUIRED IMPORTED_TARGET jansson)
add_library(blossom src/client.c src/shim.c src/capability.c src/pool.c src/types.c)
add_library(nostrc::blossom ALIAS blossom)
target_compile_features(blossom PRIVATE c_std_11)
target_compile_options(blossom PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(blossom
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE src)
target_link_libraries(blossom PRIVATE nostrc::bud02 CURL::libcurl OpenSSL::SSL OpenSSL::Crypto
  Threads::Threads PkgConfig::BLOSSOM_JSON)
generate_export_header(blossom EXPORT_MACRO_NAME BLOSSOM_API
  EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/blossom/export.h")
if(NOT BUILD_SHARED_LIBS)
  target_compile_definitions(blossom PUBLIC BLOSSOM_STATIC_DEFINE)
endif()
set_target_properties(blossom PROPERTIES C_VISIBILITY_PRESET hidden)
declare_component_version(blossom 0 2 0)
set_target_properties(blossom PROPERTIES
  COMPONENT_VERSION_HEADER_INSTALL_DIR "${CMAKE_INSTALL_INCLUDEDIR}/blossom")
apply_versioning(blossom)
if(COMMAND apply_sanitizers)
  apply_sanitizers(blossom)
endif()
# libnostrgo.pc names nsync but older installations omit its nonstandard
# system search directory. Preserve the resolved external dependency location;
# vendored nsync installs alongside our own libraries and needs no extra path.
set(BLOSSOM_NSYNC_SEARCH "")
if(IS_ABSOLUTE "${NSYNC_LIB}")
  get_filename_component(_blossom_nsync_dir "${NSYNC_LIB}" DIRECTORY)
  set(BLOSSOM_NSYNC_SEARCH "-L\"${_blossom_nsync_dir}\"")
endif()
configure_file(blossom.pc.in blossom.pc @ONLY)
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/../cmake/nostrcConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/nostrcConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/nostrc")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/nostrcConfigVersion.cmake"
  VERSION "${blossom_VERSION}" COMPATIBILITY SameMajorVersion)
install(TARGETS blossom ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/blossom DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/blossom/export.h"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/blossom)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/blossom.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostrcConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/nostrcConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nostrc)
option(BLOSSOM_BUILD_LIVE_PROBE "Build the opt-in live Blossom probe" OFF)
if(BLOSSOM_BUILD_LIVE_PROBE)
  add_executable(blossom-probe-capabilities-live tools/probe_capabilities_live.c)
  target_link_libraries(blossom-probe-capabilities-live PRIVATE nostrc::blossom)
endif()
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(blossom-transport-driver tests/transport_driver.c)
  target_link_libraries(blossom-transport-driver PRIVATE nostrc::blossom nostrc::bud02 OpenSSL::Crypto)
  if(COMMAND apply_sanitizers)
    apply_sanitizers(blossom-transport-driver)
  endif()
  add_test(NAME test-blossom-transport COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_transport.py $<TARGET_FILE:blossom-transport-driver>)
  set_tests_properties(test-blossom-transport PROPERTIES TIMEOUT 120 LABELS "libblossom;integration")
  foreach(test IN ITEMS client batch content_type shim shim_auto server_capability_probe_shim guards)
    string(REPLACE "_" "-" name "${test}")
    if(test STREQUAL "server_capability_probe_shim")
      set(source tests/test_server_capability_probe_shim.c)
    else()
      set(source "tests/test_blossom_${test}.c")
    endif()
    add_executable(test-blossom-${name} ${source})
    target_link_libraries(test-blossom-${name} PRIVATE nostrc::blossom nostrc::bud02 OpenSSL::Crypto Threads::Threads)
    add_test(NAME test-blossom-${name} COMMAND test-blossom-${name})
    set_tests_properties(test-blossom-${name} PROPERTIES TIMEOUT 90 LABELS "libblossom;unit")
    if(COMMAND apply_sanitizers)
      apply_sanitizers(test-blossom-${name})
    endif()
  endforeach()
endif()
