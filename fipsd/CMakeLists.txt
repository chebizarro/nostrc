cmake_minimum_required(VERSION 3.22)
project(fipsd C)

declare_component_version(FIPSD 0 1 0)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FIPSD_SODIUM REQUIRED IMPORTED_TARGET libsodium)
add_executable(fipsd src/main.c src/host_os.c)
set_target_properties(fipsd PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(fipsd PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include" "${FIPSD_VERSION_HEADER_DIR}")
target_link_libraries(fipsd PRIVATE fips_core libgo go_fiber nostr PkgConfig::FIPSD_SODIUM)
apply_warnings(fipsd)
apply_sanitizers(fipsd)
install(TARGETS fipsd RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
