cmake_minimum_required(VERSION 3.22)
project(nostr_bud02 C)

include(GNUInstallDirs)
if(NOT COMMAND declare_component_version)
  include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/VersionHelpers.cmake")
endif()
find_package(OpenSSL REQUIRED)

add_library(bud02 src/auth.c)
add_library(nostrc::bud02 ALIAS bud02)
target_compile_features(bud02 PRIVATE c_std_11)
target_include_directories(bud02
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bud02 PUBLIC nostr PRIVATE OpenSSL::Crypto)

# Shared builds expose only the codec API; static consumers also need that API
# visible when an executable supplies archive definitions to a DSO.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcVisibility.cmake")
nostrc_generate_export_header(bud02 NOSTR_BUD02_API nostr/bud02/export.h)
target_include_directories(bud02 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
set_target_properties(bud02 PROPERTIES C_VISIBILITY_PRESET hidden)
declare_component_version(bud02 0 1 1)
set_target_properties(bud02 PROPERTIES
  COMPONENT_VERSION_HEADER_INSTALL_DIR "${CMAKE_INSTALL_INCLUDEDIR}/nostr/bud02")
apply_versioning(bud02)
if(COMMAND apply_sanitizers)
  apply_sanitizers(bud02)
endif()

configure_file(bud02.pc.in bud02.pc @ONLY)
install(TARGETS bud02
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/nostr/bud02/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr/bud02)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/nostr/bud02/export.h"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr/bud02)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/bud02.pc"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(BUILD_TESTING)
  add_executable(test-bud02-auth tests/test_bud02_auth.c)
  target_compile_features(test-bud02-auth PRIVATE c_std_11)
  target_link_libraries(test-bud02-auth PRIVATE nostrc::bud02)
  add_test(NAME test-bud02-auth COMMAND test-bud02-auth)
  set_tests_properties(test-bud02-auth PROPERTIES TIMEOUT 30 LABELS "bud02;unit")
  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-bud02-auth)
  endif()
endif()
