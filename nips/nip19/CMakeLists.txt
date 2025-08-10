
project(nip19)

# Include the header files
include_directories(include ${CMAKE_SOURCE_DIR}/libnostr/include)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Create the library
add_library(nip19 ${SOURCES})
target_link_libraries(nip19 nostr)
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
include(GNUInstallDirs)
install(TARGETS nip19
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr
  FILES_MATCHING PATTERN "*.h")

