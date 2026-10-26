include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcVisibility.cmake")

project(nip45)

include_directories(
  include
  ${CMAKE_SOURCE_DIR}/libnostr/include
)

add_library(nip45 src/nip45.c)
nostrc_library_visibility(nip45 NOSTR_NIP45_API
  nostr/nip45/export.h "${CMAKE_INSTALL_INCLUDEDIR}/nostr/nip45")

target_link_libraries(nip45 PUBLIC nostr)
