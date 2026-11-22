include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcVisibility.cmake")
project(nip78 VERSION 0.1.0 LANGUAGES C)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)

add_library(nostr_nip78_core src/nip78.c)
add_library(nostr_nip78 ALIAS nostr_nip78_core)
nostrc_library_visibility(nostr_nip78_core NOSTR_NIP78_API
  nostr/nip78/export.h "${CMAKE_INSTALL_INCLUDEDIR}/nostr/nip78")
set_target_properties(nostr_nip78_core PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)
target_include_directories(nostr_nip78_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(nostr_nip78_core PRIVATE ${CMAKE_SOURCE_DIR}/nips)
target_link_libraries(nostr_nip78_core PUBLIC nostr PRIVATE PkgConfig::JANSSON)

install(TARGETS nostr_nip78_core
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
configure_file(nip78.pc.in nip78.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nip78.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(BUILD_TESTING)
  add_executable(test_nip78 tests/test_nip78.c)
  target_link_libraries(test_nip78 PRIVATE nostr_nip78_core)
  add_test(NAME test_nip78 COMMAND test_nip78)
  if(TARGET nostr_nip44_core)
    add_executable(test_nip78_signer tests/test_nip78_signer.c)
    target_link_libraries(test_nip78_signer PRIVATE nostr_nip78_core nostr_nip44_core)
    add_test(NAME test_nip78_signer COMMAND test_nip78_signer)
  endif()
endif()
