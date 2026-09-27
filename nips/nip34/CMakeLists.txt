project(nip34 C)

if(NOT COMMAND declare_component_version)
  include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/VersionHelpers.cmake")
endif()
include(GNUInstallDirs)

# NIP-34: Git Stuff — Nostr events for Git collaboration

file(GLOB NIP34_SOURCES "src/*.c")

add_library(nip34 ${NIP34_SOURCES})
target_compile_features(nip34 PRIVATE c_std_11)

target_include_directories(nip34
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(nip34 PUBLIC nostr)

# nostrc-4c0o: real SONAME (libnip34.so.0), shipped once in libnostr-nips0
# (plan D-6) instead of privately inside libhanami0. Same policy as nip19.
declare_component_version(nip34 0 1 0)
set_target_properties(nip34 PROPERTIES
  COMPONENT_VERSION_HEADER_INSTALL_DIR "${CMAKE_INSTALL_INCLUDEDIR}/nostr")
apply_versioning(nip34)

# Tests
if(BUILD_TESTING)
  file(GLOB NIP34_TEST_SOURCES "tests/*.c")
  if(NIP34_TEST_SOURCES)
    add_executable(test-nip34 ${NIP34_TEST_SOURCES})
    target_compile_features(test-nip34 PRIVATE c_std_11)
    target_link_libraries(test-nip34 PRIVATE nip34 nostr)
    add_test(NAME test-nip34 COMMAND test-nip34)
    set_tests_properties(test-nip34 PROPERTIES
      TIMEOUT 30
      LABELS "nip34;unit"
    )
  endif()
endif()

# Install rules (added for nostrc-y4ye: without this the .so was built
# but not installed to debian/tmp/, so dpkg-shlibdeps couldn't resolve
# libnip34 for its consumers (libhanami0, nostr-homed-provision) and
# the packaged .deb was silently broken on aarch64).
install(TARGETS nip34
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr
  FILES_MATCHING PATTERN "*.h")
