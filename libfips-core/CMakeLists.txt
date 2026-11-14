cmake_minimum_required(VERSION 3.22)
project(libfips-core C)

declare_component_version(fips_core 0 1 0)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FIPS_CORE_SECP256K1 REQUIRED IMPORTED_TARGET libsecp256k1)
pkg_check_modules(FIPS_CORE_SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

# Follow BUILD_SHARED_LIBS, as the other nostrc libraries do.
add_library(fips_core src/core.c src/host.c)
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
apply_sanitizers(fips_core)

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

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
