
project(nip19)

if(NOT COMMAND declare_component_version)
  include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/VersionHelpers.cmake")
endif()
include(GNUInstallDirs)

# Include the header files
include_directories(include ${CMAKE_SOURCE_DIR}/libnostr/include)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Create the library
add_library(nip19 ${SOURCES})
target_link_libraries(nip19 nostr)

# nostrc-4c0o: a real SONAME (libnip19.so.0 -> libnip19.so.0.1.0), mirroring
# libnostr, so the shared build can ship once in libnostr-nips0 (plan D-6)
# with ABI-based dependencies instead of a private copy per consumer.
# Pre-1.0: bump the minor for additions, SOVERSION for any ABI break.
declare_component_version(nip19 0 1 0)
set_target_properties(nip19 PROPERTIES
  COMPONENT_VERSION_HEADER_INSTALL_DIR "${CMAKE_INSTALL_INCLUDEDIR}/nostr")
apply_versioning(nip19)
target_include_directories(nip19
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/nostr>
)

# Tests
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip19.c)
    add_executable(test_nip19 tests/test_nip19.c)
    target_link_libraries(test_nip19 PRIVATE nip19 nostr)
    add_test(NAME test_nip19 COMMAND test_nip19)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip19_tlv.c)
    add_executable(test_nip19_tlv tests/test_nip19_tlv.c)
    target_link_libraries(test_nip19_tlv PRIVATE nip19 nostr)
    add_test(NAME test_nip19_tlv COMMAND test_nip19_tlv)
endif()

# Examples
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/nrelay_multi_example.c)
    add_executable(nrelay_multi_example examples/nrelay_multi_example.c)
    target_link_libraries(nrelay_multi_example PRIVATE nip19 nostr)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/example.c)
    add_executable(nip19_examples examples/example.c)
    target_link_libraries(nip19_examples PRIVATE nip19 nostr)
endif()

# Install rules
install(TARGETS nip19
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr
  FILES_MATCHING PATTERN "*.h")

