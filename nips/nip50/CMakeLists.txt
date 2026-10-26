include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcVisibility.cmake")

project(nip50)

include_directories(
  include
  ${CMAKE_SOURCE_DIR}/libnostr/include
)

add_library(nip50 src/nip50.c)
nostrc_library_visibility(nip50 NOSTR_NIP50_API
  nostr/nip50/export.h "${CMAKE_INSTALL_INCLUDEDIR}/nostr/nip50")

target_link_libraries(nip50 PUBLIC nostr)
