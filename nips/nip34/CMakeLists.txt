project(nip34 C)

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
include(GNUInstallDirs)
install(TARGETS nip34
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr
  FILES_MATCHING PATTERN "*.h")
